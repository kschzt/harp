/* device/log_ring.c — the §4.2 stream `log` device log ring (§14.4).
 * See device/log_ring.h for the contract. A fixed 128 KiB circular byte buffer of
 * length-prefixed variable records; oldest-dropped on overflow; one internal mutex.
 * The buffer is never serialized to the wire (it is drained to CBOR in-process),
 * so the record header packs host-native scalars — no endianness concern. */

#include "log_ring.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "harp/cbor.h"

/* Per-record bounds — a runaway log line is truncated, never rejected. */
#define LOGREC_TAG_MAX 31u
#define LOGREC_MSG_MAX 480u

/* On-ring record header, 16 bytes, fixed layout (no struct padding):
 *   [0..4)   total  u32  whole-record byte count (HDR + tag + msg) — the skip stride
 *   [4..12)  msc    u64  §14.4 log-record key 0
 *   [12..14) msglen u16
 *   [14]     level  u8   §14.4 log-record key 1
 *   [15]     taglen u8 */
#define HDR_BYTES 16u

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_buf[HARP_LOGRING_CAP];
static size_t g_head = 0;      /* write position (mod CAP) */
static size_t g_tail = 0;      /* oldest record start (mod CAP) */
static size_t g_used = 0;      /* bytes currently stored */
static size_t g_nrec = 0;      /* records currently stored */
static uint64_t g_dropped = 0; /* records evicted (overflow) since reset */

/* Modular copy into/out of the circular buffer (a record may straddle the wrap). */
static void ring_put(size_t pos, const void *p, size_t n) {
    size_t first = HARP_LOGRING_CAP - pos;
    if (n <= first) {
        memcpy(g_buf + pos, p, n);
    } else {
        memcpy(g_buf + pos, p, first);
        memcpy(g_buf, (const uint8_t *)p + first, n - first);
    }
}
static void ring_get(size_t pos, void *p, size_t n) {
    size_t first = HARP_LOGRING_CAP - pos;
    if (n <= first) {
        memcpy(p, g_buf + pos, n);
    } else {
        memcpy(p, g_buf + pos, first);
        memcpy((uint8_t *)p + first, g_buf, n - first);
    }
}

/* Store one record. Caller holds no lock; this takes g_mu. */
static void ring_append_locked(int level, const char *tag, uint64_t msc, const char *msg,
                               size_t msglen) {
    size_t taglen = tag ? strlen(tag) : 0;
    if (taglen > LOGREC_TAG_MAX) taglen = LOGREC_TAG_MAX;
    if (msglen > LOGREC_MSG_MAX) msglen = LOGREC_MSG_MAX;
    size_t total = HDR_BYTES + taglen + msglen;
    if (total > HARP_LOGRING_CAP) return; /* unreachable given the bounds above */

    pthread_mutex_lock(&g_mu);
    /* Evict oldest whole records until the new one fits (drop-oldest). */
    while (g_used + total > HARP_LOGRING_CAP && g_nrec > 0) {
        uint8_t th[4];
        ring_get(g_tail, th, 4);
        uint32_t rl;
        memcpy(&rl, th, 4);
        g_tail = (g_tail + rl) % HARP_LOGRING_CAP;
        g_used -= rl;
        g_nrec--;
        g_dropped++;
    }
    uint8_t h[HDR_BYTES];
    uint32_t total32 = (uint32_t)total;
    uint16_t msg16 = (uint16_t)msglen;
    uint8_t lvl8 = (uint8_t)level, tag8 = (uint8_t)taglen;
    memcpy(h, &total32, 4);
    memcpy(h + 4, &msc, 8);
    memcpy(h + 12, &msg16, 2);
    h[14] = lvl8;
    h[15] = tag8;
    ring_put(g_head, h, HDR_BYTES);
    ring_put((g_head + HDR_BYTES) % HARP_LOGRING_CAP, tag, taglen);
    ring_put((g_head + HDR_BYTES + taglen) % HARP_LOGRING_CAP, msg, msglen);
    g_head = (g_head + total) % HARP_LOGRING_CAP;
    g_used += total;
    g_nrec++;
    pthread_mutex_unlock(&g_mu);
}

