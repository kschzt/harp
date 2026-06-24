/* tools/eth-rtp-test — end-to-end §8.7 Ethernet audio-plane check.
 *
 *   eth-rtp-test HOST:PORT [secs] [rtp_port]
 *
 * Dials the device's TCP framed link, says hello, then audio.start with a
 * negotiated RTP destination port (key 6) — the device derives our IP from the
 * TCP peer and streams its render over RTP/UDP to us. We recover the device's
 * rate from arrival timing, resample to the local 48 kHz, and report drift,
 * SINAD, buffer occupancy, and wire counters.
 *
 * This is the Ethernet analog of golden-test.sh: it proves the device emits
 * clean RTP driven by a TCP audio.start (Task 3 integration) and that the host
 * rtp_udp + freerun recovery deliver RME-grade audio over a real network. SINAD
 * is measured on the LEFT channel of the device's stereo main mix; the default
 * drone is harmonically rich, so SINAD reads conservatively — drift, zero loss
 * and zero underflow are the clean-delivery proof. (For the pure-tone ASRC
 * ceiling, see rtp-demo loopback.)
 */
#include "client.h"
#include "rtp_udp.h"
#include "sock_io.h"

#include "harp/cbor.h"
#include "harp/envelope.h"
#include "harp/link.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 48000.0
#define BLK  256
#define CH   2 /* the device's stereo main mix {0,1} */

static unsigned long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
static void sleep_until(unsigned long long target) {
    /* Sample-accurate pull pacing. nanosleep's ms-scale granularity jitters the
     * pull cadence, which wobbles the elastic-buffer fill -> the recovery's
     * centering trim wobbles the resample ratio -> FM on the tone (a HARNESS
     * artifact, not the transport). A DAW pulls on the hardware audio clock,
     * rock-steady; HARP_SPIN_PULL=1 busy-waits here to mimic that, so we measure
     * the recovery and not our own sleep. Default keeps the cheap nanosleep. */
    static int spin = -1;
    if (spin < 0) { const char *e = getenv("HARP_SPIN_PULL"); spin = (e && e[0] == '1'); }
    if (spin) {
        while (now_ns() < target) { /* busy-wait to the exact deadline */ }
        return;
    }
    unsigned long long n = now_ns();
    if (target <= n) return;
    struct timespec d = {.tv_sec = (long)((target - n) / 1000000000ull),
                         .tv_nsec = (long)((target - n) % 1000000000ull)};
    nanosleep(&d, NULL);
}

/* Coarse-then-fine fit of the dominant tone, then SINAD at that tone (signal
 * power in the fitted sinusoid over the residual). Lifted from rtp-demo's
 * run_recv. `tone_out` gets the fitted frequency. */
