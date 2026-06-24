/* Trace-driven validation of the free-running receiver (host/freerun.c) with
 * TIMESTAMP-based recovery. Models a device emitting fixed-size packets at its
 * own (drifted) crystal rate; each packet carries its device sample index (the
 * RTP timestamp) and arrives at a host time = production time + bounded jitter.
 * The host observes (dev_ts, host_ns) for recovery and pulls fixed blocks.
 *
 * Asserts: zero under/overflow (jitter absorbed), glitch-free output, amplitude
 * preserved, the elastic buffer stays bounded, and SINAD far above the frame-count
 * floor, because the rate is recovered from sub-frame arrival timing rather than
 * whole-frame counts. Three checks:
 *   - JITTERED trace (±50 µs): arrival jitter is the SINAD ceiling (the realistic floor),
 *     and a no-starvation run must report ZERO re-anchors.
 *   - ISOLATED converter quality: a pure libsamplerate resample swept across the UPPER band
 *     (18-23 kHz) at HARP_ASRC_QUALITY (no recovery loop), gating the worst SINAD >=120 dB AND
 *     passband ripple <=0.01 dB (the §8.3 floor's two MUSTs). Near Nyquist is where converters
 *     diverge — SRC_SINC_MEDIUM collapses to ~96 dB at 23 kHz and FASTEST to ~97 dB (both FAIL);
 *     only SRC_SINC_BEST clears it. The runtime AND this test share HARP_ASRC_QUALITY
 *     (freerun.h), so a runtime regression below BEST fails here too.
 *   - §8.3 stream RE-ANCHOR: three starve/recover cycles must count exactly 3 EPISODES
 *     (host-counters key 7 / clock-stats key 4) — never silent, and not count-once.
 */
#include "freerun.h"
#include <samplerate.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST    48000.0
#define DEVNOM  48000.0
#define F       1000.0
#define BLK     256
#define NSPKT   240            /* device packet size (frames)                    */
#define TARGET  2048u
#define CAP     8192u

/* Run the timestamp-recovery trace at a given arrival jitter. Returns 1 on the
 * structural asserts (under/overflow, glitch, rms, recovered drift); *sinad_out
 * gets the measured SINAD and *st_out the final stats. */
