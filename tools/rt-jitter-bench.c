/* tools/rt-jitter-bench.c — a hardware-free regression proxy for the §4.3 USB
 * real-time feed thread (#133).
 *
 * #133 promoted the host USB feed/IO threads to the host real-time class
 * (macOS THREAD_TIME_CONSTRAINT / Linux SCHED_FIFO) and measured, on a dev rig
 * under 16 CPU burners: producer-cadence out_gap P99.9 21.7 -> 11.1 ms, cliff
 * dropouts 3 -> 0. Those numbers needed a USB device and were reproducible
 * NOWHERE in CI. This bench captures the SCHEDULING claim that produced them
 * with no device on any runner:
 *
 *   - One "feed" thread is promoted with the SAME production call the USB
 *     completion thread uses — harp_thread_set_realtime() (host/rt_sched.c),
 *     which honours HARP_USB_RT=0 (the #133 A/B "before" arm).
 *   - It runs a periodic loop on an ABSOLUTE schedule (target_i = start +
 *     i*period): each iteration it sleeps to its next deadline and records how
 *     LATE it actually woke. That wakeup-lateness is the pure scheduling
 *     component of the USB out_gap tail — when this thread is preempted under
 *     load, the real feeder is preempted too and the FunctionFS OUT endpoint
 *     empties (the cliff-onset dropout).
 *   - N burner threads saturate every core so an ordinary time-share thread is
 *     descheduled. The RT thread is not — that is the whole claim.
 *
 * Output (one parseable line on stdout): the lateness distribution in µs. With
 * --max-late-us it exits non-zero if the P99.9 lateness exceeds the threshold;
 * with --require-rt it exits 3 if the host refused to grant RT (so the gate
 * cannot draw a conclusion here). scripts/rt-jitter-gate.sh runs it twice — RT
 * on (must pass the threshold) and HARP_USB_RT=0 (must blow past it) — to prove
 * the gate has discriminating power. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sync_compat.h" /* portable threads/mutex/flag (POSIX + Windows) */
