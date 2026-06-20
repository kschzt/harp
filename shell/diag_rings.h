/* §14.4 host-context-B diagnostic rings — the two instrumentation buffers the
 * DiagBundleAssembler (HarpRuntime::getDiagBundle) drains into top-level keys 6
 * (session state-machine history, §12.1) and 8 (recent runtime logs, §14.4).
 *
 * These are DIAGNOSTIC-ONLY: nothing in here is on the audio render path, and
 * recording into either ring MUST NOT change a single rendered sample (the
 * offline-golden gate). The SessionHistory ring is control-path (its producers
 * are the supervisor/feeder/reader control sites, never the audio thread); the
 * RuntimeLog ring is LOCK-FREE / WAIT-FREE because a log producer MAY be the
 * audio thread — its push() adds NO lock and NO allocation to whoever calls it.
 *
 * Enum numberings are FROZEN against docs/diag-bundle-design.md (session-state,
 * transition-reason, log-level). Do not renumber without bumping the bundle
 * version (diag-bundle key 1). */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

/* §12.1 session-state enum (FROZEN, diag-bundle-design.md:164). */
enum HarpSessionState : uint8_t {
    HARP_ST_DETACHED = 0,
    HARP_ST_ATTACHED = 1,
    HARP_ST_NEGOTIATED = 2,
    HARP_ST_SYNCED = 3,
    HARP_ST_STREAMING = 4,
};

/* transition-reason enum (FROZEN, diag-bundle-design.md:175). */
enum HarpTransitionReason : uint8_t {
    HARP_TR_ATTACH = 0,
    HARP_TR_DETACH = 1,
    HARP_TR_HELLO_OK = 2,
    HARP_TR_SERIAL_MISMATCH = 3,
    HARP_TR_ENGINE_MAJOR_MISMATCH = 4,
    HARP_TR_PARAM_MAP_HASH_MISMATCH = 5,
    HARP_TR_RECONCILE_PUSH = 6,
    HARP_TR_RECONCILE_PULL = 7,
    HARP_TR_RECONCILE_OPEN_RO = 8,
    HARP_TR_RECONCILE_DUPLICATE_PUSH = 9,
    HARP_TR_AUDIO_START = 10,
    HARP_TR_AUDIO_STOP = 11,
    HARP_TR_SESSION_RESET = 12,
    HARP_TR_TRANSPORT_ERROR = 13,
};

/* log-level enum (FROZEN, diag-bundle-design.md:195). */
enum HarpLogLevel : uint8_t {
    HARP_LOG_DEBUG = 0,
    HARP_LOG_INFO = 1,
    HARP_LOG_WARN = 2,
    HARP_LOG_ERROR = 3,
};

/* One §12.1 state-machine transition. Fixed-size (no heap) so a record never
 * allocates; `detail` is a bounded snapshot of the free-text reason. */
struct StateTransition {
    uint64_t tstamp_epoch = 0; /* key 0[0]: wall-clock epoch seconds (0 if unknown) */
    uint64_t tstamp_msc = 0;   /* key 0[1]: stream MSC (0 if pre-stream) */
    uint8_t from_state = HARP_ST_DETACHED; /* key 1 */
    uint8_t to_state = HARP_ST_DETACHED;   /* key 2 */
    uint8_t reason_code = HARP_TR_ATTACH;  /* key 3 (always emitted) */
    char detail[224] = {0};                /* key 4 free-text (anon => "") */
};

/* One §14.4 runtime log record. Fixed-size; `tag`/`msg` are bounded snapshots. */
struct LogRecord {
    uint64_t msc = 0;          /* key 0: stream MSC (0 if pre-stream) */
    uint64_t tstamp_epoch = 0; /* key 4: wall-clock correlation (0 => key 4 omitted) */
    uint8_t level = HARP_LOG_INFO; /* key 1 */
    char tag[32] = {0};            /* key 2 (NEVER anonymized) */
    char msg[224] = {0};           /* key 3 (anon => "") */
};

