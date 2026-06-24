#ifndef HARP_FENCE_WAIT_H
#define HARP_FENCE_WAIT_H
#include <stdbool.h>
#include <stdint.h>

/* §8.3.1 fence-wait predicate (PURE, host-unit-tested in harp_engine_logic_tests.c).
 *
 * host_paced_loop blocks a fenced pacing-frame range until its named event-stream messages are
 * consumed (two pipes, no mutual ordering). Keep blocking WHILE the events are still pending AND
 * the session is running AND — on the REAL-TIME path — the few-millisecond bound has not yet
 * elapsed:
 *   - REAL-TIME host-paced (USB; offline=false): §8.3.1 "Devices MUST bound the wait (a few
 *     milliseconds) so a host that fences beyond what it feeds cannot wedge the stream." A
 *     running wall clock makes a stalled fence a wedged stream, so we stop at the deadline,
 *     render the range with the late event applied immediately, and count the expiry
 *     (g_fence_timeouts / evt_late are the probes).
 *   - Deterministic OFFLINE bounce (the §8.3-over-§8.7 TCP pull; offline=true): faster-than-
 *     real-time with no stream to wedge, and it MUST reproduce the EXACT fenced event set for
 *     bit-exact determinism — so its fence stays an UNBOUNDED barrier (ignores the deadline).
 *
 * pending = (int32_t)(want - g_evt_consumed) — signed, wraparound-safe (§8.3.1). */
static inline bool harp_fence_keep_waiting(int32_t pending, bool running, bool offline,
                                           uint64_t now_ns, uint64_t deadline_ns) {
    if (pending <= 0 || !running) return false; /* consumed, or session stopping */
    if (offline) return true;                   /* deterministic bounce: unbounded barrier */
    return now_ns < deadline_ns;                /* real-time: bounded — stop at the deadline */
}

#endif /* HARP_FENCE_WAIT_H */