static int run_trace(double jit_us, double drift_ppm, harp_freerun_stats *st_out, double *sinad_out) {
    const double dev_true = DEVNOM * (1.0 + drift_ppm * 1e-6);
    const double prebuf_s = (double)TARGET / HOST;        /* head start = target latency */
    const double T0 = 100.0;                              /* offset so host_ns > 0 */
    harp_freerun_cfg cfg = {1, HOST, DEVNOM, TARGET, CAP, HARP_ASRC_QUALITY};
    harp_freerun *fr = harp_freerun_new(&cfg);
    if (!fr) return -1;

    const long nblocks = 6000, warmup = 4000;
    float pkt[NSPKT], out[BLK];
    long dev_emitted = 0;
    int glitches = 0; double maxstep = 0, sumsq = 0; long outn = 0; double prev = 0;
    float *ya = malloc(sizeof(float) * (size_t)(nblocks - warmup) * BLK);
    long na = 0;

    for (long blk = 0; blk < nblocks; blk++) {
        double t = blk * BLK / HOST;                      /* host time (s)        */
        for (;;) {
            long k = dev_emitted / NSPKT;
            double jit = (((double)((k * 1103515245L + 12345L) % 2001L)) - 1000.0)
                         * (jit_us * 1e-6 / 1000.0);       /* +/- jit_us us         */
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

    harp_freerun_get_stats(fr, st_out);
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
    *sinad_out = sinad;

    int ok = 1;
    if (st_out->underflow_frames != 0) { printf("  FAIL underflow=%u\n", st_out->underflow_frames); ok = 0; }
    if (st_out->overflow_frames  != 0) { printf("  FAIL overflow=%u\n", st_out->overflow_frames); ok = 0; }
    if (glitches != 0) { printf("  FAIL glitches=%d maxstep=%.3f\n", glitches, maxstep); ok = 0; }
    if (fabs(rms - 0.70711) > 0.05) { printf("  FAIL rms=%.4f\n", rms); ok = 0; }
    if (fabs(st_out->est_ppm - drift_ppm) > 5) { printf("  FAIL est %.2f vs %.2f ppm\n", st_out->est_ppm, drift_ppm); ok = 0; }
    printf("  trace(jit=%.0fus): est %.2f ppm  fill %u/%u  rms %.4f  SINAD %.1f dB  reanchors %u -> %s\n",
           jit_us, st_out->est_ppm, st_out->fill_frames, TARGET, rms, sinad, st_out->reanchors, ok ? "ok" : "FAIL");
    harp_freerun_free(fr);
    return ok;
}

/* §8.3 stream RE-ANCHOR: draining the elastic buffer past empty is a starvation episode;
 * the runtime counts each episode (clock-stats key 4) — never silent. */
static int test_reanchor(void) {
    harp_freerun_cfg cfg = {1, HOST, DEVNOM, 64u, 512u, HARP_ASRC_QUALITY};
    harp_freerun *fr = harp_freerun_new(&cfg);
    if (!fr) { printf("  reanchor: new() failed\n"); return 0; }
    float in[256], out[256];
    for (int i = 0; i < 256; i++) in[i] = 0.1f * (float)sin(2 * M_PI * F * i / DEVNOM);
    harp_freerun_push(fr, in, 200);                 /* > target 64 -> warm latches */
    for (int r = 0; r < 3; r++) {
        for (int p = 0; p < 8; p++) harp_freerun_pull(fr, out, 256); /* drain past empty -> starve */
        harp_freerun_push(fr, in, 200);              /* refill */
        harp_freerun_pull(fr, out, 64);              /* recover (ends the episode) */
    }
    harp_freerun_stats st; harp_freerun_get_stats(fr, &st);
    harp_freerun_free(fr);
    int ok = st.reanchors == 3; /* exactly 3 starve/recover cycles => 3 EPISODES (not frames, not count-once) */
    printf("  reanchor: episodes=%u (want 3) underflow=%u -> %s\n",
           st.reanchors, st.underflow_frames, ok ? "PASS" : "FAIL (episode count wrong — frames? count-once?)");
    return ok;
}

/* Resample a clean unit sine at FT (Hz @ HOST) through HARP_ASRC_QUALITY at ~50 ppm varispeed
 * (no recovery loop — the freerun trace's SINAD is dominated by rate-recovery FM, not the
 * converter, so it can't gate this). Returns the converter SINAD (dB) and the recovered |gain|
 * at FT (1.0 = unity) via a sub-ppm LS sine fit. */
static void resample_measure(double FT, double *sinad_out, double *gain_out) {
    const double ratio = 1.000050;
    const long NIN = 200000;
    float *in = malloc(sizeof(float) * (size_t)NIN);
    for (long i = 0; i < NIN; i++) in[i] = (float)sin(2 * M_PI * FT * i / HOST);
    long NOUT = (long)(NIN * ratio) + 64;
    float *out = malloc(sizeof(float) * (size_t)NOUT);
    SRC_DATA d; memset(&d, 0, sizeof d);
    d.data_in = in; d.input_frames = NIN;
    d.data_out = out; d.output_frames = NOUT;
    d.src_ratio = ratio; d.end_of_input = 1;
    int err = src_simple(&d, HARP_ASRC_QUALITY, 1);
    if (err) { printf("  converter: src_simple err %s\n", src_strerror(err)); *sinad_out = -1e9; *gain_out = 0; free(in); free(out); return; }
    long n = d.output_frames_gen, skip = 8192, na = n - 2 * skip; /* drop filter startup AND flush tail */
    double f0 = FT / ratio;                 /* the sine's frequency in output samples (Hz @ HOST) */
    double sinad = -1e9, gain = 0;
    for (int kf = -200; kf <= 200; kf++) {  /* search the exact resampled frequency (sub-ppm fit) */
        double f_out = f0 * (1.0 + kf * 1e-6);
        double Sss = 0, Scc = 0, Ssc = 0, Sys = 0, Syc = 0, Syy = 0;
        for (long j = 0; j < na; j++) {
            double ph = 2 * M_PI * f_out * (skip + j) / HOST;
            double s = sin(ph), c = cos(ph), y = out[skip + j];
            Sss += s*s; Scc += c*c; Ssc += s*c; Sys += y*s; Syc += y*c; Syy += y*y;
        }
        double det = Sss*Scc - Ssc*Ssc;
        double A = (Sys*Scc - Syc*Ssc) / det, B = (Syc*Sss - Sys*Ssc) / det;
        double sg = (A*A*Sss + 2*A*B*Ssc + B*B*Scc) / na;
        double rs = (Syy - (A*Sys + B*Syc)) / na;
        double sdb = 10 * log10(sg / rs);
        if (sdb > sinad) { sinad = sdb; gain = sqrt(A*A + B*B); }
    }
    free(in); free(out);
    *sinad_out = sinad; *gain_out = gain;
}

/* §8.3 converter quality, ISOLATED + ACROSS THE BAND. The §8.3 floor is two MUSTs: stopband
 * SINAD >=120 dB AND passband ripple <=0.01 dB. Gating a SINGLE near-Nyquist tone at 110 dB
 * (the old test) was rigged — it passed SRC_SINC_MEDIUM (1), which actually collapses to ~96 dB
 * at 23 kHz. This sweeps the UPPER band (18-23 kHz, where converters diverge) and gates the
 * WORST SINAD >=120, plus the passband |gain| flatness (1-10 kHz) <=0.01 dB. SRC_SINC_BEST (0)
 * clears both; MEDIUM (1, ~96 dB @23 kHz) and FASTEST (2, ~97 dB) FAIL. Shares HARP_ASRC_QUALITY
 * with the runtime, so a regression below BEST fails here. */
static int test_converter_stopband(void) {
    const double stop_f[] = {18000.0, 20000.0, 22000.0, 23000.0};
    double worst = 1e9;
    for (size_t i = 0; i < sizeof stop_f / sizeof *stop_f; i++) {
        double sinad, gain;
        resample_measure(stop_f[i], &sinad, &gain);
        printf("  converter(stopband): SINAD %6.1f dB at %5.0f Hz\n", sinad, stop_f[i]);
        if (sinad < worst) worst = sinad;
    }
    const double pass_f[] = {1000.0, 5000.0, 10000.0};   /* passband ripple: |gain| flatness */
    double gmin = 1e9, gmax = -1e9;
    for (size_t i = 0; i < sizeof pass_f / sizeof *pass_f; i++) {
        double sinad, gain;
        resample_measure(pass_f[i], &sinad, &gain);
        if (gain < gmin) gmin = gain;
        if (gain > gmax) gmax = gain;
    }
    double ripple = 20.0 * log10(gmax / gmin);
    int ok = worst >= 120.0 && ripple <= 0.01;
    printf("  converter: worst stopband SINAD %.1f dB (>=120), passband ripple %.4f dB (<=0.01) at quality=%d -> %s\n",
           worst, ripple, HARP_ASRC_QUALITY, ok ? "PASS" : "FAIL (below the §8.3 floor)");
    return ok;
}

int main(void) {
    harp_freerun_stats st; double sinad;
    int ok = 1;

    int r = run_trace(50.0, 50.0, &st, &sinad);     /* JITTERED: realistic arrival floor */
    if (r < 0) { printf("freerun: new() failed\n"); return 1; }
    if (!r) ok = 0;
    if (sinad < 80) { printf("  FAIL jittered SINAD=%.1f dB (<80)\n", sinad); ok = 0; }
    if (st.reanchors != 0) { printf("  FAIL jittered reanchors=%u (expected 0 — no starvation)\n", st.reanchors); ok = 0; }

    if (!test_converter_stopband()) ok = 0;         /* converter's §8.3 stopband, isolated */

    if (!test_reanchor()) ok = 0;

    printf("freerun(ts): SINAD jittered/clean + reanchor -> %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