/* SessionHistory ring — CONTROL-PATH only (§12.1 transition sites in
 * runtime.cpp run on the supervisor/feeder/reader control threads, NEVER the
 * audio thread). A short internal mutex is acceptable because transitions are
 * rare (a handful per session); it is independent of ctlMutex_, so a reader-
 * thread transport-error record can be filed without acquiring ctlMutex_.
 * Drop-OLDEST on overflow: a long-lived session keeps the most recent history. */
class SessionHistoryRing {
public:
    void record(const StateTransition &t) {
        std::lock_guard<std::mutex> lk(mu_);
        buf_[head_ & (kCap - 1)] = t;
        head_++;
        if (head_ - tail_ > kCap) tail_ = head_ - kCap; /* drop oldest */
    }
    /* Consumer (getDiagBundle, under ctlMutex_): NON-destructive snapshot of the
     * recent window, oldest-first. Leaving the ring intact lets a later bundle
     * re-observe the same lifecycle history (a snapshot, not a queue drain). */
    std::vector<StateTransition> snapshot(size_t maxN) const {
        std::lock_guard<std::mutex> lk(mu_);
        size_t avail = head_ - tail_;
        size_t n = avail < maxN ? avail : maxN;
        std::vector<StateTransition> out;
        out.reserve(n);
        for (size_t i = head_ - n; i < head_; i++) out.push_back(buf_[i & (kCap - 1)]);
        return out;
    }

private:
    static constexpr size_t kCap = 128; /* power of two; bounded, circular */
    mutable std::mutex mu_;
    StateTransition buf_[kCap];
    size_t head_ = 0, tail_ = 0; /* guarded by mu_ */
};

/* RuntimeLog ring — LOCK-FREE / WAIT-FREE multi-producer, single-consumer.
 * A log producer MAY be the audio thread, so push() must NOT lock or allocate.
 * >= 64 KiB: kCap(256) * sizeof(LogRecord) clears 64 KiB. Producers claim a
 * slot with one fetch_add on a monotonic sequence; the consumer (getDiagBundle,
 * under ctlMutex_) reads the recent window. Drop-OLDEST on overflow — a flood
 * (e.g. a per-block warning) overwrites stale records, keeping the newest.
 *
 * Concurrency: producers race only on `seq_` (the claim). The consumer never
 * writes a slot, so it never races a producer's slot write; it tears at most
 * the few in-flight slots near the head during a flood, which is acceptable for
 * a best-effort diagnostic snapshot (no UB: fixed-size POD slots, no pointers).
 * This matches the existing lock-free SPSC patterns in ring.h. */
class RuntimeLogRing {
public:
    static_assert(sizeof(LogRecord) * 256 >= 65536, "RuntimeLog ring must be >= 64 KiB");

    /* Producer — wait-free: one fetch_add, one POD copy. Safe on ANY thread,
     * including the audio thread. Never blocks, allocates, or syscalls. */
    void push(const LogRecord &rec) {
        uint64_t s = seq_.fetch_add(1, std::memory_order_acq_rel);
        buf_[s & (kCap - 1)] = rec; /* relaxed POD write into the claimed slot */
        /* publish: the consumer reads `count_` to bound its window. monotonic. */
        count_.store(s + 1, std::memory_order_release);
    }
    /* Consumer (getDiagBundle, under ctlMutex_): NON-destructive recent-window
     * snapshot, oldest-first. Bounded to the last min(maxN, kCap, total) records. */
    std::vector<LogRecord> snapshot(size_t maxN) const {
        uint64_t total = count_.load(std::memory_order_acquire);
        size_t avail = total > kCap ? kCap : (size_t)total;
        size_t n = avail < maxN ? avail : maxN;
        std::vector<LogRecord> out;
        out.reserve(n);
        for (uint64_t i = total - n; i < total; i++) out.push_back(buf_[i & (kCap - 1)]);
        return out;
    }

private:
    static constexpr size_t kCap = 256; /* power of two; 256 * sizeof(LogRecord) >= 64 KiB */
    LogRecord buf_[kCap];
    std::atomic<uint64_t> seq_{0};   /* producers' fetch_add claim counter */
    std::atomic<uint64_t> count_{0}; /* high-water mark the consumer bounds by */
};
