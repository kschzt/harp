/* tools/eth-bitexact-test — prove §8.7 BIT-EXACT (host-locked) Ethernet audio.
 *
 *   eth-bitexact-test HOST:PORT [secs] [rtp_port]
 *
 * Unlike eth-rtp-test (ASRC: recover the device rate, then resample), this plays
 * the device's RTP samples 1:1 — NO resampling — and instead closes a slow
 * feedback loop: it watches its own jitter-buffer fill and streams audio.trim
 * corrections over the framed link so the DEVICE emits at exactly the host's
 * consumption rate. The buffer then only absorbs jitter; every sample passes
 * untouched = bit-exact. Because nothing is resampled, jitter can no longer
 * distort the audio — it can only move the buffer fill (which the loop holds).
 *
 * Proof of bit-exact = (1) SINAD far above the ASRC path (no resampling
 * distortion — limited by the source tone + the meter, not the transport),
 * (2) zero under/overflow, (3) a buffer that stays near its setpoint. The
 * device's pitch locks a few ppm off nominal — inaudible, and correct, since
 * everything downstream is on the host clock.
 *
 * Tunables (env): HARP_KP, HARP_KI (loop gains), HARP_TARGET (buffer setpoint).
 */
#include "client.h"
#include "rtp.h"
#include "sock_io.h"

#include "harp/cbor.h"
#include "harp/envelope.h"
#include "harp/link.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RATE 48000.0
#define BLK  256
#define CH   2
#define FIFO_FRAMES 131072u /* power of 2; ample headroom for the prototype */
#define CAPWIN      96000   /* last 2 s of L kept for the SINAD fit (bounded for soaks) */

static unsigned long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
static void spin_until(unsigned long long target) {
    while (now_ns() < target) { /* sample-accurate pull pacing (host clock) */ }
}

/* ---- stereo SPSC ring: recv thread pushes, main thread pulls ---- */
static float              g_fifo[FIFO_FRAMES * CH];
static _Atomic uint64_t   g_head, g_tail; /* monotonic frame indices */
static _Atomic int        g_stop;
static _Atomic unsigned   g_under, g_over;
static int                g_sock = -1;

static void fifo_push(const float *src, unsigned n) {
    uint64_t head = atomic_load_explicit(&g_head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&g_tail, memory_order_acquire);
    unsigned space = (unsigned)(FIFO_FRAMES - (head - tail));
    if (n > space) {
        atomic_fetch_add_explicit(&g_over, n - space, memory_order_relaxed);
        n = space;
    }
    for (unsigned i = 0; i < n; i++) {
        unsigned s = (unsigned)((head + i) & (FIFO_FRAMES - 1));
        g_fifo[s * CH] = src[i * CH];
        g_fifo[s * CH + 1] = src[i * CH + 1];
    }
    atomic_store_explicit(&g_head, head + n, memory_order_release);
}
static void fifo_pull(float *dst, unsigned n) {
    uint64_t tail = atomic_load_explicit(&g_tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&g_head, memory_order_acquire);
    unsigned avail = (unsigned)(head - tail);
    unsigned take = n < avail ? n : avail;
    for (unsigned i = 0; i < take; i++) {
        unsigned s = (unsigned)((tail + i) & (FIFO_FRAMES - 1));
        dst[i * CH] = g_fifo[s * CH];
        dst[i * CH + 1] = g_fifo[s * CH + 1];
    }
    for (unsigned i = take; i < n; i++) { dst[i * CH] = 0; dst[i * CH + 1] = 0; }
    if (take < n) atomic_fetch_add_explicit(&g_under, n - take, memory_order_relaxed);
    atomic_store_explicit(&g_tail, tail + take, memory_order_release);
}
static unsigned fifo_fill(void) {
    uint64_t head = atomic_load_explicit(&g_head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&g_tail, memory_order_acquire);
    return (unsigned)(head - tail);
}

