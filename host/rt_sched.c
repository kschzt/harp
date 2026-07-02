/* host/rt_sched.c — promote a thread to the host's real-time scheduling class.
 *
 * Extracted verbatim from host/usb_io.c so the helper is available to every
 * realtime-path thread (and to a hardware-free regression bench) WITHOUT pulling
 * in libusb. No audio, no transport state — just the platform real-time syscalls
 * (macOS THREAD_TIME_CONSTRAINT, Linux SCHED_FIFO, Windows MMCSS "Pro Audio").
 *
 * Promote the CALLING thread to the host's real-time class so the realtime audio
 * path — every audio byte the device returns, every event-send ack, the pacing
 * feeder, the D->H reader — is never descheduled by ordinary time-share work
 * under host CPU load. This is the USB twin of what CoreAudio's own I/O thread
 * runs at (THREAD_TIME_CONSTRAINT). A mere high QoS band (USER_INTERACTIVE) is
 * STILL time-share: under contention it gets preempted, which empties the
 * FunctionFS bulk-OUT endpoint and starves the host's D->H reaping faster than
 * the small device jitter buffer can cover -> cliff-onset dropouts. The fix is
 * DETERMINISM, not a fatter buffer: `period_us` is the audio block cadence, so the
 * kernel sizes a sub-millisecond CPU reservation.
 *
 * Degrades gracefully: if the RT request is denied (a sandbox without the
 * privilege, or an unknown platform) the thread keeps whatever scheduling it had
 * and we log ONCE — the stream still runs, just at the old determinism. Returns
 * true iff real-time was granted. */
#include "rt_sched.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/mach_time.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <string.h>
#elif defined(_WIN32)
#include <windows.h>
#include <avrt.h> /* MMCSS: AvSetMmThreadCharacteristicsW / AvSetMmThreadPriority — link avrt */
/* portable thread-local: MSVC's C mode is happiest with __declspec(thread); MinGW
 * gcc has C11 _Thread_local. Holds the MMCSS handle for the thread's lifetime. */
#if defined(_MSC_VER)
#  define HARP_THREAD_LOCAL __declspec(thread)
#else
#  define HARP_THREAD_LOCAL _Thread_local
#endif
#endif

bool harp_thread_set_realtime(double period_us) {
    static int logged_fail = 0; /* one diagnostic line, not per-thread spam */
    /* Single global off-switch (A/B "before" arm + safety valve): HARP_USB_RT=0
     * keeps EVERY realtime-path thread on its prior time-share scheduling. */
    const char *rt = getenv("HARP_USB_RT");
    if (rt && rt[0] == '0') return false;
    if (period_us <= 0) period_us = HARP_RT_PERIOD_US_DEFAULT;
    double period_ns = period_us * 1000.0;
#if defined(__APPLE__)
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) != KERN_SUCCESS || tb.numer == 0) return false;
    double ticks_per_ns = (double)tb.denom / (double)tb.numer; /* ns -> mach abs ticks */
    thread_time_constraint_policy_data_t pol;
    pol.period      = (uint32_t)(period_ns * ticks_per_ns);
    /* CPU budget per period: generous headroom over the few-µs actual reap so the
     * kernel never demotes us for a brief burst of completions (a reservation, not
     * a target — a thread blocked in handle_events consumes none of it). */
    pol.computation = (uint32_t)(period_ns * 0.20 * ticks_per_ns);
    pol.constraint  = (uint32_t)(period_ns * 0.90 * ticks_per_ns); /* finish within ~90% of the block */
    pol.preemptible = 1; /* yield to even-higher-priority work; we still outrank all time-share */
    kern_return_t kr = thread_policy_set(mach_thread_self(),
                                         THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&pol,
                                         THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) return true;
    if (!logged_fail++)
        fprintf(stderr, "harp-usb: THREAD_TIME_CONSTRAINT denied (kr=%d) — staying on QoS time-share\n",
                (int)kr);
    return false;
#elif defined(__linux__)
    struct sched_param sp;
    int lo = sched_get_priority_min(SCHED_FIFO), hi = sched_get_priority_max(SCHED_FIFO);
    /* high, but a notch below the very top so kernel/IRQ threads still preempt us */
    sp.sched_priority = lo + (hi - lo) * 3 / 4;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (rc == 0) return true;
    if (!logged_fail++)
        fprintf(stderr,
                "harp-usb: SCHED_FIFO denied (%s) — staying on default scheduling "
                "(grant CAP_SYS_NICE / RLIMIT_RTPRIO for RT)\n",
                strerror(rc));
    (void)period_ns;
    return false;
#elif defined(_WIN32)
    /* Windows: MMCSS "Pro Audio" is the UNPRIVILEGED real-time mechanism every
     * pro-audio app uses (WASAPI clients, DAWs) — no admin rights, no policy change.
     * AvSetMmThreadCharacteristicsW joins THIS thread to the "Pro Audio" MMCSS task
     * and AvSetMmThreadPriority(..._CRITICAL) lifts it to that task's top band, so
     * under host CPU load it is scheduled ahead of ordinary time-share work exactly
     * like THREAD_TIME_CONSTRAINT (mac) / SCHED_FIFO (linux) above — the same
     * determinism that keeps the §4.3 feed thread from being descheduled.
     *
     * The MMCSS handle is kept in a thread_local so a later revert is POSSIBLE, but
     * for a session-lifetime audio thread this is intentionally set-and-forget: the
     * OS releases the association automatically when the thread exits, so we never
     * have to call AvRevertMmThreadCharacteristics(). MMCSS derives its own timing
     * from the "Pro Audio" task, so period_ns does not size a reservation here. */
    static HARP_THREAD_LOCAL HANDLE mmcss = NULL;
    if (mmcss != NULL) { (void)period_ns; return true; } /* already promoted this thread — idempotent */
    DWORD task_index = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (h != NULL) {
        mmcss = h; /* hold for the thread's lifetime; the OS reclaims it on thread exit */
        AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
        (void)period_ns;
        return true;
    }
    /* MMCSS unavailable (service disabled/absent): fall back to the highest
     * time-share band. DELIBERATELY NOT REALTIME_PRIORITY_CLASS — that needs a
     * privilege and can starve the OS; TIME_CRITICAL within the normal class is the
     * unprivileged ceiling and still strongly preempts the time-share load. */
    if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        (void)period_ns;
        return true;
    }
    if (!logged_fail++)
        fprintf(stderr,
                "harp-usb: MMCSS 'Pro Audio' + TIME_CRITICAL denied (err=%lu) — "
                "staying on default scheduling\n",
                (unsigned long)GetLastError());
    (void)period_ns;
    return false;
#else
    (void)period_ns;
    if (!logged_fail++)
        fprintf(stderr, "harp-usb: no real-time scheduling on this platform — time-share\n");
    return false;
#endif
}
