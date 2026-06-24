/* conn_ratelimit.h — §16 (Ethernet/IP) connection-attempt rate-limit, as pure functions.
 *
 * §16 clause (b): on the Ethernet/IP binding a device MUST rate-limit connection attempts (any
 * node on the segment can reach it). The per-connection pre-hello DEADLINE (harp-deviced.c) bounds
 * ONE connection; this caps the AGGREGATE from one abusive source. A per-peer-IP penalty ring,
 * keyed ONLY on a PRE-HELLO failure (a half-open / slow-loris that held the single session slot
 * without ever completing core.hello), sheds that source for a short window — while a
 * hello-completing client is NEVER penalized, so a legitimate post-flood reconnect is admitted on
 * its very next attempt (the PR3 lesson: a global connection-COUNT token bucket false-shed the
 * recovery probe). The accept loop SKIPS loopback peers (127.0.0.0/8 — the local refdev/host, not
 * "a node on the segment"), so per-IP keying is never confounded by a shared loopback address.
 * Pure over a fixed ring so the policy unit-tests off-hardware (mirrors evq_mod.h / voice_alloc.h). */
#ifndef HARP_DEVICE_CONN_RATELIMIT_H
#define HARP_DEVICE_CONN_RATELIMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t ip;       /* peer IPv4 (network order); 0 = empty slot */
    uint64_t until_ns; /* shed this ip until this monotonic time */
} harp_peer_penalty;

/* Is `ip_net` currently shed (a pre-hello failure recorded within its penalty window)? Pure. */
static inline bool harp_peer_penalized(const harp_peer_penalty *ring, size_t n,
                                       uint32_t ip_net, uint64_t now_ns) {
    if (!ip_net) return false; /* non-TCP (USB gadget) has no peer IP */
    for (size_t i = 0; i < n; i++)
        if (ring[i].ip == ip_net && now_ns < ring[i].until_ns) return true;
    return false;
}

/* Record a `penalty_ns` shed window for `ip_net` (round-robin over the ring slot at *idx). Pure. */
static inline void harp_peer_penalize(harp_peer_penalty *ring, size_t n, size_t *idx,
                                      uint32_t ip_net, uint64_t now_ns, uint64_t penalty_ns) {
    if (!ip_net) return;
    ring[*idx].ip = ip_net;
    ring[*idx].until_ns = now_ns + penalty_ns;
    *idx = (*idx + 1) % n;
}

#endif /* HARP_DEVICE_CONN_RATELIMIT_H */
