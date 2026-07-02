/* §8.3 / §8.7 HIGH-FREQUENCY converter-quality gate — the audio-quality claim made
 * EXPLICIT and GATED, not implied.
 *
 * harp_rtp_tests.c streams a 1 kHz sine end-to-end through the RTP -> free-running
 * receiver -> ASRC path and gates a windowed sine-fit SINAD at 85 dB. Its own note
 * (tests/harp_rtp_tests.c lines ~120-128) is careful about WHY 85 and not 120: the
 * sine-fit SINAD CEILINGS at ~87 dB over a finite block from the recovery jitter + the
 * ±ppm windowed fit — REGARDLESS of converter quality — so that gate CANNOT observe the
 * shipped converter's true ~120 dB stopband. As freerun.h spells out, the stopband shows
 * up NEAR NYQUIST, "where the RMS BAND is what discriminates BEST from MEDIUM/FASTEST"
 * (BEST ~145 dB, flat across the band; MEDIUM only ~117/96 dB at 22/23 kHz; FASTEST ~97).
 *
 * THIS test closes that gap. It streams stepped tones across 18-23 kHz through the SAME
 * RTP->freerun path with arrival jitter SUPPRESSED (so the recovered amplitude reflects
 * the CONVERTER's passband/stopband, not clock-recovery noise) and measures the RECOVERED
 * RMS per tone — the "high-freq rms band". A flat >=120 dB converter holds the HF tone at
 * full amplitude to near Nyquist; MEDIUM/FASTEST roll it off. It gates:
 *   (1) FLAT PASSBAND — the SHIPPED converter (HARP_ASRC_QUALITY) recovers every 18-22 kHz
 *       tone at full amplitude (rms ~0.707). A downgrade to MEDIUM/FASTEST attenuates the
 *       HF tones (observed: FASTEST 0.34 @20 kHz, 0.03 @22 kHz; MEDIUM 0.38 @22 kHz) and
 *       fails this — the converter floor SHOWING (or not) in the rms band.
 *   (2) TOP-BAND DISCRIMINATION — at 22/23 kHz the shipped converter must MATERIALLY beat
 *       SRC_SINC_FASTEST (observed margin 26/45 dB), pinning it as BEST-class near Nyquist —
 *       the property the 1 kHz SINAD gate is structurally blind to.
 * Both the shipped and FASTEST rms are PRINTED for every tone so the audio-quality claim is
 * a logged, gated number. NB: the sine-fit SINAD is deliberately NOT gated here — it is
 * additionally limited by the elastic-buffer control-loop ratio wobble (common to every
 * converter, ~22 dB at 20 kHz in this harness), so it cannot discriminate quality; the RMS
 * band can. This is the same "§8.7 network audio path in miniature, minus the UDP socket"
 * as the 1 kHz e2e — a pure-DSP, hardware-free, every-OS unit test. */
#include "freerun.h"
#include "rtp.h"

#include <math.h>
#include <samplerate.h>
#include <stdio.h>
#include <stdlib.h>

#define HOST 48000.0
#define DEVNOM 48000.0 /* the shipped same-nominal-rate §8.7 free-run: device + host both
                        * 48 kHz, differing only by crystal drift — the reference-device case */
#define BLK 256
#define NSPKT 240
#define TARGET 2048u
#define CAP 8192u

/* Stream a pure sine at `freq` Hz through RTP -> freerun(quality) with a fixed small clock
 * drift and NEGLIGIBLE arrival jitter, then return the recovered output RMS. Near-zero
 * jitter is deliberate: it drops the clock-recovery noise floor far below the converter's
 * HF response so the recovered amplitude READS THE CONVERTER — the rms band that
 * discriminates BEST from MEDIUM/FASTEST near Nyquist. */
static double recovered_rms(double freq, int quality) {
    const double drift_ppm = 50.0, dev_true = DEVNOM * (1.0 + drift_ppm * 1e-6);
    const double prebuf_s = (double)TARGET / HOST, T0 = 100.0;
    const double jit_us = 0.05; /* essentially jitter-free: isolate the converter */
    harp_freerun_cfg cfg = {1, HOST, DEVNOM, TARGET, CAP, quality};
    harp_freerun *fr = harp_freerun_new(&cfg);
    if (!fr) { printf("  FAIL new (quality=%d)\n", quality); return -1.0; }

    const long nblocks = 4000, warmup = 2000;
    float pkt[NSPKT], out[BLK];
    uint8_t wire[HARP_RTP_HDR_BYTES + NSPKT * sizeof(float)];
    long dev_emitted = 0;
    uint64_t ts64 = 0;
    int ts_primed = 0;
    double sumsq = 0;
    long na = 0;

    for (long blk = 0; blk < nblocks; blk++) {
        double t = blk * BLK / HOST;
        for (;;) {
            long k = dev_emitted / NSPKT;
            double jit = (((double)((k * 1103515245L + 12345L) % 2001L)) - 1000.0) * (jit_us * 1e-6 / 1000.0);
            double arr = (double)dev_emitted / dev_true - prebuf_s + jit;
            if (arr > t) break;
            for (int i = 0; i < NSPKT; i++) pkt[i] = (float)sin(2 * M_PI * freq * (dev_emitted + i) / DEVNOM);
            harp_rtp_hdr h = {96, 0, (uint16_t)k, (uint32_t)dev_emitted, 0x1234u};
            int n = harp_rtp_pack(wire, sizeof wire, &h, pkt, sizeof pkt);
            harp_rtp_hdr g; const uint8_t *pl; size_t pln;
            if (harp_rtp_unpack(wire, (size_t)n, &g, &pl, &pln) != 0) { printf("  FAIL hf unpack\n"); harp_freerun_free(fr); return -1.0; }
            ts64 = ts_primed ? harp_rtp_unwrap_ts(g.timestamp, ts64) : g.timestamp;
            ts_primed = 1;
            harp_freerun_observe(fr, ts64, (unsigned long long)((arr + T0) * 1e9));
            harp_freerun_push(fr, (const float *)pl, (unsigned)(pln / sizeof(float)));
            dev_emitted += NSPKT;
        }
        harp_freerun_pull(fr, out, BLK);
        if (blk > warmup)
            for (int i = 0; i < BLK; i++) sumsq += out[i] * out[i];
        if (blk > warmup) na += BLK;
    }
    harp_freerun_free(fr);
    return sqrt(sumsq / (double)na);
}

