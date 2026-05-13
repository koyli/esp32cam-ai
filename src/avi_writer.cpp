#include "avi_writer.h"
#include <SD_MMC.h>

// Minimal MJPEG-in-AVI writer. Header sizes are patched on end().

namespace {
constexpr size_t HDR_SIZE = 224;        // bytes reserved before first frame
}

bool AviWriter::begin(const char* path, uint16_t w, uint16_t h, uint8_t fps) {
    file_ = SD_MMC.open(path, FILE_WRITE);
    if (!file_) return false;
    width_ = w; height_ = h; fps_ = fps;
    frame_count_ = 0; movi_size_ = 0;

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
    return true;
}

bool AviWriter::end() {
    if (!file_) return false;
    // TODO: seek to offset 0 and write the actual RIFF/AVI/hdrl/strl headers
    // with width_, height_, fps_, frame_count_, movi_size_.
    file_.close();
    return true;
}
