#include "motion_detect.h"
#include <jpeg_decoder.h>
#include <stdlib.h>
#include <string.h>

static constexpr uint8_t LUMA_STEP    = 16;
static constexpr uint8_t MIN_LUMA     = 10;  // frames below this mean are treated as dark/invalid
static constexpr uint8_t GLOBAL_SHIFT = 20;  // if mean luma shifts by more than this, likely auto-exposure

// Decodes a JPEG at 1/8 scale to RGB888 and averages its luma over the
// GW×GH grid. The RGB buffer is kept between calls (about 14 KB at VGA).
static bool decodeLumaGrid(const camera_fb_t* fb, uint8_t* grid) {
    constexpr int GW = MotionDetector::GW;
    constexpr int GH = MotionDetector::GH;

    static uint8_t* rgb     = nullptr;
    static size_t   rgb_cap = 0;
    size_t need = (size_t)((fb->width + 7) / 8) * ((fb->height + 7) / 8) * 3;
    if (need > rgb_cap) {
        free(rgb);
        rgb     = (uint8_t*)malloc(need);
        rgb_cap = rgb ? need : 0;
        if (!rgb) return false;
    }

    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata      = fb->buf;
    cfg.indata_size = fb->len;
    cfg.outbuf      = rgb;
    cfg.outbuf_size = rgb_cap;
    cfg.out_format  = JPEG_IMAGE_FORMAT_RGB888;
    cfg.out_scale   = JPEG_IMAGE_SCALE_1_8;
    esp_jpeg_image_output_t img = {};
    if (esp_jpeg_decode(&cfg, &img) != ESP_OK || img.width == 0 || img.height == 0) return false;

    uint32_t sums[GW * GH]   = {};
    uint32_t counts[GW * GH] = {};
    for (int y = 0; y < img.height; ++y) {
        int row = y * GH / img.height;
        const uint8_t* p = rgb + (size_t)y * img.width * 3;
        for (int x = 0; x < img.width; ++x, p += 3) {
            uint8_t luma = (uint8_t)((77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8);
            int cell = row * GW + x * GW / img.width;
            sums[cell] += luma;
            counts[cell]++;
        }
    }
    for (int i = 0; i < GW * GH; ++i) {
        grid[i] = counts[i] ? (uint8_t)(sums[i] / counts[i]) : 0;
    }
    return true;
}

MotionDetector::MotionDetector(uint8_t threshold_pct)
    : threshold_pct_(threshold_pct), has_prev_(false) {
    memset(prev_, 0, sizeof(prev_));
}

int MotionDetector::update(const camera_fb_t* fb) {
    uint8_t cur[GW * GH];
    if (!decodeLumaGrid(fb, cur)) return -1;

    // Compute mean luma of current frame.
    uint32_t cur_sum = 0;
    for (int i = 0; i < GW * GH; ++i) cur_sum += cur[i];
    uint8_t cur_mean = (uint8_t)(cur_sum / (GW * GH));

    // Reject dark frames — don't update prev_ so we compare against last valid frame.
    if (cur_mean < MIN_LUMA) return 0;

    if (!has_prev_) {
        memcpy(prev_, cur, sizeof(cur));
        has_prev_ = true;
        return 0;
    }

    // Reject global illumination shifts (auto-exposure jump).
    uint32_t prev_sum = 0;
    for (int i = 0; i < GW * GH; ++i) prev_sum += prev_[i];
    uint8_t prev_mean = (uint8_t)(prev_sum / (GW * GH));
    int global_shift = (int)cur_mean - (int)prev_mean;
    if (global_shift < 0) global_shift = -global_shift;
    if (global_shift > GLOBAL_SHIFT) {
        memcpy(prev_, cur, sizeof(cur));
        return 0;
    }

    int changed = 0;
    for (int i = 0; i < GW * GH; ++i) {
        int diff = (int)cur[i] - (int)prev_[i];
        if (diff < 0) diff = -diff;
        if (diff > LUMA_STEP) changed++;
    }
    memcpy(prev_, cur, sizeof(cur));
    return changed;
}

bool MotionDetector::triggered(int changed_cells) const {
    return changed_cells >= thresholdCells();
}
