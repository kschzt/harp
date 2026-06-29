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
    /* owns a heap buffer + atomic cursors -> non-copyable by construction (the atomics already
     * delete the implicit copy); make it explicit so the rule-of-3 is unambiguous (no double-free). */
    FloatRing(const FloatRing &) = delete;
    FloatRing &operator=(const FloatRing &) = delete;

    /* consumer-side (and observer) form: acquire on head publishes the
     * producer's buffer writes */
    size_t readAvailable() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }
    /* producer-side form: acquire on TAIL — without it the producer can
     * see a recycled slot as free before the consumer's reads of it have
     * happened-before, and overwrite samples mid-read (found by TSan) */
    size_t writeAvailable() const {
        return cap_ - (head_.load(std::memory_order_relaxed) -
                       tail_.load(std::memory_order_acquire));
    }

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
    uint16_t part; /* §9.4 multitimbral part for device echoes; 0 on the input/param-set ring */
};

/* Timestamped outbound events (params, ramps, notes) — one ring so cross-
 * type ordering is preserved. kind: 0 = param set, 1 = ramp, 2 = UMP word. */
struct TimedEv {
    uint8_t kind;
    uint32_t a; /* param id or UMP word */
    float v;    /* value / ramp target */
    uint64_t ts, end;
    /* MULTI-OUT M2: the device part this event targets (§9.4 key 5), resolved at QUEUE time
     * from the caller's channel (a satellite's MIDI channel N -> part N) or, when unspecified,
     * the source's own channel — so a single main instance drives every part per-event instead
     * of one fixed part per source. A note (kind 2) carries its channel in the UMP word instead. */
    uint8_t channel = 0;
};

/* SPSC ring of fixed-capacity POD items. Capacity must be a power of two. Overflow REFUSES (push
 * returns false; the caller retries — automation resends anyway). The relaxed/acquire/release
 * ordering is the load-bearing lock-free contract, so it lives here ONCE — the param and event
 * rings can't drift (ring-tests stresses both producer/consumer). */
template <typename T, size_t kCap>
class SpscRing {
    static_assert((kCap & (kCap - 1)) == 0, "SpscRing capacity must be a power of two");

public:
    bool push(const T &e) { /* producer: audio thread */
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= kCap) return false;
        buf_[h & (kCap - 1)] = e;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T &out) { /* consumer: feeder thread */
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t & (kCap - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }
    bool empty() const {
        return tail_.load(std::memory_order_relaxed) ==
               head_.load(std::memory_order_acquire);
    }

private:
    T buf_[kCap];
    std::atomic<size_t> head_{0}, tail_{0};
};

/* Timestamped outbound events (params/ramps/notes) — one ring so cross-type ordering is kept. */
using TimedRing = SpscRing<TimedEv, 1024>;
/* Param changes (audio thread -> feeder); device front-panel echoes ride one of these back. */
using ParamRing = SpscRing<ParamChange, 256>;
