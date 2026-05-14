#include "motion_detect.h"
#include <esp_jpg_decode.h>
#include <string.h>

static constexpr uint8_t LUMA_STEP = 16;

struct DecodeCtx {
    const uint8_t* src;
    size_t         src_len;
    uint32_t       sums[MotionDetector::GW * MotionDetector::GH];
    uint32_t       counts[MotionDetector::GW * MotionDetector::GH];
    uint16_t       img_w;
    uint16_t       img_h;
};

static size_t jpegReader(void* arg, size_t index, uint8_t* buf, size_t len) {
    auto* ctx = (DecodeCtx*)arg;
    if (index >= ctx->src_len) return 0;
    size_t n = ctx->src_len - index;
    if (n > len) n = len;
    if (buf) memcpy(buf, ctx->src + index, n);
    return n;
}

// Callback receives RGB888 blocks at (x,y) of size (w,h).
// Maps each pixel into the GW×GH luma grid and accumulates.
static bool jpegWriter(void* arg, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* data) {
    auto* ctx = (DecodeCtx*)arg;
    if (!data || ctx->img_w == 0 || ctx->img_h == 0) return true;

    constexpr int GW = MotionDetector::GW;
    constexpr int GH = MotionDetector::GH;

    for (uint16_t py = 0; py < h; ++py) {
        for (uint16_t px = 0; px < w; ++px) {
            const uint8_t* rgb = data + (py * w + px) * 3;
            uint8_t luma = (uint8_t)((77u * rgb[0] + 150u * rgb[1] + 29u * rgb[2]) >> 8);
            int col = (int)(x + px) * GW / ctx->img_w;
            int row = (int)(y + py) * GH / ctx->img_h;
            if ((unsigned)col < (unsigned)GW && (unsigned)row < (unsigned)GH) {
                ctx->sums [row * GW + col] += luma;
                ctx->counts[row * GW + col]++;
            }
        }
    }
    return true;
}

MotionDetector::MotionDetector(uint8_t threshold_pct)
    : threshold_pct_(threshold_pct), has_prev_(false) {
    memset(prev_, 0, sizeof(prev_));
}

bool MotionDetector::update(const camera_fb_t* fb) {
    DecodeCtx ctx = {};
    ctx.src     = fb->buf;
    ctx.src_len = fb->len;
    ctx.img_w   = (uint16_t)fb->width;
    ctx.img_h   = (uint16_t)fb->height;

    if (esp_jpg_decode(fb->len, JPG_SCALE_8X, jpegReader, jpegWriter, &ctx) != ESP_OK) {
        return false;
    }

    uint8_t cur[GW * GH];
    for (int i = 0; i < GW * GH; ++i) {
        cur[i] = ctx.counts[i] ? (uint8_t)(ctx.sums[i] / ctx.counts[i]) : 0;
    }

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
