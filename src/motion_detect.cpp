#include "motion_detect.h"
#include <img_converters.h>
#include <esp_heap_caps.h>
#include <string.h>

// JPEG_DIV_8 on a 640x480 frame → 80x60 RGB565 pixels (~9.6 KB).
static constexpr int SCALED_W = 80;
static constexpr int SCALED_H = 60;

// Block size mapping SCALED → GW x GH grid.
static constexpr int BW = SCALED_W / MotionDetector::GW;  // 5
static constexpr int BH = SCALED_H / MotionDetector::GH;  // 5

// Per-cell luma change that counts as motion (~6% of the 0-255 range).
static constexpr uint8_t LUMA_STEP = 16;

MotionDetector::MotionDetector(uint8_t threshold_pct)
    : threshold_pct_(threshold_pct), has_prev_(false) {
    memset(prev_, 0, sizeof(prev_));
}

static uint8_t luma565(uint16_t px) {
    uint8_t r5 = (px >> 11) & 0x1f;
    uint8_t g6 = (px >> 5)  & 0x3f;
    uint8_t b5 =  px        & 0x1f;
    // Expand to 8-bit.
    uint16_t r = (r5 << 3) | (r5 >> 2);
    uint16_t g = (g6 << 2) | (g6 >> 4);
    uint16_t b = (b5 << 3) | (b5 >> 2);
    // BT.601 luma: Y = 0.299R + 0.587G + 0.114B  (integer approximation).
    return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

bool MotionDetector::update(const camera_fb_t* fb) {
    auto* scaled = (uint16_t*)heap_caps_malloc(SCALED_W * SCALED_H * 2, MALLOC_CAP_SPIRAM);
    if (!scaled) return false;

    if (!jpg2rgb565(fb->buf, fb->len, (uint8_t*)scaled, JPEG_DIV_8)) {
        heap_caps_free(scaled);
        return false;
    }

    // Downsample SCALED_W×SCALED_H → GW×GH by averaging BW×BH pixel blocks.
    uint8_t cur[GW * GH];
    for (int row = 0; row < GH; ++row) {
        for (int col = 0; col < GW; ++col) {
            uint32_t sum = 0;
            for (int by = 0; by < BH; ++by)
                for (int bx = 0; bx < BW; ++bx)
                    sum += luma565(scaled[(row * BH + by) * SCALED_W + (col * BW + bx)]);
            cur[row * GW + col] = (uint8_t)(sum / (BW * BH));
        }
    }
    heap_caps_free(scaled);

    if (!has_prev_) {
        memcpy(prev_, cur, sizeof(cur));
        has_prev_ = true;
        return false;
    }

    int changed = 0;
    for (int i = 0; i < GW * GH; ++i) {
        int diff = (int)cur[i] - (int)prev_[i];
        if (diff < 0) diff = -diff;
        if (diff > LUMA_STEP) changed++;
    }
    memcpy(prev_, cur, sizeof(cur));

    int threshold_cells = (GW * GH * threshold_pct_) / 100;
    return changed >= threshold_cells;
}
