#include "avi_writer.h"
#include <SD_MMC.h>

// Minimal MJPEG-in-AVI writer. Header sizes are patched on end().

namespace {
constexpr size_t HDR_SIZE = 224;        // bytes reserved before first frame
}

bool AviWriter::begin(const char* path, uint16_t w, uint16_t h) {
    file_ = SD_MMC.open(path, FILE_WRITE);
    if (!file_) return false;
    width_ = w; height_ = h;
    frame_count_ = 0; movi_size_ = 0;
    frame_sizes_.clear();
    frame_sizes_.reserve(128);

    // Reserve space for the header — finalised in end().
    uint8_t zeros[HDR_SIZE] = {0};
    file_.write(zeros, HDR_SIZE);
    // 'movi' LIST marker is written as part of the header in end(); body starts here.
    return true;
}

bool AviWriter::writeFrame(const uint8_t* jpeg, size_t len) {
    if (!file_) return false;
    uint8_t chunk[8] = {'0','0','d','c', 0,0,0,0};
    chunk[4] = (uint8_t)(len & 0xff);
    chunk[5] = (uint8_t)((len >> 8) & 0xff);
    chunk[6] = (uint8_t)((len >> 16) & 0xff);
    chunk[7] = (uint8_t)((len >> 24) & 0xff);
    file_.write(chunk, 8);
    file_.write(jpeg, len);
    if (len & 1) { uint8_t pad = 0; file_.write(&pad, 1); }
    movi_size_ += 8 + len + (len & 1);
    frame_count_++;
    frame_sizes_.push_back((uint32_t)len);
    return true;
}

bool AviWriter::end(uint32_t us_per_frame) {
    if (!file_) return false;

    auto writeFourCC = [&](const char* s) {
        file_.write(reinterpret_cast<const uint8_t*>(s), 4);
    };
    auto writeU32 = [&](uint32_t v) {
        uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24) };
        file_.write(b, 4);
    };
    auto writeU16 = [&](uint16_t v) {
        uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
        file_.write(b, 2);
    };

    // idx1 index after the movi list, so players can seek. Offsets are relative
    // to the 'movi' fourcc; the first chunk header follows it at offset 4.
    writeFourCC("idx1");
    writeU32(16 * (uint32_t)frame_sizes_.size());
    uint32_t offset = 4;
    for (uint32_t sz : frame_sizes_) {
        writeFourCC("00dc");
        writeU32(0x10);                   // AVIIF_KEYFRAME (every MJPEG frame is)
        writeU32(offset);
        writeU32(sz);
        offset += 8 + sz + (sz & 1);
    }
    frame_sizes_.clear();

    const uint32_t file_size = file_.position();
    if (!file_.seek(0)) { file_.close(); return false; }

    if (!us_per_frame) us_per_frame = 100000UL;
    const uint32_t suggested_buf     = 100000;
    const uint32_t max_bytes_per_sec = (uint32_t)((uint64_t)suggested_buf * 1000000UL / us_per_frame);

    // RIFF
    writeFourCC("RIFF");
    writeU32(file_size - 8);
    writeFourCC("AVI ");

    // LIST 'hdrl' — contains avih + LIST 'strl'.
    // 4 ('hdrl') + 64 (avih) + 124 (LIST strl) = 192
    writeFourCC("LIST");
    writeU32(192);
    writeFourCC("hdrl");

    // avih
    writeFourCC("avih"); writeU32(56);
    writeU32(us_per_frame);
    writeU32(max_bytes_per_sec);
    writeU32(0);                      // padding granularity
    writeU32(0x10);                   // flags: AVIF_HASINDEX
    writeU32(frame_count_);
    writeU32(0);                      // initial frames
    writeU32(1);                      // streams
    writeU32(suggested_buf);
    writeU32(width_);
    writeU32(height_);
    writeU32(0); writeU32(0); writeU32(0); writeU32(0);  // reserved[4]

    // LIST 'strl' — contains strh + strf.
    // 4 ('strl') + 64 (strh) + 48 (strf) = 116
    writeFourCC("LIST");
    writeU32(116);
    writeFourCC("strl");

    // strh
    writeFourCC("strh"); writeU32(56);
    writeFourCC("vids");
    writeFourCC("MJPG");
    writeU32(0);                      // flags
    writeU16(0);                      // priority
    writeU16(0);                      // language
    writeU32(0);                      // initial frames
    writeU32(us_per_frame);           // scale
    writeU32(1000000UL);              // rate -> fps = rate/scale
    writeU32(0);                      // start
    writeU32(frame_count_);           // length
    writeU32(suggested_buf);
    writeU32(0xFFFFFFFFUL);           // quality
    writeU32(0);                      // sample size
    writeU16(0); writeU16(0); writeU16(width_); writeU16(height_);  // rcFrame

    // strf (BITMAPINFOHEADER)
    writeFourCC("strf"); writeU32(40);
    writeU32(40);                     // biSize
    writeU32(width_);
    writeU32(height_);
    writeU16(1);                      // planes
    writeU16(24);                     // bit count
    writeFourCC("MJPG");              // compression
    writeU32(uint32_t(width_) * uint32_t(height_) * 3);
    writeU32(0); writeU32(0);         // pels per meter
    writeU32(0); writeU32(0);         // clr used / important

    // LIST 'movi' — header for the frame chunks that follow on disk.
    writeFourCC("LIST");
    writeU32(4 + movi_size_);
    writeFourCC("movi");

    file_.flush();
    file_.close();
    return true;
}