void harp_devlog(int level, const char *tag, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    /* stderr / journal: byte-identical to the prior fprintf(stderr, fmt, ...) for
     * any line shorter than the buffer (every device log line is). */
    fputs(buf, stderr);
    /* Ring: store the formatted line minus its trailing newline(s). */
    size_t len = (n < 0) ? 0 : ((size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    ring_append_locked(level, tag, 0, buf, len);
}

void harp_devlog_put(int level, const char *tag, uint64_t msc, const char *msg) {
    ring_append_locked(level, tag, msc, msg, msg ? strlen(msg) : 0);
}

void harp_devlog_emit_cbor(harp_cbuf *m, size_t max_payload) {
    pthread_mutex_lock(&g_mu);
    /* Bound the window to the most-recent records whose stored bytes fit max_payload:
     * walk a read cursor forward from the tail, skipping oldest records until the
     * remaining span is within budget. */
    size_t cur = g_tail, remain = g_used, nrec = g_nrec;
    while (remain > max_payload && nrec > 0) {
        uint8_t th[4];
        ring_get(cur, th, 4);
        uint32_t rl;
        memcpy(&rl, th, 4);
        cur = (cur + rl) % HARP_LOGRING_CAP;
        remain -= rl;
        nrec--;
    }
    harp_cbor_array(m, (uint64_t)nrec);
    for (size_t i = 0; i < nrec; i++) {
        uint8_t h[HDR_BYTES];
        ring_get(cur, h, HDR_BYTES);
        uint32_t total;
        uint64_t msc;
        uint16_t msglen;
        memcpy(&total, h, 4);
        memcpy(&msc, h + 4, 8);
        memcpy(&msglen, h + 12, 2);
        uint8_t level = h[14], taglen = h[15];
        char tag[LOGREC_TAG_MAX + 1], msg[LOGREC_MSG_MAX + 1];
        ring_get((cur + HDR_BYTES) % HARP_LOGRING_CAP, tag, taglen);
        ring_get((cur + HDR_BYTES + taglen) % HARP_LOGRING_CAP, msg, msglen);
        harp_cbor_map(m, 4); /* §14.4 log-record { 0 msc, 1 level, 2 tag, 3 msg } */
        harp_cbor_uint(m, 0);
        harp_cbor_uint(m, msc);
        harp_cbor_uint(m, 1);
        harp_cbor_uint(m, level);
        harp_cbor_uint(m, 2);
        harp_cbor_textn(m, tag, taglen);
        harp_cbor_uint(m, 3);
        harp_cbor_textn(m, msg, msglen);
        cur = (cur + total) % HARP_LOGRING_CAP;
    }
    pthread_mutex_unlock(&g_mu);
}

size_t harp_devlog_count(void) {
    pthread_mutex_lock(&g_mu);
    size_t n = g_nrec;
    pthread_mutex_unlock(&g_mu);
    return n;
}
size_t harp_devlog_used(void) {
    pthread_mutex_lock(&g_mu);
    size_t n = g_used;
    pthread_mutex_unlock(&g_mu);
    return n;
}
size_t harp_devlog_capacity(void) { return HARP_LOGRING_CAP; }
uint64_t harp_devlog_dropped(void) {
    pthread_mutex_lock(&g_mu);
    uint64_t n = g_dropped;
    pthread_mutex_unlock(&g_mu);
    return n;
}
void harp_devlog_reset(void) {
    pthread_mutex_lock(&g_mu);
    g_head = g_tail = g_used = g_nrec = 0;
    g_dropped = 0;
    pthread_mutex_unlock(&g_mu);
}
