/* op_ratelimit.h — §16 per-connection rate-limit for the EXPENSIVE control ops
 * (state.snapshot, diag.bundle), as a pure integer token bucket.
 *
 * §16 "Denial of service": credit (§4.2.1) bounds memory, but two control ops are
 * O(whole-state) work rather than O(message): state.snapshot serializes the entire live
 * state closure to the content store, and diag.bundle assembles identity + the §14.2
 * counters + the §4.2 log ring. A peer that spams either on ONE session pins the single
 * recv thread and starves every legitimate control request — a flood-DoS the credit
 * window does not cover. This is the SHOULD the spec names for exactly these two ops.
 *
 * The shed is a token bucket: credit accrues at real wall-rate up to a burst ceiling and
 * each admitted op costs one min-interval. A NORMAL cadence (a snapshot on save / host
 * pull §10.4, one bundle on a support click) never approaches the ceiling and passes
 * untouched; a flood is throttled to the sustained trickle. It fails CLOSED-BUT-POLITE —
 * the caller returns the §5.3 `busy` error and the session is NEVER dropped (unlike the
 * pre-hello penalty ring in conn_ratelimit.h, which sheds an UNauthenticated half-open).
 * Per-CONNECTION, not per-IP: the single-session reference device has one peer at a time,
 * and the SHOULD binds on USB (a hostile host) as much as on the §4.4 segment, so there is
 * no loopback exemption — the guard runs on every session.
 *
 * Integer, monotonic-ns accounting; pure over caller-owned state so the policy unit-tests
 * off-hardware (mirrors conn_ratelimit.h / evq_mod.h / voice_alloc.h). */
#ifndef HARP_DEVICE_OP_RATELIMIT_H
#define HARP_DEVICE_OP_RATELIMIT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t allowance_ns; /* accrued credit, in ns, capped at burst_ns */
    uint64_t last_ns;      /* monotonic ns of the last decision; 0 = fresh bucket (full burst) */
} harp_op_bucket;

/* Try to admit one expensive op at monotonic time `now_ns`. Credit accrues at real
 * wall-rate (1 ns per ns elapsed) up to `burst_ns`; each admitted op costs `cost_ns`
 * (the sustained min-interval). Returns true = ADMIT, false = SHED (rate-limited).
 * A fresh bucket (last_ns == 0, as zero-initialised at session start) begins with a full
 * burst so the first ops on a new session are never throttled. Pure. */
static inline bool harp_op_admit(harp_op_bucket *b, uint64_t now_ns,
                                 uint64_t cost_ns, uint64_t burst_ns) {
    if (b->last_ns == 0) {            /* fresh session/bucket: start with a full burst */
        b->allowance_ns = burst_ns;
    } else if (now_ns > b->last_ns) { /* accrue elapsed real time as credit, capped at burst */
        b->allowance_ns += now_ns - b->last_ns;
        if (b->allowance_ns > burst_ns) b->allowance_ns = burst_ns;
    }
    b->last_ns = now_ns;
    if (b->allowance_ns >= cost_ns) { b->allowance_ns -= cost_ns; return true; }
    return false;
}

#endif /* HARP_DEVICE_OP_RATELIMIT_H */
