#pragma once

#include <Arduino.h>
#include <esp_camera.h>

// Fixed-capacity ring of JPEG frames held in PSRAM. Owns the buffers.
class JpegRingBuffer {
public:
    explicit JpegRingBuffer(size_t capacity);
    ~JpegRingBuffer();

    // Copies fb->buf into the ring. Oldest entry is evicted when full.
    void push(const camera_fb_t* fb);
    // Drops all frames (buffers are kept for reuse).
    void clear() { head_ = 0; count_ = 0; }

    size_t size() const { return count_; }
    size_t capacity() const { return capacity_; }

    // Returns frame at logical index (0 = oldest). Pointer valid until next push().
    const uint8_t* data(size_t i) const;
    size_t len(size_t i) const;

private:
    struct Slot { uint8_t* buf; size_t len; size_t cap; };
    Slot* slots_;
    size_t capacity_;
    size_t head_;   // next write
    size_t count_;
};
