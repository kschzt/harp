/* Concurrent (SPSC) stress for host/freerun.c — the runtime's REAL topology:
 * a PRODUCER thread doing push()+observe() while a CONSUMER thread does pull(),
 * plus an OBSERVER doing get_stats(). The single-threaded freerun/rtp tests
 * never exercised this; the design that wires freerun into HarpRuntime does.
 * Built with ThreadSanitizer — its job is to PROVE no data race on the ring
 * indices, ratio_target, or counters (a torn fill once risked a heap overrun).
 * Also asserts the audio path stays sane under concurrency: warms up, delivers
 * non-silent audio, buffer stays bounded, no crash. */
#include "freerun.h"
#include <samplerate.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#define RATE   48000.0
#define DEV    48002.4          /* +50 ppm */
#define F      1000.0
#define NSPKT  240
#define BLK    256
#define CH     2               /* stereo, as the runtime uses */
#define TARGET 2048u
#define CAP    8192u

static atomic_int stop;
static harp_freerun *FR;

static void nsleep(long ns) { struct timespec t = {0, ns}; nanosleep(&t, NULL); }
static unsigned long long mono_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}

static void *producer(void *arg) {
    (void)arg;
    float pkt[NSPKT * CH];
    long dev = 0;
    while (!atomic_load(&stop)) {
        for (int i = 0; i < NSPKT; i++) {            /* stereo sine */
            float s = (float)sin(2 * M_PI * F * (dev + i) / RATE);
            pkt[2*i] = s; pkt[2*i+1] = s;
        }
        harp_freerun_observe(FR, (unsigned long long)dev, mono_ns());
        harp_freerun_push(FR, pkt, NSPKT);
        dev += NSPKT;
        nsleep((long)(NSPKT / DEV * 1e9));           /* paced ~ device rate */
    }
    return NULL;
}

static void *observer(void *arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        harp_freerun_stats s; harp_freerun_get_stats(FR, &s);
        (void)s;
        nsleep(2000000);                             /* 2 ms — like a UI meter */
    }
    return NULL;
}

int main(void) {
    harp_freerun_cfg cfg = {CH, RATE, RATE, TARGET, CAP, SRC_SINC_FASTEST};
    FR = harp_freerun_new(&cfg);
    if (!FR) { printf("freerun-mt: new() failed\n"); return 1; }

    pthread_t pt, ot;
    pthread_create(&pt, NULL, producer, NULL);
    pthread_create(&ot, NULL, observer, NULL);

    /* CONSUMER = this thread (the audio callback), pulling fixed blocks ~host rate */
    float out[BLK * CH];
    double sumsq = 0; long outn = 0; unsigned worst_fill = 0;
    long blocks = (long)(2.0 * RATE / BLK);          /* ~2 s of audio */
    unsigned long long t0 = mono_ns();
    for (long b = 0; b < blocks; b++) {
        harp_freerun_pull(FR, out, BLK);
        for (int i = 0; i < BLK * CH; i++) { sumsq += (double)out[i]*out[i]; outn++; }
        harp_freerun_stats s; harp_freerun_get_stats(FR, &s);
        if (s.fill_frames > worst_fill) worst_fill = s.fill_frames;
        /* pace ~host rate */
        unsigned long long due = t0 + (unsigned long long)((double)(b+1)*BLK/RATE*1e9);
        unsigned long long now = mono_ns();
        if (due > now) nsleep((long)(due - now));
    }
    atomic_store(&stop, 1);
    pthread_join(pt, NULL); pthread_join(ot, NULL);

    harp_freerun_stats st; harp_freerun_get_stats(FR, &st);
    double rms = sqrt(sumsq / outn);
    int ok = 1;
    if (!harp_freerun_warm(FR))         { printf("  FAIL never warmed\n"); ok = 0; }
    if (rms < 0.05)                     { printf("  FAIL silent rms=%.4f (no audio crossed threads)\n", rms); ok = 0; }
    if (worst_fill >= CAP)              { printf("  FAIL fill hit capacity %u (ring corruption?)\n", worst_fill); ok = 0; }
    if (st.fill_frames >= CAP)          { printf("  FAIL final fill %u >= cap\n", st.fill_frames); ok = 0; }
    printf("freerun-mt: est %.1f ppm  rms %.4f  worst_fill %u/%u  under %u over %u  warm %d -> %s\n",
           st.est_ppm, rms, worst_fill, CAP, st.underflow_frames, st.overflow_frames,
           harp_freerun_warm(FR), ok ? "PASS" : "FAIL");
    harp_freerun_free(FR);
    return ok ? 0 : 1;
}
