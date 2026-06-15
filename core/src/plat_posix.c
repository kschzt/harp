/* POSIX implementation of the HARP platform shim (harp/plat.h). */
#include "harp/plat.h"

#include <time.h>

void harp_plat_init(void) { /* nothing to do: the POSIX clocks are already hi-res */ }

uint64_t harp_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void harp_sleep_ns(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ull);
    ts.tv_nsec = (long)(ns % 1000000000ull);
    nanosleep(&ts, NULL);
}

void harp_gmtime(time_t t, struct tm *out) { gmtime_r(&t, out); }
