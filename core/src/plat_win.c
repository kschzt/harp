/* Windows implementation of the HARP platform shim (harp/plat.h). */
#include "harp/plat.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h> /* timeBeginPeriod (winmm) */

void harp_plat_init(void) {
    static LONG done = 0;
    /* timeBeginPeriod(1) is process-wide and never reset (the process owns the
     * 1 ms tick for its lifetime, as pro-audio hosts do) — guard it once. */
    if (InterlockedExchange(&done, 1) == 0) timeBeginPeriod(1);
}

uint64_t harp_now_ns(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f); /* constant; cheap, avoids an init race */
    uint64_t q = (uint64_t)c.QuadPart / (uint64_t)f.QuadPart;
    uint64_t r = (uint64_t)c.QuadPart % (uint64_t)f.QuadPart;
    return q * 1000000000ull + (r * 1000000000ull) / (uint64_t)f.QuadPart;
}

void harp_sleep_ns(uint64_t ns) {
    if (!ns) return;
    /* A high-resolution one-shot waitable timer (Win10 1803+) gives ~0.5 ms
     * accuracy without the global side effects of busy-waiting; reused per
     * thread (HARP's threads are long-lived). */
    static __declspec(thread) HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(ns / 100); /* 100-ns units, negative = relative */
        if (due.QuadPart == 0) due.QuadPart = -1;
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)((ns + 999999ull) / 1000000ull)); /* fallback */
}

void harp_gmtime(time_t t, struct tm *out) { gmtime_s(out, &t); }
