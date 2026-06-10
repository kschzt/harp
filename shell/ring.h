/* Lock-free SPSC plumbing between the DAW audio thread and the feeder
 * thread. The audio thread side never blocks, allocates, or syscalls
 * (spec §15.1: the shell's audio-thread contract is hard). */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

/* Single-producer single-consumer ring of floats (interleaved audio).
 * Capacity must be a power of two. */
class FloatRing {
public:
    explicit FloatRing(size_t capacity_pow2) : cap_(capacity_pow2), mask_(capacity_pow2 - 1) {
        buf_ = new float[cap_]();
    }
    ~FloatRing() { delete[] buf_; }

    size_t readAvailable() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }
    size_t writeAvailable() const { return cap_ - readAvailable(); }

    /* producer */
    size_t write(const float *src, size_t n) {
        size_t can = writeAvailable();
        if (n > can) n = can;
        size_t h = head_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; i++) buf_[(h + i) & mask_] = src[i];
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    /* consumer; returns floats actually read */
    size_t read(float *dst, size_t n) {
        size_t can = readAvailable();
        if (n > can) n = can;
        size_t t = tail_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; i++) dst[i] = buf_[(t + i) & mask_];
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    const size_t cap_, mask_;
    float *buf_;
    std::atomic<size_t> head_{0}, tail_{0};
};

/* SPSC ring of param changes (audio thread -> feeder). Fixed capacity;
 * overflow drops oldest-style by refusing (caller may retry next block —
 * automation resends anyway). */
struct ParamChange {
    uint32_t id;
    float value;
};

class ParamRing {
public:
    bool push(ParamChange c) { /* producer: audio thread */
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= kCap) return false;
        buf_[h & (kCap - 1)] = c;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(ParamChange &out) { /* consumer: feeder thread */
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t & (kCap - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kCap = 256;
    ParamChange buf_[kCap];
    std::atomic<size_t> head_{0}, tail_{0};
};
