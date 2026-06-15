/* HARP platform shim — the handful of OS primitives the host tooling and the
 * plugin runtime need that have no portable C spelling: a monotonic
 * high-resolution clock, a high-resolution sleep, and thread-safe UTC
 * breakdown. POSIX maps to clock_gettime/nanosleep/gmtime_r; Windows to
 * QueryPerformanceCounter, a high-resolution waitable timer, and gmtime_s,
 * plus a one-time timer-resolution bump so sub-millisecond pacing waits are
 * honored (the default ~15.6 ms granularity would starve the audio loops).
 *
 * This is intentionally NOT part of harpcore (which stays dependency-free,
 * pure-codec C11): it is compiled into the host/shell targets that need it. */
#ifndef HARP_PLAT_H
#define HARP_PLAT_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at process / plugin start. Idempotent. On Windows it raises the
 * system timer resolution to 1 ms for the process lifetime; no-op elsewhere. */
void harp_plat_init(void);

/* Monotonic clock in nanoseconds. Never goes backwards; unrelated to wall time. */
uint64_t harp_now_ns(void);

/* Sleep at least `ns` nanoseconds (high-resolution; ~0.5 ms floor on Windows). */
void harp_sleep_ns(uint64_t ns);

/* Thread-safe UTC broken-down time (wraps gmtime_r / gmtime_s). */
void harp_gmtime(time_t t, struct tm *out);

#ifdef __cplusplus
}
#endif

#endif /* HARP_PLAT_H */
