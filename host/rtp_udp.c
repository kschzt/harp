/* See rtp_udp.h. POSIX UDP transport for the RTP audio plane. */
#include "rtp_udp.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RX_BUF 2048

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
    unsigned long c_ok, c_lost, c_bad;
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
        ssize_t n = recv(rx->fd, buf, sizeof buf, 0);
        if (n < 0) return -1;
        unsigned long long arr = now_ns();        /* arrival stamp = recovery clock */

        harp_rtp_hdr h; const uint8_t *pl; size_t pln;
        if (harp_rtp_unpack(buf, (size_t)n, &h, &pl, &pln) != 0 ||
            pln % (rx->ch * sizeof(float)) != 0) { rx->c_bad++; continue; }

        if (rx->have_seq) {                        /* seq-gap loss accounting      */
            uint16_t gap = (uint16_t)(h.seq - rx->last_seq - 1);
            if (gap && gap < 0x8000) rx->c_lost += gap;
        }
        rx->last_seq = h.seq; rx->have_seq = 1;

        rx->ts64 = rx->have_ts ? harp_rtp_unwrap_ts(h.timestamp, rx->ts64) : h.timestamp;
        rx->have_ts = 1;
        harp_freerun_observe(rx->fr, rx->ts64, arr);
        harp_freerun_push(rx->fr, (const float *)pl, (unsigned)(pln / (rx->ch * sizeof(float))));
        rx->c_ok++;
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
    if (ok) *ok = rx->c_ok;
    if (lost) *lost = rx->c_lost;
    if (bad) *bad = rx->c_bad;
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
