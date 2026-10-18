#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

// Writes a minimal MJPEG-in-AVI file to the SD card. Frames are JPEG payloads.
class AviWriter {
public:
    bool begin(const char* path, uint16_t width, uint16_t height);
    bool writeFrame(const uint8_t* jpeg, size_t len);
    // us_per_frame is the measured average frame interval; it sets the playback rate.
    bool end(uint32_t us_per_frame);

private:
    File file_;
    uint32_t frame_count_ = 0;
    uint32_t movi_size_ = 0;
    std::vector<uint32_t> frame_sizes_;  // for the idx1 index
    uint16_t width_ = 0;
    uint16_t height_ = 0;
};
