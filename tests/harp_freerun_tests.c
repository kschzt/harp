/* Trace-driven validation of the free-running receiver (host/freerun.c) with
 * TIMESTAMP-based recovery. Models a device emitting fixed-size packets at its
 * own (drifted) crystal rate; each packet carries its device sample index (the
 * RTP timestamp) and arrives at a host time = production time + bounded jitter.
 * The host observes (dev_ts, host_ns) for recovery and pulls fixed blocks.
 *
 * Asserts: zero under/overflow (jitter absorbed), glitch-free output, amplitude
 * preserved, the elastic buffer stays bounded, and — the airtight part — SINAD
 * far above the frame-count floor, because the rate is recovered from sub-frame
 * arrival timing rather than whole-frame counts.
 */
#include "freerun.h"
#include <samplerate.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define HOST    48000.0
#define DEVNOM  48000.0
#define F       1000.0
#define BLK     256
#define NSPKT   240            /* device packet size (frames)                    */
#define TARGET  2048u
#define CAP     8192u
#define JIT_US  50.0          /* +/- arrival jitter, microseconds               */

int main(void) {
    const double drift_ppm = 50.0;
    const double dev_true = DEVNOM * (1.0 + drift_ppm * 1e-6);
    const double prebuf_s = (double)TARGET / HOST;        /* head start = target latency */
    const double T0 = 100.0;                              /* offset so host_ns > 0 */
    harp_freerun_cfg cfg = {1, HOST, DEVNOM, TARGET, CAP, SRC_SINC_MEDIUM_QUALITY};
    harp_freerun *fr = harp_freerun_new(&cfg);
    if (!fr) { printf("freerun: new() failed\n"); return 1; }

    const long nblocks = 6000, warmup = 4000;
    float pkt[NSPKT], out[BLK];
    long dev_emitted = 0;
    int glitches = 0; double maxstep = 0, sumsq = 0; long outn = 0; double prev = 0;
    float *ya = malloc(sizeof(float) * (size_t)(nblocks - warmup) * BLK);
    long na = 0;

    for (long blk = 0; blk < nblocks; blk++) {
        double t = blk * BLK / HOST;                      /* host time (s)        */
        /* deliver every packet that has arrived by now (in order) */
        for (;;) {
            long k = dev_emitted / NSPKT;
            double jit = (((double)((k * 1103515245L + 12345L) % 2001L)) - 1000.0)
                         * (JIT_US * 1e-6 / 1000.0);       /* +/- JIT_US us         */
            double arr = (double)dev_emitted / dev_true - prebuf_s + jit;
            if (arr > t) break;
            for (int i = 0; i < NSPKT; i++)
                pkt[i] = (float)sin(2 * M_PI * F * (dev_emitted + i) / DEVNOM);
            harp_freerun_observe(fr, (unsigned long long)dev_emitted,
                                 (unsigned long long)((arr + T0) * 1e9));
            harp_freerun_push(fr, pkt, NSPKT);
            dev_emitted += NSPKT;
        }
        harp_freerun_pull(fr, out, BLK);
        if (blk > warmup) {
            for (int i = 0; i < BLK; i++) {
                double step = fabs(out[i] - prev);
                if (step > 0.5) glitches++;
                if (step > maxstep) maxstep = step;
                sumsq += out[i] * out[i]; outn++; prev = out[i];
                ya[na++] = out[i];
            }
        } else {
            prev = out[BLK - 1];
        }
    }

    harp_freerun_stats st; harp_freerun_get_stats(fr, &st);
    double rms = sqrt(sumsq / outn);

    /* SINAD: LS sine fit at the recovered frequency, searched +/-200 ppm. */
    double f0 = F * dev_true / HOST, base = (double)(warmup + 1) * BLK, sinad = -1e9;
    for (int kf = -200; kf <= 200; kf++) {
        double f_out = f0 * (1.0 + kf * 1e-6);
        double Sss = 0, Scc = 0, Ssc = 0, Sys = 0, Syc = 0, Syy = 0;
        for (long j = 0; j < na; j++) {
            double ph = 2 * M_PI * f_out * (base + j) / HOST;
            double s = sin(ph), c = cos(ph), y = ya[j];
            Sss += s*s; Scc += c*c; Ssc += s*c; Sys += y*s; Syc += y*c; Syy += y*y;
        }
        double det = Sss*Scc - Ssc*Ssc;
        double A = (Sys*Scc - Syc*Ssc) / det, B = (Syc*Sss - Sys*Ssc) / det;
        double sg = (A*A*Sss + 2*A*B*Ssc + B*B*Scc) / na;
        double rs = (Syy - (A*Sys + B*Syc)) / na;
        double s = 10 * log10(sg / rs);
        if (s > sinad) sinad = s;
    }
    free(ya);

    int ok = 1;
    if (st.underflow_frames != 0) { printf("  FAIL underflow=%u\n", st.underflow_frames); ok = 0; }
    if (st.overflow_frames  != 0) { printf("  FAIL overflow=%u\n", st.overflow_frames); ok = 0; }
    if (glitches != 0) { printf("  FAIL glitches=%d maxstep=%.3f\n", glitches, maxstep); ok = 0; }
    if (fabs(rms - 0.70711) > 0.05) { printf("  FAIL rms=%.4f\n", rms); ok = 0; }
    if (fabs(st.est_ppm - drift_ppm) > 5) { printf("  FAIL est %.2f vs %.2f ppm\n", st.est_ppm, drift_ppm); ok = 0; }
    if (st.fill_frames < TARGET/4 || st.fill_frames > 7*TARGET/4) { printf("  FAIL fill=%u\n", st.fill_frames); ok = 0; }
    /* timestamp recovery clears the ~40 dB frame-count floor by a wide margin */
    if (sinad < 80) { printf("  FAIL SINAD=%.1f dB (<80)\n", sinad); ok = 0; }

    printf("freerun(ts): +%.0fppm -> est %.2f ppm  fill %u/%u  rms %.4f  SINAD %.1f dB  maxstep %.3f  under %u over %u glitch %d -> %s\n",
           drift_ppm, st.est_ppm, st.fill_frames, TARGET, rms, sinad, maxstep,
           st.underflow_frames, st.overflow_frames, glitches, ok ? "PASS" : "FAIL");
    harp_freerun_free(fr);
    return ok ? 0 : 1;
}
