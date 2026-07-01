#ifndef HARP_DEVICE_LOG_RING_H
#define HARP_DEVICE_LOG_RING_H

/* device/log_ring.h — the §4.2 stream `log` device log ring (§14.4).
 *
 * A bounded, fixed-size (>= 64 KiB) in-memory circular buffer that captures the
 * device daemon's own diagnostic log lines. The device already writes these to
 * stderr/journal via fprintf; harp_devlog() is the drop-in that ALSO retains
 * them in the ring so `req diag.bundle` can surface "the file that ends the
 * guessing" (§14.4). The ring is:
 *   - fixed-capacity (HARP_LOGRING_CAP, a static buffer — NO allocation on the
 *     log path; a record memcpy under a short mutex is the only cost);
 *   - oldest-dropped on overflow (a flood overwrites stale records, keeps newest);
 *   - thread-safe (a single internal mutex; safe from every daemon thread — the
 *     accept/session/audio/panel/ffs threads all log through it. NOT called from
 *     the per-sample render inner loop, so the mutex is never on the hot path).
 *
 * Records mirror the §14.4 stream-`log` shape { 0 => msc, 1 => level, 2 => tag,
 * 3 => msg }; harp_devlog_emit_cbor() drains a recent window as the diag-bundle
 * device-section key 2 (docs/diag-bundle-design.md). §16 anonymization of the
 * msg free-text is HOST-side (harp_anonymize_device_section clears log-record
 * key 3), consistent with the rest of the device-section §16 seam. */

#include <stddef.h>
#include <stdint.h>

#include "harp/cbor.h" /* harp_cbuf */

#ifdef __cplusplus
extern "C" {
#endif

/* §14.4 ring capacity: 128 KiB — comfortably above the >= 64 KiB the spec
 * RECOMMENDS, so a long-lived session keeps a deep tail of history. */
#define HARP_LOGRING_CAP (128u * 1024u)

/* Per-bundle drain budget: the diag.bundle device-section rides the `ctl` stream,
 * and §4.2.1 caps a single ctl message at 64 KiB. Emit at most this many bytes of
 * (the most recent) log payload so identity + counters + logs stay well under the
 * ctl bound; the RING itself still retains the full >= 64 KiB tail. */
#define HARP_LOGRING_BUNDLE_MAX (32u * 1024u)

/* §14.4 log-level (FROZEN — same numbering as shell/diag_rings.h /
 * docs/diag-bundle-design.md log-level: debug 0, info 1, warn 2, error 3). */
enum {
    HARP_LOG_DEBUG = 0,
    HARP_LOG_INFO = 1,
    HARP_LOG_WARN = 2,
    HARP_LOG_ERROR = 3
};

/* Route a device diagnostic line to BOTH stderr (byte-identical to the prior
 * fprintf(stderr, fmt, ...)) AND the §4.2 log ring. printf-style; the message is
 * formatted into a fixed stack buffer (no heap). `tag` is a short, stable,
 * machine-greppable label (NEVER anonymized); the formatted line is the msg
 * (a trailing newline is stripped before storage). msc is recorded 0
 * (pre-stream / not audio-clocked) — the lifecycle logs are not sample-clocked. */
#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void harp_devlog(int level, const char *tag, const char *fmt, ...);

/* Emit the recent ring records as a CBOR `[* log-record]` array (device-section
 * key 2), oldest-first, bounded to <= max_payload bytes of stored record data
 * (the bundle path passes HARP_LOGRING_BUNDLE_MAX; a test passes SIZE_MAX to see
 * the whole ring). Each record is a map { 0 => msc, 1 => level, 2 => tag,
 * 3 => msg }. Thread-safe (snapshots under the ring mutex). */
void harp_devlog_emit_cbor(harp_cbuf *m, size_t max_payload);

/* Test / introspection support. */
size_t harp_devlog_count(void);    /* records currently stored */
size_t harp_devlog_used(void);     /* bytes currently stored */
size_t harp_devlog_capacity(void); /* HARP_LOGRING_CAP */
uint64_t harp_devlog_dropped(void); /* records evicted since reset (overflow) */
void harp_devlog_reset(void);      /* clear the ring (tests) */
/* Append a record WITHOUT writing stderr (tests) — same store path as harp_devlog. */
void harp_devlog_put(int level, const char *tag, uint64_t msc, const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* HARP_DEVICE_LOG_RING_H */
