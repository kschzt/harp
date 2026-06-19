/* compat_time_win.h — MSVC-only shims for the small POSIX clock surface the device
 * uses. struct timespec comes from <time.h> (C11, VS2015+); CLOCK_MONOTONIC +
 * clock_gettime + nanosleep route through the existing high-res timer in
 * core/src/plat_win.c (harp_now_ns/harp_sleep_ns), so the engine's pacing loop and
 * device.h's now_ms compile unchanged. MinGW (winpthreads) and POSIX provide these
 * natively and never include this file. (clock_nanosleep is Linux-only and already
 * #ifdef __linux__-gated, so it is not shimmed here.) */
#ifndef HARP_DEVICE_COMPAT_TIME_WIN_H
#define HARP_DEVICE_COMPAT_TIME_WIN_H
#if defined(_WIN32) && !defined(__MINGW32__)

#include <stdint.h>
#include <time.h>      /* struct timespec */
#include "harp/plat.h" /* harp_now_ns / harp_sleep_ns — core/src/plat_win.c */

#define CLOCK_MONOTONIC 1

static __inline int clock_gettime(int clk, struct timespec *ts) {
    uint64_t ns = harp_now_ns(); /* QPC-based monotonic, immune to wall-clock steps */
    (void)clk;
    ts->tv_sec = (time_t)(ns / 1000000000ull);
    ts->tv_nsec = (long)(ns % 1000000000ull);
    return 0;
}

static __inline int nanosleep(const struct timespec *req, struct timespec *rem) {
    (void)rem;
    harp_sleep_ns((uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec);
    return 0;
}

#endif
#endif /* HARP_DEVICE_COMPAT_TIME_WIN_H */
