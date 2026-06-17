/* harp rtp-demo — exercise the §8.7 RTP audio path over real UDP.
 *
 *   rtp-demo send <host> <port> [secs]   device: stream a 1 kHz sine
 *   rtp-demo recv <port> [secs]          host: recover + resample, report
 *   rtp-demo loopback [secs]             both, over 127.0.0.1 (one machine)
 *
 * The sender paces off its own monotonic clock (its "crystal"); the receiver
 * recovers that rate from arrival timing and resamples to the local 48 kHz.
 * On one machine (loopback) the two clocks are identical (drift ~0); across two
 * machines (KR260 -> Mac) it shows the real crystal drift. Reports recovered
 * drift, SINAD, buffer occupancy, and wire counters.
 */
#include "rtp_udp.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE   48000.0
#define FREQ   1000.0
#define NSPKT  240
#define BLK    256
#define CH     1

static unsigned long long now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
static void sleep_until(unsigned long long target) {
    unsigned long long n = now_ns();
    if (target <= n) return;
    struct timespec d = {.tv_sec = (target - n) / 1000000000ull, .tv_nsec = (target - n) % 1000000000ull};
    nanosleep(&d, NULL);
}

static void run_send(const char *host, int port, double secs) {
    harp_rtp_tx *tx = harp_rtp_tx_open(host, port, CH, 0x48415250u /* "HARP" */);
    if (!tx) { fprintf(stderr, "send: open failed\n"); exit(1); }
    long npkt = (long)(secs * RATE / NSPKT);
    float pkt[NSPKT];
    unsigned long long t0 = now_ns();
    uint32_t ts = 0;
    for (long i = 0; i < npkt; i++) {
        for (int j = 0; j < NSPKT; j++) pkt[j] = (float)sin(2 * M_PI * FREQ * (ts + j) / RATE);
        harp_rtp_tx_send(tx, ts, pkt, NSPKT);
        ts += NSPKT;
        sleep_until(t0 + (unsigned long long)((double)(i + 1) * NSPKT / RATE * 1e9));
    }
    harp_rtp_tx_close(tx);
    fprintf(stderr, "send: %ld packets done\n", npkt);
}

static void run_recv(int port, double secs) {
    harp_freerun_cfg cfg = {CH, RATE, RATE, 2048, 8192, 2 /* SRC_SINC_MEDIUM_QUALITY */};
    harp_rtp_rx *rx = harp_rtp_rx_open(port, &cfg);
    if (!rx) { fprintf(stderr, "recv: open failed\n"); exit(1); }
    long nblk = (long)(secs * RATE / BLK), warm = (long)(RATE / BLK);   /* 1 s warmup */
    float out[BLK];
    float *ya = malloc(sizeof(float) * (size_t)(nblk > warm ? nblk - warm : 1) * BLK);
    long na = 0;

    /* Prebuffer: fill the elastic buffer to ~target before output starts, so
     * startup and jitter don't underflow (the standard jitter-buffer warm-up). */
    harp_freerun_stats pre;
    for (int i = 0; i < 4000; i++) {
        harp_rtp_rx_poll(rx, 5);
        harp_rtp_rx_stats(rx, &pre);
        if (pre.fill_frames >= cfg.target_frames) break;
    }
    unsigned long long t0 = now_ns();
    for (long b = 0; b < nblk; b++) {
        harp_rtp_rx_poll(rx, 0);                 /* drain arrivals (non-blocking) */
        harp_rtp_rx_pull(rx, out, BLK);
        if (b > warm) for (int i = 0; i < BLK; i++) ya[na++] = out[i];
        sleep_until(t0 + (unsigned long long)((double)(b + 1) * BLK / RATE * 1e9));
    }
    harp_freerun_stats st; harp_rtp_rx_stats(rx, &st);
    unsigned long ok, lost, bad; harp_rtp_rx_counters(rx, &ok, &lost, &bad);

    /* SINAD at the recovered frequency (search +/-300 ppm). */
    double f0 = FREQ * (1.0 + st.est_ppm * 1e-6), sinad = -1e9;
    for (int kf = -300; kf <= 300 && na > 1000; kf++) {
        double fo = f0 * (1.0 + kf * 1e-6), Sss=0,Scc=0,Ssc=0,Sys=0,Syc=0,Syy=0;
        for (long j = 0; j < na; j++) {
            double ph = 2*M_PI*fo*j/RATE, s=sin(ph), c=cos(ph), y=ya[j];
            Sss+=s*s; Scc+=c*c; Ssc+=s*c; Sys+=y*s; Syc+=y*c; Syy+=y*y;
        }
        double det=Sss*Scc-Ssc*Ssc, A=(Sys*Scc-Syc*Ssc)/det, B=(Syc*Sss-Sys*Ssc)/det;
        double v=10*log10((A*A*Sss+2*A*B*Ssc+B*B*Scc)/na/((Syy-(A*Sys+B*Syc))/na));
        if (v>sinad) sinad=v;
    }
    free(ya);
    printf("recv: drift %.2f ppm  fill %u  SINAD %.1f dB  pkts ok=%lu lost=%lu bad=%lu  under=%u over=%u\n",
           st.est_ppm, st.fill_frames, sinad, ok, lost, bad, st.underflow_frames, st.overflow_frames);
    harp_rtp_rx_close(rx);
}

struct sender_args { int port; double secs; };
static void *sender_thread(void *p) {
    struct sender_args *a = p;
    struct timespec d = {0, 200000000}; nanosleep(&d, NULL);   /* let recv bind first */
    run_send("127.0.0.1", a->port, a->secs);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: rtp-demo send|recv|loopback ...\n"); return 2; }
    if (!strcmp(argv[1], "send") && argc >= 4) {
        run_send(argv[2], atoi(argv[3]), argc > 4 ? atof(argv[4]) : 10.0);
    } else if (!strcmp(argv[1], "recv") && argc >= 3) {
        run_recv(atoi(argv[2]), argc > 3 ? atof(argv[3]) : 10.0);
    } else if (!strcmp(argv[1], "loopback")) {
        double secs = argc > 2 ? atof(argv[2]) : 6.0;
        int port = 47900;
        pthread_t th; struct sender_args a = {port, secs};
        pthread_create(&th, NULL, sender_thread, &a);
        run_recv(port, secs);
        pthread_join(th, NULL);
    } else {
        fprintf(stderr, "usage: rtp-demo send <host> <port> [secs] | recv <port> [secs] | loopback [secs]\n");
        return 2;
    }
    return 0;
}
