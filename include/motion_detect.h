#pragma once

#include <esp_camera.h>

// Lightweight inter-frame motion check. Decodes each JPEG to a small luma
// grid and compares against the previous grid; returns true when the
// fraction of changed cells exceeds threshold_pct.
class MotionDetector {
public:
    explicit MotionDetector(uint8_t threshold_pct);
    bool update(const camera_fb_t* fb);

private:
    uint8_t threshold_pct_;
    // 16x12 downsampled luma grid from the previous frame.
    static constexpr int GW = 16;
    static constexpr int GH = 12;
    uint8_t prev_[GW * GH];
    bool has_prev_;
};
