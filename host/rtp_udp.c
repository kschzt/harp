/* See rtp_udp.h. POSIX UDP transport for the RTP audio plane. */
#include "rtp_udp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h> /* struct timeval for SCM_TIMESTAMP */
#include <time.h>
#include <unistd.h>

/* Big enough for the largest §8.7 datagram without truncation: the stereo main
 * mix at AUDIO_MAX_NSAMPLES (1024) is 12 + 1024*2*4 = 8204 bytes. A short recv
 * buffer truncates the UDP datagram, and the leftover payload then fails the
 * channel-multiple check — every packet reads as malformed. 16 KB leaves 2x
 * headroom (a transient stack buffer, negligible). */
#define RX_BUF 16384

static unsigned long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}

/* ---------------- receiver ---------------- */

struct harp_rtp_rx {
    int           fd;
    unsigned      ch;
    harp_freerun *fr;
    uint64_t      ts64;          /* unwrapped device timestamp        */
    int           have_ts, have_seq;
    uint16_t      last_seq;
    /* counters + liveness: written on the rx (poll) thread, read by any observer
     * (UI / the runtime's reconnect logic) -> atomic. last_arr_ns lets a caller
     * detect a dead UDP stream (UDP has no EOF) via harp_rtp_rx_silent_ms(). */
    _Atomic unsigned long      c_ok, c_lost, c_bad;
    _Atomic unsigned long long last_arr_ns;
};

harp_rtp_rx *harp_rtp_rx_open(int port, const harp_freerun_cfg *cfg) {
    harp_rtp_rx *rx = calloc(1, sizeof *rx);
    if (!rx) return NULL;
    rx->ch = cfg->channels;
    rx->fr = harp_freerun_new(cfg);
    rx->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!rx->fr || rx->fd < 0) { harp_rtp_rx_close(rx); return NULL; }
    int one = 1;
    setsockopt(rx->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* §7.3: ask the kernel to stamp each datagram at reception (SCM_TIMESTAMP).
     * The arrival time drives clock recovery; a userspace stamp taken after
     * recv() returns folds in OS scheduling latency (ms-scale on a non-RT host
     * under load/Wi-Fi), which is the jitter wall between ~40 dB and the
     * recovery's true SINAD. Best-effort — poll() falls back to a userspace
     * monotonic stamp if the OS delivers no control message. */
    setsockopt(rx->fd, SOL_SOCKET, SO_TIMESTAMP, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)port);
    if (bind(rx->fd, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "rtp_rx: bind port %d failed\n", port);
        harp_rtp_rx_close(rx);
        return NULL;
    }
    return rx;
}

void harp_rtp_rx_close(harp_rtp_rx *rx) {
    if (!rx) return;
    if (rx->fd >= 0) close(rx->fd);
    harp_freerun_free(rx->fr);
    free(rx);
}

int harp_rtp_rx_poll(harp_rtp_rx *rx, int timeout_ms) {
    int processed = 0;
    for (;;) {
        struct pollfd pfd = {rx->fd, POLLIN, 0};
        int pr = poll(&pfd, 1, processed == 0 ? timeout_ms : 0);
        if (pr < 0) return -1;
        if (pr == 0) break;                      /* nothing (more) available     */
        uint8_t buf[RX_BUF];
        union { /* control buffer for the SCM_TIMESTAMP cmsg */
            struct cmsghdr align;
            char space[CMSG_SPACE(sizeof(struct timeval))];
        } cbuf;
        struct iovec iov = {buf, sizeof buf};
        struct msghdr mh = {0};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf.space;
        mh.msg_controllen = sizeof cbuf.space;
        ssize_t n = recvmsg(rx->fd, &mh, 0);
        if (n < 0) return -1;
        unsigned long long mono = now_ns();
        atomic_store_explicit(&rx->last_arr_ns, mono, memory_order_relaxed); /* liveness (monotonic) */
        /* kernel RX timestamp drives recovery (sub-scheduling-jitter); fall back
         * to the userspace monotonic stamp if the OS delivered no SCM_TIMESTAMP. */
        unsigned long long arr = 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_TIMESTAMP) {
                struct timeval tv;
                memcpy(&tv, CMSG_DATA(cm), sizeof tv);
                arr = (unsigned long long)tv.tv_sec * 1000000000ull +
                      (unsigned long long)tv.tv_usec * 1000ull;
                break;
            }
        }
        if (!arr) arr = mono;

        harp_rtp_hdr h; const uint8_t *pl; size_t pln;
        if (harp_rtp_unpack(buf, (size_t)n, &h, &pl, &pln) != 0 ||
            pln % (rx->ch * sizeof(float)) != 0) {
            atomic_fetch_add_explicit(&rx->c_bad, 1, memory_order_relaxed); continue;
        }

        if (rx->have_seq) {                        /* seq-gap loss accounting      */
            uint16_t gap = (uint16_t)(h.seq - rx->last_seq - 1);
            if (gap && gap < 0x8000)
                atomic_fetch_add_explicit(&rx->c_lost, gap, memory_order_relaxed);
        }
        rx->last_seq = h.seq; rx->have_seq = 1;

        rx->ts64 = rx->have_ts ? harp_rtp_unwrap_ts(h.timestamp, rx->ts64) : h.timestamp;
        rx->have_ts = 1;
        harp_freerun_observe(rx->fr, rx->ts64, arr);
        harp_freerun_push(rx->fr, (const float *)pl, (unsigned)(pln / (rx->ch * sizeof(float))));
        atomic_fetch_add_explicit(&rx->c_ok, 1, memory_order_relaxed);
        processed++;
    }
    return processed;
}

