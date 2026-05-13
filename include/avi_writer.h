#pragma once

#include <Arduino.h>
#include <FS.h>

// Writes a minimal MJPEG-in-AVI file to the SD card. Frames are JPEG payloads.
class AviWriter {
public:
    bool begin(const char* path, uint16_t width, uint16_t height, uint8_t fps);
    bool writeFrame(const uint8_t* jpeg, size_t len);
    bool end();

private:
    File file_;
    uint32_t frame_count_ = 0;
    uint32_t movi_size_ = 0;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint8_t  fps_ = 0;
};
