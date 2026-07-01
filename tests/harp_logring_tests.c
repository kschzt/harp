/* Unit tests for the §4.2 stream `log` device log ring (device/log_ring.c, §14.4).
 *
 * Covers the RME contract: (a) log lines routed through the ring land as
 * well-formed §14.4 log-records; (b) the ring is bounded at >= 64 KiB and drops
 * OLDEST on overflow (write past capacity => oldest gone, newest kept); plus the
 * §4.2.1 per-bundle ctl budget (the emit window caps its bytes). The §16 msg
 * redaction of the SAME records is proven host-side (harp-tests test_anonymize_*
 * over harp_anonymize_device_section) and end-to-end in diag-bundle-eth-test.sh. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harp/cbor.h"
#include "log_ring.h"

#include "check.h"

/* Portable substring search over raw bytes (memmem is not on every libc/MinGW). */
static int mem_contains(const uint8_t *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return nlen == 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return 1;
    return 0;
}

/* Decode one §14.4 log-record map { 0 msc, 1 level, 2 tag, 3 msg } and check it. */
static void check_record(harp_cdec *d, uint64_t want_msc, uint64_t want_level,
                         const char *want_tag, const char *want_msg) {
    uint64_t n, k, u;
    const char *s;
    size_t sl;
    CHECK(harp_cdec_map(d, &n) && n == 4);
    CHECK(harp_cdec_uint(d, &k) && k == 0 && harp_cdec_uint(d, &u) && u == want_msc);
    CHECK(harp_cdec_uint(d, &k) && k == 1 && harp_cdec_uint(d, &u) && u == want_level);
    CHECK(harp_cdec_uint(d, &k) && k == 2 && harp_cdec_text(d, &s, &sl) &&
          sl == strlen(want_tag) && memcmp(s, want_tag, sl) == 0);
    CHECK(harp_cdec_uint(d, &k) && k == 3 && harp_cdec_text(d, &s, &sl) &&
          sl == strlen(want_msg) && memcmp(s, want_msg, sl) == 0);
}

/* (a) log lines routed through the ring land as well-formed records, in order,
 * with msc/level/tag preserved and the msg's trailing newline stripped. */
static void test_ring_capture(void) {
    harp_devlog_reset();
    CHECK(harp_devlog_count() == 0);

    harp_devlog_put(HARP_LOG_INFO, "session", 0, "alpha-token");
    harp_devlog_put(HARP_LOG_WARN, "audio", 5, "beta-token");
    /* The printf/stderr path (harp_devlog) must ALSO land in the ring, with the
     * formatted line stored minus its trailing newline. */
    harp_devlog(HARP_LOG_ERROR, "daemon", "harp-deviced: gamma-token %d\n", 42);

    CHECK(harp_devlog_count() == 3);

    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_devlog_emit_cbor(&m, (size_t)-1); /* whole ring */

    harp_cdec d;
    uint64_t narr;
    harp_cdec_init(&d, m.buf, m.len);
    CHECK(harp_cdec_array(&d, &narr) && narr == 3);
    check_record(&d, 0, HARP_LOG_INFO, "session", "alpha-token");
    check_record(&d, 5, HARP_LOG_WARN, "audio", "beta-token");
    check_record(&d, 0, HARP_LOG_ERROR, "daemon", "harp-deviced: gamma-token 42");
    CHECK(!d.err && d.p == d.end);
    /* Newline was stripped, not stored. */
    CHECK(!mem_contains(m.buf, m.len, "gamma-token 42\n"));
    harp_cbuf_free(&m);
}

/* (b) the ring bounds at >= 64 KiB and drops OLDEST on overflow; and the emit
 * window honours the §4.2.1 per-bundle ctl budget. */
static void test_ring_bound_and_evict(void) {
    /* The ring must retain at least the >= 64 KiB the spec RECOMMENDS. */
    CHECK(harp_devlog_capacity() >= 65536u);

    harp_devlog_reset();
    /* ~220 bytes/record; 3000 records ≈ 660 KiB, far past the 128 KiB ring. */
    char msg[256];
    for (int i = 0; i < 3000; i++) {
        snprintf(msg, sizeof msg,
                 "MARK-%06d filler-payload-to-make-each-record-about-two-hundred-bytes-"
                 "so-the-ring-overflows-and-drops-the-oldest-records-first-xxxxxxxxxx",
                 i);
        harp_devlog_put(HARP_LOG_INFO, "fill", (uint64_t)i, msg);
    }

    /* Bounded: never exceeds capacity. Deep: still holds >= 64 KiB of tail. */
    CHECK(harp_devlog_used() <= harp_devlog_capacity());
    CHECK(harp_devlog_used() >= 65536u);
    /* Overflow actually evicted, and the record count is bounded (not all 3000). */
    CHECK(harp_devlog_dropped() > 0);
    CHECK(harp_devlog_count() > 0 && harp_devlog_count() < 3000);

    harp_cbuf full;
    harp_cbuf_init(&full);
    harp_devlog_emit_cbor(&full, (size_t)-1); /* whole ring */
    uint64_t narr;
    harp_cdec d;
    harp_cdec_init(&d, full.buf, full.len);
    CHECK(harp_cdec_array(&d, &narr) && narr == harp_devlog_count());
    /* Oldest GONE, newest KEPT. */
    CHECK(!mem_contains(full.buf, full.len, "MARK-000000"));
    CHECK(mem_contains(full.buf, full.len, "MARK-002999"));

    /* §4.2.1 ctl budget: the bundle window is smaller than the whole ring and its
     * payload stays within the 32 KiB emit budget (records ~= a handful more). */
    harp_cbuf win;
    harp_cbuf_init(&win);
    harp_devlog_emit_cbor(&win, HARP_LOGRING_BUNDLE_MAX);
    CHECK(win.len > 0 && win.len < HARP_LOGRING_BUNDLE_MAX + 2048u);
    CHECK(win.len < full.len); /* the window is a proper subset of the ring */
    CHECK(mem_contains(win.buf, win.len, "MARK-002999")); /* newest always in the window */

    harp_cbuf_free(&full);
    harp_cbuf_free(&win);
}

int main(void) {
    test_ring_capture();
    test_ring_bound_and_evict();
    return check_report("harp-logring-tests");
}