unsigned harp_rtp_rx_pull(harp_rtp_rx *rx, float *out, unsigned n) {
    return harp_freerun_pull(rx->fr, out, n);
}

void harp_rtp_rx_stats(const harp_rtp_rx *rx, harp_freerun_stats *st) {
    harp_freerun_get_stats(rx->fr, st);
}

void harp_rtp_rx_counters(const harp_rtp_rx *rx, unsigned long *ok,
                          unsigned long *lost, unsigned long *bad) {
    if (ok) *ok = atomic_load_explicit(&rx->c_ok, memory_order_relaxed);
    if (lost) *lost = atomic_load_explicit(&rx->c_lost, memory_order_relaxed);
    if (bad) *bad = atomic_load_explicit(&rx->c_bad, memory_order_relaxed);
}

/* Milliseconds since the last packet arrived (any packet, even malformed — it
 * proves the peer is alive). UINT_MAX before the first packet. The runtime polls
 * this to declare an Ethernet session dead and reconnect, since UDP has no EOF. */
unsigned harp_rtp_rx_silent_ms(const harp_rtp_rx *rx) {
    unsigned long long last = atomic_load_explicit(&rx->last_arr_ns, memory_order_relaxed);
    if (last == 0) return 0xFFFFFFFFu;
    unsigned long long now = now_ns();
    return now > last ? (unsigned)((now - last) / 1000000ull) : 0;
}

/* ---------------- sender ---------------- */

struct harp_rtp_tx {
    int      fd;
    unsigned ch;
    uint32_t ssrc;
    uint16_t seq;
};

harp_rtp_tx *harp_rtp_tx_open(const char *host, int port, unsigned channels, uint32_t ssrc) {
    harp_rtp_tx *tx = calloc(1, sizeof *tx);
    if (!tx) return NULL;
    tx->ch = channels;
    tx->ssrc = ssrc;
    tx->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx->fd < 0) { free(tx); return NULL; }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {        /* try a hostname    */
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            fprintf(stderr, "rtp_tx: cannot resolve %s\n", host);
            close(tx->fd); free(tx); return NULL;
        }
        a.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (connect(tx->fd, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "rtp_tx: connect failed\n");
        close(tx->fd); free(tx); return NULL;
    }
    return tx;
}

void harp_rtp_tx_close(harp_rtp_tx *tx) {
    if (!tx) return;
    if (tx->fd >= 0) close(tx->fd);
    free(tx);
}

int harp_rtp_tx_send(harp_rtp_tx *tx, uint32_t ts, const float *frames, unsigned nframes) {
    uint8_t buf[RX_BUF];
    size_t plen = (size_t)nframes * tx->ch * sizeof(float);
    harp_rtp_hdr h = {96, 0, tx->seq++, ts, tx->ssrc};
    int n = harp_rtp_pack(buf, sizeof buf, &h, frames, plen);
    if (n < 0) return -1;
    return send(tx->fd, buf, (size_t)n, 0) == n ? 0 : -1;
}
