#pragma once

#include <esp_camera.h>

// Lightweight inter-frame motion check. Decodes each JPEG to a small luma
// grid and compares against the previous grid; returns true when the
// fraction of changed cells exceeds threshold_pct.
class MotionDetector {
public:
    static constexpr int GW = 16;
    static constexpr int GH = 12;

    explicit MotionDetector(uint8_t threshold_pct);
    bool update(const camera_fb_t* fb);

private:
    uint8_t threshold_pct_;
    uint8_t prev_[GW * GH];
    bool has_prev_;
};