static void *recv_thread(void *arg) {
    (void)arg;
    uint8_t buf[16384];
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        struct pollfd pfd = {g_sock, POLLIN, 0};
        int pr = poll(&pfd, 1, 100);
        if (pr <= 0) continue;
        ssize_t n = recv(g_sock, buf, sizeof buf, 0);
        if (n < 0) continue;
        harp_rtp_hdr h;
        const uint8_t *pl;
        size_t pln;
        if (harp_rtp_unpack(buf, (size_t)n, &h, &pl, &pln) != 0) continue;
        if (pln % (CH * sizeof(float))) continue;
        fifo_push((const float *)pl, (unsigned)(pln / (CH * sizeof(float))));
    }
    return NULL;
}

/* coarse->fine dominant-tone SINAD (same as eth-rtp-test) */
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
        fprintf(stderr, "usage: eth-bitexact-test HOST:PORT [secs] [rtp_port]\n");
        return 2;
    }
    const char *hostport = argv[1];
    double secs = argc > 2 ? atof(argv[2]) : 10.0;
    int rtp_port = argc > 3 ? atoi(argv[3]) : 47820;
    uint32_t rate = 48000, nsamples = 256;
    unsigned target = (unsigned)(getenv("HARP_TARGET") ? atoi(getenv("HARP_TARGET")) : 2048);
    /* Proportional-only by default: control of the buffer (an integrator) by a
     * single gain is FIRST-ORDER -> unconditionally stable, no oscillation, no
     * windup. The cost is a tiny steady-state fill offset (trim_ss / Kp frames),
     * which is harmless. Ki>0 adds an integral (zeroes that offset) but needs
     * the anti-windup below. Fill is EMA-smoothed so the gain reacts to the slow
     * rate-offset trend, not packet jitter. */
    double Kp = getenv("HARP_KP") ? atof(getenv("HARP_KP")) : 2000.0; /* ppb / frame */
    double Ki = getenv("HARP_KI") ? atof(getenv("HARP_KI")) : 0.0;    /* ppb / (frame*s) */
    double ema = getenv("HARP_EMA") ? atof(getenv("HARP_EMA")) : 0.03; /* fill smoothing (~1.7 s) */

    /* 1. dial the framed link + hello */
    harp_sockhandle s = harp_sock_dial(hostport);
    if (s == HARP_SOCK_INVALID) return 1;
    harp_sock_io tio;
    harp_sock_io_init(&tio, s);
    harp_link link;
    harp_link_init(&link);
    harp_client client;
    harp_client_init(&client, &tio.io, &link, NULL, NULL, NULL);
    harp_client_identity id;
    if (harp_client_hello(&client, "eth-bitexact-test 0.1", &id) != 0) {
        fprintf(stderr, "bitexact: hello failed (%s)\n", client.err_code);
        return 1;
    }

    /* 2. bind our RTP socket + start the receive thread (no timestamps needed) */
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)rtp_port);
    if (bind(g_sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "bitexact: cannot bind RTP port %d\n", rtp_port);
        return 1;
    }
    pthread_t rxt;
    pthread_create(&rxt, NULL, recv_thread, NULL);

    /* 3. audio.start, free-running, with our RTP port (key 6) */
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client, &req, "audio.start", true);
    harp_cbor_map(&req, 4);
    harp_cbor_uint(&req, 0); harp_cbor_uint(&req, rate);
    harp_cbor_uint(&req, 1); harp_cbor_uint(&req, nsamples);
    harp_cbor_uint(&req, 5); harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 6); harp_cbor_uint(&req, (uint64_t)rtp_port);
    harp_env e;
    int rc = harp_client_request(&client, &req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc != 0) {
        fprintf(stderr, "bitexact: audio.start failed (rc=%d %s)\n", rc, client.err_code);
        return 1;
    }
    fprintf(stderr, "bitexact: 1:1 play + audio.trim feedback (target %u frames, Kp %.1f Ki %.1f)\n",
            target, Kp, Ki);

    /* 4. prebuffer to the setpoint before output starts */
    for (int i = 0; i < 4000 && fifo_fill() < target; i++) {
        struct timespec d = {0, 1000000};
        nanosleep(&d, NULL);
    }

    /* 5. pull 1:1 at the host clock; run the slow trim loop; capture L for SINAD */
    long nblk = (long)(secs * RATE / BLK), warm = (long)(RATE / BLK);
    float out[BLK * CH];
    static float yl[CAPWIN]; /* circular: last 2 s of L (bounded for long soaks) */
    long na = 0;
    int tick = (int)(0.05 * RATE / BLK);
    if (tick < 1) tick = 1; /* ~50 ms control tick */
    double dt = (double)tick * BLK / RATE;
    double integ = 0, trim = 0, sm_fill = target;
    unsigned fmin = (unsigned)-1, fmax = 0;
    unsigned long long t0 = now_ns(), last_log = t0;
    for (long b = 0; b < nblk; b++) {
        fifo_pull(out, BLK);
        if (b > warm)
            for (int i = 0; i < BLK; i++) { yl[na % CAPWIN] = out[i * CH]; na++; }
        if (b % tick == 0) {
            unsigned fill = fifo_fill();
            if (fill < fmin) fmin = fill;
            if (fill > fmax) fmax = fill;
            sm_fill += ema * ((double)fill - sm_fill); /* EMA: keep jitter out of the loop */
            double err = sm_fill - (double)target;     /* >0 => buffer too full */
            if (Ki > 0) {                              /* optional integral, with anti-windup */
                integ += err * dt;
                double imax = 190000.0 / Ki;           /* the integral alone can't saturate trim */
                if (integ > imax) integ = imax;
                else if (integ < -imax) integ = -imax;
            }
            trim = -(Kp * err + Ki * integ);           /* too full => slow the device */
            if (trim > 200000) trim = 200000;
            else if (trim < -200000) trim = -200000;
            harp_cbuf tr;
            harp_cbuf_init(&tr);
            harp_client_req_head(&client, &tr, "audio.trim", true);
            harp_cbor_map(&tr, 1);
            harp_cbor_uint(&tr, 0);
            harp_cbor_float(&tr, (float)trim);
            harp_client_send(&client, &tr); /* fire-and-forget */
            harp_cbuf_free(&tr);
            unsigned long long nowt = now_ns();
            if (nowt - last_log >= 30000000000ull) { /* interim progress every 30 s */
                last_log = nowt;
                fprintf(stderr, "  t=%4.0fs  trim %8.0f ppb  fill %u..%u (set %u)  under %u over %u\n",
                        (double)(nowt - t0) / 1e9, trim, fmin, fmax, target,
                        atomic_load(&g_under), atomic_load(&g_over));
            }
        }
        spin_until(t0 + (unsigned long long)((double)(b + 1) * BLK / RATE * 1e9));
    }

    /* 6. stop rx + audio.stop */
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    shutdown(g_sock, SHUT_RDWR);
    pthread_join(rxt, NULL);
    harp_cbuf sreq, srsp;
    harp_cbuf_init(&sreq);
    harp_cbuf_init(&srsp);
    harp_client_req_head(&client, &sreq, "audio.stop", false);
    harp_env se;
    harp_client_request(&client, &sreq, &srsp, &se);
    harp_cbuf_free(&sreq);
    harp_cbuf_free(&srsp);

    /* 7. report — linearize the last min(na,CAPWIN) samples oldest->newest */
    long have = na < CAPWIN ? na : CAPWIN;
    static float lin[CAPWIN];
    for (long i = 0; i < have; i++) lin[i] = yl[(na - have + i) % CAPWIN];
    double sumsq = 0;
    for (long j = 0; j < have; j++) sumsq += (double)lin[j] * lin[j];
    double rms = have ? sqrt(sumsq / have) : 0;
    double tone = 0, sinad = measure_sinad(lin, have, &tone);
    unsigned under = atomic_load(&g_under), over = atomic_load(&g_over);
    printf("bitexact: trim %.1f ppb  fill %u..%u (target %u)  RMS %.4f  tone %.1f Hz  "
           "SINAD %.1f dB  under %u over %u\n",
           trim, fmin, fmax, target, rms, tone, sinad, under, over);
    int ok = (na > 0 && rms > 0.001 && under == 0 && over == 0);
    printf("%s\n", ok ? "BIT-EXACT PASS (1:1 play, no resampling, buffer held, 0 under/overflow)"
                      : "BIT-EXACT CHECK (inspect counters)");
    harp_sock_close(s);
    return ok ? 0 : 1;
}