static double measure_sinad(const float *y, long n, double *tone_out) {
    long wn = n < 48000 ? n : 48000;
    if (wn <= 2000) { *tone_out = 0; return -1e9; }
    const float *yw = y + (n - wn);
    double bestf = 0;
    for (int pass = 0; pass < 2; pass++) {
        double lo = pass ? bestf - 2 : 40, hi = pass ? bestf + 2 : 4000;
        double step = pass ? 0.05 : 2.0, bestamp = -1;
        for (double fo = lo; fo <= hi; fo += step) {
            double Sss = 0, Scc = 0, Ssc = 0, Sys = 0, Syc = 0;
            for (long j = 0; j < wn; j++) {
                double ph = 2 * M_PI * fo * j / RATE, s = sin(ph), c = cos(ph), v = yw[j];
                Sss += s * s; Scc += c * c; Ssc += s * c; Sys += v * s; Syc += v * c;
            }
            double det = Sss * Scc - Ssc * Ssc;
            if (det <= 0) continue;
            double A = (Sys * Scc - Syc * Ssc) / det, B = (Syc * Sss - Sys * Ssc) / det;
            double amp = A * A + B * B;
            if (amp > bestamp) { bestamp = amp; bestf = fo; }
        }
    }
    *tone_out = bestf;
    if (bestf <= 0) return -1e9;
    double Sss = 0, Scc = 0, Ssc = 0, Sys = 0, Syc = 0, Syy = 0;
    for (long j = 0; j < wn; j++) {
        double ph = 2 * M_PI * bestf * j / RATE, s = sin(ph), c = cos(ph), v = yw[j];
        Sss += s * s; Scc += c * c; Ssc += s * c; Sys += v * s; Syc += v * c; Syy += v * v;
    }
    double det = Sss * Scc - Ssc * Ssc;
    double A = (Sys * Scc - Syc * Ssc) / det, B = (Syc * Sss - Sys * Ssc) / det;
    return 10 * log10((A * A * Sss + 2 * A * B * Ssc + B * B * Scc) / wn /
                      ((Syy - (A * Sys + B * Syc)) / wn));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: eth-rtp-test HOST:PORT [secs] [rtp_port]\n");
        return 2;
    }
    const char *hostport = argv[1];
    double secs = argc > 2 ? atof(argv[2]) : 8.0;
    int rtp_port = argc > 3 ? atoi(argv[3]) : 47811;
    int note_pitch = argc > 4 ? atoi(argv[4]) : 0; /* >0: send a held note-on (single-session audibility test) */
    uint32_t rate = 48000, nsamples = 256;

    /* 1. dial the device's TCP framed link */
    harp_sockhandle s = harp_sock_dial(hostport);
    if (s == HARP_SOCK_INVALID) return 1;
    harp_sock_io tio;
    harp_sock_io_init(&tio, s);

    harp_link link;
    harp_link_init(&link);
    harp_client client;
    harp_client_init(&client, &tio.io, &link, NULL, NULL, NULL);
    harp_client_identity id;
    if (harp_client_hello(&client, "eth-rtp-test 0.1", &id) != 0) {
        fprintf(stderr, "eth-rtp: hello failed (%s)\n", client.err_code);
        return 1;
    }

    /* 2. bind the RTP receiver BEFORE audio.start so we miss no packet */
    harp_freerun_cfg cfg = {CH, RATE, RATE, 2048, 8192, 0 /* SRC_SINC_BEST_QUALITY */};
    harp_rtp_rx *rx = harp_rtp_rx_open(rtp_port, &cfg);
    if (!rx) {
        fprintf(stderr, "eth-rtp: cannot bind RTP port %d\n", rtp_port);
        return 1;
    }

    /* 3. audio.start, free-running, with our RTP port (key 6) */
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client, &req, "audio.start", true);
    harp_cbor_map(&req, 4);
    harp_cbor_uint(&req, 0); harp_cbor_uint(&req, rate);
    harp_cbor_uint(&req, 1); harp_cbor_uint(&req, nsamples);
    harp_cbor_uint(&req, 5); harp_cbor_uint(&req, 0);                 /* free-running */
    harp_cbor_uint(&req, 6); harp_cbor_uint(&req, (uint64_t)rtp_port); /* RTP dest port */
    harp_env e;
    int rc = harp_client_request(&client, &req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc != 0) {
        fprintf(stderr, "eth-rtp: audio.start failed (rc=%d code=%s msg=%s)\n", rc,
                client.err_code, client.err_msg);
        harp_rtp_rx_close(rx);
        return 1;
    }
    fprintf(stderr, "eth-rtp: audio.start ok -> device RTP -> us:%d (48k stereo, free-running)\n",
            rtp_port);

    /* 3b. SINGLE-SESSION audibility: send a held note-on on THIS session's evt stream
     * (§9.10 MIDI-1.0-in-UMP at ts 0 = asap), same encoding as harp-probe's send_ump_note. */
    if (note_pitch > 0) {
        uint32_t word = 0x20900000u | ((uint32_t)(note_pitch & 0x7f) << 8) | 100u; /* ch0 note-on vel100 */
        uint8_t nb[4] = {(uint8_t)(word >> 24), (uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word};
        harp_cbuf ev;
        harp_cbuf_init(&ev);
        harp_cbor_array(&ev, 3);
        harp_cbor_array(&ev, 2);
        harp_cbor_uint(&ev, 0); /* epoch 0 = now */
        harp_cbor_uint(&ev, 0); /* msc 0 = asap */
        harp_cbor_uint(&ev, 0); /* etype 0 = UMP carriage */
        harp_cbor_bytes(&ev, nb, 4);
        harp_link_send(&tio.io, HARP_STREAM_EVT, ev.buf, ev.len);
        harp_cbuf_free(&ev);
        fprintf(stderr, "eth-rtp: sent note-on pitch=%d vel=100 (single session)\n", note_pitch);
    }

    /* 4. prebuffer the elastic buffer to ~target before output starts */
    harp_freerun_stats pre;
    for (int i = 0; i < 4000; i++) {
        harp_rtp_rx_poll(rx, 5);
        harp_rtp_rx_stats(rx, &pre);
        if (pre.fill_frames >= cfg.target_frames) break;
    }

    /* 5. pull host-rate stereo blocks for `secs`; capture L after a 1 s warmup */
    long nblk = (long)(secs * RATE / BLK), warm = (long)(RATE / BLK);
    float out[BLK * CH];
    long cap = (nblk > warm ? nblk - warm : 1) * BLK;
    float *yl = malloc(sizeof(float) * (size_t)cap);
    long na = 0;
    unsigned long long t0 = now_ns();
    for (long b = 0; b < nblk; b++) {
        harp_rtp_rx_poll(rx, 0);
        harp_rtp_rx_pull(rx, out, BLK);
        if (b > warm)
            for (int i = 0; i < BLK && na < cap; i++) yl[na++] = out[i * CH]; /* L */
        sleep_until(t0 + (unsigned long long)((double)(b + 1) * BLK / RATE * 1e9));
    }

    /* 6. audio.stop */
    harp_cbuf sreq, srsp;
    harp_cbuf_init(&sreq);
    harp_cbuf_init(&srsp);
    harp_client_req_head(&client, &sreq, "audio.stop", false);
    harp_env se;
    harp_client_request(&client, &sreq, &srsp, &se);
    harp_cbuf_free(&sreq);
    harp_cbuf_free(&srsp);

    /* 7. report */
    harp_freerun_stats st;
    harp_rtp_rx_stats(rx, &st);
    unsigned long ok, lost, bad;
    harp_rtp_rx_counters(rx, &ok, &lost, &bad);
    double sumsq = 0;
    for (long j = 0; j < na; j++) sumsq += (double)yl[j] * yl[j];
    double rms = na ? sqrt(sumsq / na) : 0;
    double tone = 0, sinad = measure_sinad(yl, na, &tone);
    printf("eth-rtp: drift %.2f ppm  jitter %.0f us  fill %u  RMS %.4f  tone %.1f Hz  "
           "SINAD %.1f dB  pkts ok=%lu lost=%lu bad=%lu under=%u over=%u\n",
           st.est_ppm, st.jitter_us, st.fill_frames, rms, tone, sinad, ok, lost, bad,
           st.underflow_frames, st.overflow_frames);
    int clean = (na > 0 && rms > 0.001 && lost == 0 && bad == 0 && st.underflow_frames == 0);
    printf("%s\n", clean ? "ETH-RTP PASS (clean recovery: signal present, no loss/underflow)"
                         : "ETH-RTP CHECK (inspect counters above)");

    free(yl);
    harp_rtp_rx_close(rx);
    harp_client_free(&client);
    harp_sock_close(s);
    return clean ? 0 : 1;
}
