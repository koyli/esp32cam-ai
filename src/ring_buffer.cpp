#include "ring_buffer.h"
#include <esp_heap_caps.h>

JpegRingBuffer::JpegRingBuffer(size_t capacity)
    : capacity_(capacity), head_(0), count_(0) {
    slots_ = new Slot[capacity_]();
}

JpegRingBuffer::~JpegRingBuffer() {
    for (size_t i = 0; i < capacity_; ++i) {
        if (slots_[i].buf) heap_caps_free(slots_[i].buf);
    }
    delete[] slots_;
}

void JpegRingBuffer::push(const camera_fb_t* fb) {
    Slot& s = slots_[head_];
    if (s.cap < fb->len) {
        if (s.buf) heap_caps_free(s.buf);
        s.buf = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
        s.cap = s.buf ? fb->len : 0;
    }
    if (s.buf) {
        memcpy(s.buf, fb->buf, fb->len);
        s.len = fb->len;
        s.t_ms = millis();
    }
    head_ = (head_ + 1) % capacity_;
    if (count_ < capacity_) count_++;
}

const uint8_t* JpegRingBuffer::data(size_t i) const {
    size_t start = (count_ == capacity_) ? head_ : 0;
    size_t idx = (start + i) % capacity_;
    return slots_[idx].buf;
}

size_t JpegRingBuffer::len(size_t i) const {
    size_t start = (count_ == capacity_) ? head_ : 0;
    size_t idx = (start + i) % capacity_;
    return slots_[idx].len;
}

uint32_t JpegRingBuffer::time(size_t i) const {
    size_t start = (count_ == capacity_) ? head_ : 0;
    size_t idx = (start + i) % capacity_;
    return slots_[idx].t_ms;
}