#include "harp/plat.h"   /* harp_now_ns, harp_sleep_ns, harp_plat_init */
#include "rt_sched.h"    /* harp_thread_set_realtime — the function under test */

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static int cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* ---- CPU burners: pure time-share contention so the feed thread is preempted ---- */
static harp_flag g_burn_run;
static HARP_THREAD_RET burn_main(void *arg) {
    (void)arg;
#ifdef _WIN32
    /* Windows models the contention ONE notch higher than POSIX — deliberately, and
     * honestly. A periodically-waking NORMAL-priority thread gets a wait-completion
     * priority BOOST (~+2), so it can still preempt equal-priority (NORMAL) burners:
     * plain time-share load does NOT deschedule it, and RT-on vs RT-off wakeup tails
     * come out identical, both pinned at the 1 ms timer-resolution floor (measured on
     * the hosted runner: 1025µs vs 1023µs). That is not RT having no value on Windows
     * — it is that the threat a pure-wakeup audio thread actually faces here is the
     * OTHER elevated audio/system threads a DAW runs, which the boost can NOT beat.
     * MMCSS "Pro Audio" (and the TIME_CRITICAL fallback) exist precisely to win that
     * race. So the faithful Windows contention is THREAD_PRIORITY_HIGHEST burners (10
     * within the NORMAL class): a boosted time-share feed thread ties/loses to them
     * and its wakeup tail blows out, while the RT-promoted thread (MMCSS real-time /
     * TIME_CRITICAL = 15) still preempts them and stays at the timer floor — which is
     * exactly the RT-on-vs-off separation the ratio gate needs. POSIX is untouched:
     * there, default-priority burners already deschedule a time-share thread (and
     * SCHED_FIFO / THREAD_TIME_CONSTRAINT is what escapes them). */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    volatile double x = 0.0;
    while (harp_flag_load_acq(&g_burn_run)) {
        for (int i = 0; i < 200000; i++) x += (double)i * 0.5 + 1.0;
    }
    (void)x;
    return 0;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static uint32_t pct(const uint32_t *sorted, size_t n, double p) {
    size_t i = (size_t)(n * p);
    if (i >= n) i = n - 1;
    return sorted[i];
}

int main(int argc, char **argv) {
    double period_us = HARP_RT_PERIOD_US_DEFAULT; /* the §8.3 256-sample @ 48k block cadence */
    int samples = 6000;                           /* >=1000 so the P99.9 index is meaningful */
    int warmup = 256;                             /* discard while the burners ramp + caches warm */
    int burners = -1;                             /* default: 2*ncpu, clamped [12,64] */
    long max_late_us = -1;                        /* >=0: exit 1 if lateness P99.9 exceeds it */
    int require_rt = 0;                           /* exit 3 if RT was not granted */

    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--period-us") && a + 1 < argc) period_us = atof(argv[++a]);
        else if (!strcmp(argv[a], "--samples") && a + 1 < argc) samples = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--warmup") && a + 1 < argc) warmup = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--burners") && a + 1 < argc) burners = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--max-late-us") && a + 1 < argc) max_late_us = atol(argv[++a]);
        else if (!strcmp(argv[a], "--require-rt")) require_rt = 1;
        else {
            fprintf(stderr,
                    "usage: %s [--period-us N] [--samples N] [--warmup N] [--burners N]\n"
                    "          [--max-late-us T] [--require-rt]\n"
                    "  HARP_USB_RT=0 in the environment disables the RT promotion (the A/B baseline).\n",
                    argv[0]);
            return 2;
        }
    }
    if (period_us <= 0) period_us = HARP_RT_PERIOD_US_DEFAULT;
    if (samples < 1000) samples = 1000;
    if (warmup < 0) warmup = 0;
    if (burners < 0) {
        burners = 2 * cpu_count();
        if (burners < 2) burners = 2;   /* scale to cores: a fixed-12 floor 4x-oversubscribes a 3-core CI runner, where even an RT-granted thread cannot hold the deadline */
        if (burners > 64) burners = 64;
    }

    harp_plat_init(); /* Windows: 1 ms timer resolution so sub-ms sleeps are honoured */

    uint32_t *late = malloc((size_t)samples * sizeof *late);
    uint32_t *gap = malloc((size_t)samples * sizeof *gap);
    harp_thread *bt = malloc((size_t)burners * sizeof *bt);
    if (!late || !gap || !bt) {
        fprintf(stderr, "rt-jitter-bench: out of memory\n");
        return 2;
    }

    /* Spin up the contention BEFORE promoting + measuring, so every sample is
     * taken with all cores already saturated. */
    harp_flag_store_rel(&g_burn_run, 1);
    for (int i = 0; i < burners; i++) harp_thread_create(&bt[i], burn_main, NULL);

    /* THIS thread is the feed thread: promote it exactly as the USB completion
     * thread does. Returns false (and stays time-share) under HARP_USB_RT=0 or
     * where the host refuses RT. */
    int rt_granted = harp_thread_set_realtime(period_us) ? 1 : 0;

    const uint64_t period_ns = (uint64_t)(period_us * 1000.0);
    uint64_t start = harp_now_ns();
    uint64_t last = start;
    for (int i = 0; i < warmup + samples; i++) {
        uint64_t target = start + (uint64_t)(i + 1) * period_ns;
        uint64_t now = harp_now_ns();
        if (target > now) harp_sleep_ns(target - now);
        uint64_t wake = harp_now_ns();
        if (i >= warmup) {
            uint64_t l = wake > target ? wake - target : 0; /* one-sided: how late we woke */
            uint64_t g = wake - last;                       /* inter-wakeup interval (out_gap analog) */
            late[i - warmup] = (uint32_t)(l / 1000 > 0xFFFFFFFFull ? 0xFFFFFFFFu : l / 1000);
            gap[i - warmup] = (uint32_t)(g / 1000 > 0xFFFFFFFFull ? 0xFFFFFFFFu : g / 1000);
        }
        last = wake;
    }

    harp_flag_store_rel(&g_burn_run, 0);
    for (int i = 0; i < burners; i++) harp_thread_join(bt[i]);

    qsort(late, (size_t)samples, sizeof *late, cmp_u32);
    qsort(gap, (size_t)samples, sizeof *gap, cmp_u32);
    double mean = 0;
    for (int i = 0; i < samples; i++) mean += late[i];
    mean /= samples;
    uint32_t lp50 = pct(late, samples, 0.50), lp99 = pct(late, samples, 0.99),
             lp999 = pct(late, samples, 0.999), lmax = late[samples - 1];
    uint32_t gp999 = pct(gap, samples, 0.999), gmax = gap[samples - 1];

    /* one machine-parseable line (the gate script greps these key=value fields) */
    printf("RTBENCH rt_granted=%d period_us=%.0f burners=%d samples=%d cpus=%d "
           "late_mean=%.1f late_p50=%u late_p99=%u late_p999=%u late_max=%u "
           "gap_p999=%u gap_max=%u\n",
           rt_granted, period_us, burners, samples, cpu_count(), mean, lp50, lp99,
           lp999, lmax, gp999, gmax);
    fflush(stdout);

    free(late);
    free(gap);
    free(bt);

    if (require_rt && !rt_granted) {
        fprintf(stderr,
                "rt-jitter-bench: RT was NOT granted (HARP_USB_RT off, or the host "
                "refuses THREAD_TIME_CONSTRAINT/SCHED_FIFO) — gate inconclusive here\n");
        return 3;
    }
    if (max_late_us >= 0 && (long)lp999 > max_late_us) {
        fprintf(stderr,
                "rt-jitter-bench: FAIL — wakeup-lateness P99.9 = %u µs exceeds the "
                "%ld µs gate (the feed thread is being descheduled under load)\n",
                lp999, max_late_us);
        return 1;
    }
    return 0;
}
