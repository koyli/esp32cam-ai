#include "motion_detect.h"
#include <string.h>

MotionDetector::MotionDetector(uint8_t threshold_pct)
    : threshold_pct_(threshold_pct), has_prev_(false) {
    memset(prev_, 0, sizeof(prev_));
}

bool MotionDetector::update(const camera_fb_t* fb) {
    // TODO: decode JPEG to a GW x GH luma grid (using esp_jpg_decode with an
    // output callback that averages each tile). For now treat every frame as
    // unchanged so the rest of the pipeline can be wired up first.
    (void)fb;
    if (!has_prev_) { has_prev_ = true; return false; }
    return false;
}