int main(void) {
    /* stepped sweep across the top band. 18-22 kHz: the flat-passband gate (BEST holds full
     * amplitude; a downgrade rolls off). 22-23 kHz: the top-band discrimination gate (BEST
     * vs FASTEST margin near Nyquist). 23 kHz (0.479 fs) is near enough to Nyquist that even
     * BEST rolls off a little (~0.56), so it is a discrimination tone, NOT a flat-passband one. */
    static const double freqs[] = {18000, 19000, 20000, 21000, 22000, 23000};
    const int NF = (int)(sizeof freqs / sizeof freqs[0]);

    const double FLAT_LO = 0.65711, FLAT_HI = 0.75711; /* |rms - 0.70711| <= 0.05 (as the 1 kHz gate) */
    const double FLAT_MAX_HZ = 22000.0;                /* flat-passband gate applies up to here */
    const double TOP_MIN_HZ = 22000.0;                 /* top-band discrimination applies from here */
    const double TOP_MARGIN_DB = 15.0;                 /* shipped must beat FASTEST by this near Nyquist (obs. 26/45) */

    int ok = 1;
    printf("  §8.3 HF rms-band gate: shipped HARP_ASRC_QUALITY=%d vs SRC_SINC_FASTEST(%d), jitter-suppressed\n",
           HARP_ASRC_QUALITY, SRC_SINC_FASTEST);
    printf("  (the sine-fit SINAD ceilings ~87 dB/finite-block AND ~22 dB here under the control-loop wobble —\n");
    printf("   it cannot see the shipped ~120-145 dB stopband; freerun.h: the RMS BAND below discriminates it)\n");
    printf("  freq     shipped-rms   FASTEST-rms   margin(dB)\n");

    for (int i = 0; i < NF; i++) {
        double rms_ship = recovered_rms(freqs[i], HARP_ASRC_QUALITY);
        double rms_fast = recovered_rms(freqs[i], SRC_SINC_FASTEST);
        double margin = 20.0 * log10(rms_ship / (rms_fast > 1e-9 ? rms_fast : 1e-9));
        int flat = freqs[i] <= FLAT_MAX_HZ;
        int top = freqs[i] >= TOP_MIN_HZ;
        printf("  %5.0f Hz   %.4f        %.4f        %5.1f%s%s\n", freqs[i], rms_ship, rms_fast, margin,
               flat ? "   <-flat-passband" : "", top ? " <-top-band" : "");

        /* (1) FLAT PASSBAND: the shipped converter holds the HF tone at full amplitude. A
         *     MEDIUM/FASTEST downgrade attenuates it here (the converter floor showing in the
         *     rms band). */
        if (flat && (rms_ship < FLAT_LO || rms_ship > FLAT_HI)) {
            printf("    FAIL shipped rms %.4f at %.0f Hz outside [%.3f,%.3f] — HF passband not flat (§8.3 converter degraded)\n",
                   rms_ship, freqs[i], FLAT_LO, FLAT_HI);
            ok = 0;
        }
        /* (2) TOP-BAND DISCRIMINATION: near Nyquist the shipped converter must materially beat
         *     FASTEST — the BEST-class property the 1 kHz gate cannot see. */
        if (top && margin < TOP_MARGIN_DB) {
            printf("    FAIL top-band margin %.1f dB < %.1f dB at %.0f Hz (shipped converter not BEST-class near Nyquist)\n",
                   margin, TOP_MARGIN_DB, freqs[i]);
            ok = 0;
        }
        /* dominance sanity across the whole sweep: the shipped converter never loses to FASTEST. */
        if (rms_ship < rms_fast - 1e-6) { printf("    FAIL shipped rms below FASTEST at %.0f Hz\n", freqs[i]); ok = 0; }
    }

    printf("rtp-hf: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
