/* Unit tests for the HARP core library. */
#include <stdio.h>
#include <string.h>

#include "harp/audio.h"
#include "harp/cbor.h"
#include "harp/envelope.h"
#include "harp/frame.h"
#include "harp/object.h"
#include "harp/sha256.h"
#include "harp/store.h"

static int g_fail = 0, g_pass = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

static void hexcheck(const harp_cbuf *b, const char *hex) {
    char got[1024];
    static const char hexd[] = "0123456789abcdef";
    size_t n = b->len < 511 ? b->len : 511;
    for (size_t i = 0; i < n; i++) {
        got[2 * i] = hexd[b->buf[i] >> 4];
        got[2 * i + 1] = hexd[b->buf[i] & 0xf];
    }
    got[2 * n] = 0;
    if (strcmp(got, hex) != 0) {
        g_fail++;
        fprintf(stderr, "FAIL encoding: got %s want %s\n", got, hex);
    } else
        g_pass++;
}

static void test_sha256(void) {
    uint8_t d[32];
    char hex[65];
    static const char hexd[] = "0123456789abcdef";
    harp_sha256_digest("abc", 3, d);
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = hexd[d[i] >> 4];
        hex[2 * i + 1] = hexd[d[i] & 0xf];
    }
    hex[64] = 0;
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    harp_sha256_digest("", 0, d);
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = hexd[d[i] >> 4];
        hex[2 * i + 1] = hexd[d[i] & 0xf];
    }
    hex[64] = 0;
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
}

static void test_cbor_encode(void) {
    harp_cbuf b;
    harp_cbuf_init(&b);

    /* shortest-form integers (RFC 8949 §4.2.1) */
    harp_cbor_uint(&b, 0);
    hexcheck(&b, "00");
    harp_cbuf_reset(&b);
    harp_cbor_uint(&b, 23);
    hexcheck(&b, "17");
    harp_cbuf_reset(&b);
    harp_cbor_uint(&b, 24);
    hexcheck(&b, "1818");
    harp_cbuf_reset(&b);
    harp_cbor_uint(&b, 255);
    hexcheck(&b, "18ff");
    harp_cbuf_reset(&b);
    harp_cbor_uint(&b, 256);
    hexcheck(&b, "190100");
    harp_cbuf_reset(&b);
    harp_cbor_uint(&b, 65536);
    hexcheck(&b, "1a00010000");
    harp_cbuf_reset(&b);
    harp_cbor_uint(&b, 4294967296ull);
    hexcheck(&b, "1b0000000100000000");
    harp_cbuf_reset(&b);
    harp_cbor_int(&b, -1);
    hexcheck(&b, "20");
    harp_cbuf_reset(&b);
    harp_cbor_int(&b, -100);
    hexcheck(&b, "3863");

    /* preferred float serialization */
    harp_cbuf_reset(&b);
    harp_cbor_float(&b, 0.5);
    hexcheck(&b, "f93800");
    harp_cbuf_reset(&b);
    harp_cbor_float(&b, 1.0);
    hexcheck(&b, "f93c00");
    harp_cbuf_reset(&b);
    harp_cbor_float(&b, 100000.0);
    hexcheck(&b, "fa47c35000");
    harp_cbuf_reset(&b);
    harp_cbor_float(&b, 0.1);
    hexcheck(&b, "fb3fb999999999999a");
    harp_cbuf_reset(&b);
    harp_cbor_float(&b, 5.960464477539063e-8); /* smallest f16 subnormal */
    hexcheck(&b, "f90001");

    /* text, bytes, containers */
    harp_cbuf_reset(&b);
    harp_cbor_text(&b, "IETF");
    hexcheck(&b, "6449455446");
    harp_cbuf_reset(&b);
    harp_cbor_array(&b, 2);
    harp_cbor_uint(&b, 1);
    harp_cbor_uint(&b, 2);
    hexcheck(&b, "820102");
    harp_cbuf_reset(&b);
    harp_cbor_map(&b, 1);
    harp_cbor_uint(&b, 0);
    harp_cbor_bool(&b, true);
    hexcheck(&b, "a100f5");

    harp_cbuf_free(&b);
}

static void test_cbor_roundtrip(void) {
    harp_cbuf b;
    harp_cbuf_init(&b);
    harp_cbor_map(&b, 3);
    harp_cbor_uint(&b, 0);
    harp_cbor_int(&b, -42);
    harp_cbor_uint(&b, 1);
    harp_cbor_text(&b, "hello");
    harp_cbor_uint(&b, 2);
    harp_cbor_array(&b, 3);
    harp_cbor_float(&b, 0.83);
    harp_cbor_bool(&b, false);
    harp_cbor_null(&b);

    harp_cdec d;
    harp_cdec_init(&d, b.buf, b.len);
    uint64_t n, key;
    int64_t iv;
    CHECK(harp_cdec_map(&d, &n) && n == 3);
    CHECK(harp_cdec_uint(&d, &key) && key == 0);
    CHECK(harp_cdec_int(&d, &iv) && iv == -42);
    CHECK(harp_cdec_uint(&d, &key) && key == 1);
    const char *s;
    size_t sl;
    CHECK(harp_cdec_text(&d, &s, &sl) && sl == 5 && memcmp(s, "hello", 5) == 0);
    CHECK(harp_cdec_uint(&d, &key) && key == 2);
    CHECK(harp_cdec_array(&d, &n) && n == 3);
    double f;
    CHECK(harp_cdec_float(&d, &f) && f > 0.829 && f < 0.831);
    bool bv;
    CHECK(harp_cdec_bool(&d, &bv) && !bv);
    CHECK(harp_cdec_null(&d));
    CHECK(!d.err && d.p == d.end);

    /* skip/span */
    harp_cdec_init(&d, b.buf, b.len);
    const uint8_t *span;
    size_t span_len;
    CHECK(harp_cdec_span(&d, &span, &span_len) && span_len == b.len);

    /* truncated input must error, not crash */
    harp_cdec_init(&d, b.buf, b.len - 3);
    CHECK(!harp_cdec_skip(&d) || d.err);

    /* hostile container count must be rejected */
    uint8_t evil[] = {0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    harp_cdec_init(&d, evil, sizeof evil);
    CHECK(!harp_cdec_array(&d, &n));

    harp_cbuf_free(&b);
}

static void test_frame(void) {
    harp_frame_hdr h = {HARP_FRAME_FVER, HARP_STREAM_OBJ, HARP_FLAG_FIN, 65536};
    uint8_t enc[HARP_FRAME_HDR_LEN];
    harp_frame_hdr_encode(&h, enc);
    harp_frame_hdr out;
    CHECK(harp_frame_hdr_decode(enc, &out));
    CHECK(out.fver == h.fver && out.stream == h.stream && out.flags == h.flags &&
          out.length == h.length);

    enc[0] = 0x02; /* wrong fver: fatal */
    CHECK(!harp_frame_hdr_decode(enc, &out));
    enc[0] = 0x01;
    enc[3] = 0x80; /* reserved flag set: fatal */
    CHECK(!harp_frame_hdr_decode(enc, &out));
    enc[3] = 0;
    enc[7] = 0xff; /* oversize length: fatal */
    CHECK(!harp_frame_hdr_decode(enc, &out));
}

static void test_objects(void) {
    /* identical logical content -> identical hash; entry order irrelevant */
    harp_cbuf b1, b2;
    harp_cbuf_init(&b1);
    harp_cbuf_init(&b2);
    harp_obj_encode_blob(&b1, "application/x.test", "payload", 7);
    harp_obj_encode_blob(&b2, "application/x.test", "payload", 7);
    harp_hash h1 = harp_hash_compute(b1.buf, b1.len);
    harp_hash h2 = harp_hash_compute(b2.buf, b2.len);
    CHECK(harp_hash_eq(&h1, &h2));

    harp_obj_encode_blob(&b2, "application/x.test", "payloae", 7);
    harp_hash h3 = harp_hash_compute(b2.buf + b1.len, b2.len - b1.len);
    CHECK(!harp_hash_eq(&h1, &h3));

    /* tree key sorting: same entries, different given order, same encoding */
    harp_tree_entry e1[3] = {{"zz", h1, 0}, {"a", h2, 0}, {"ab", h3, 0}};
    harp_tree_entry e2[3] = {{"a", h2, 0}, {"ab", h3, 0}, {"zz", h1, 0}};
    harp_cbuf t1, t2;
    harp_cbuf_init(&t1);
    harp_cbuf_init(&t2);
    harp_obj_encode_tree(&t1, e1, 3);
    harp_obj_encode_tree(&t2, e2, 3);
    CHECK(t1.len == t2.len && memcmp(t1.buf, t2.buf, t1.len) == 0);
    CHECK(harp_obj_kind(t1.buf, t1.len) == HARP_OBJ_TREE);

    /* blob parse roundtrip */
    const char *media;
    size_t media_len;
    const uint8_t *payload;
    size_t payload_len;
    CHECK(harp_obj_parse_blob(b1.buf, b1.len, &media, &media_len, &payload, &payload_len));
    CHECK(payload_len == 7 && memcmp(payload, "payload", 7) == 0);

    /* snapshot root extraction */
    harp_hash tree_h = harp_hash_compute(t1.buf, t1.len);
    harp_cbuf snap;
    harp_cbuf_init(&snap);
    harp_obj_encode_snapshot(&snap, &tree_h, &h1, 1, 1760000000, "device", "1.0.0", "msg");
    harp_hash root;
    CHECK(harp_obj_parse_snapshot_root(snap.buf, snap.len, &root));
    CHECK(harp_hash_eq(&root, &tree_h));
    CHECK(harp_obj_kind(snap.buf, snap.len) == HARP_OBJ_SNAPSHOT);

    harp_cbuf_free(&b1);
    harp_cbuf_free(&b2);
    harp_cbuf_free(&t1);
    harp_cbuf_free(&t2);
    harp_cbuf_free(&snap);
}

static void test_store(void) {
    harp_store s;
    CHECK(harp_store_open(&s, "/tmp/harp-test-store") == 0);

    harp_cbuf b;
    harp_cbuf_init(&b);
    harp_obj_encode_blob(&b, "application/x.test", "store me", 8);
    harp_hash h;
    CHECK(harp_store_put(&s, b.buf, b.len, &h) == 0);
    CHECK(harp_store_have(&s, &h));

    harp_cbuf out;
    harp_cbuf_init(&out);
    CHECK(harp_store_get(&s, &h, &out) == 0);
    CHECK(out.len == b.len && memcmp(out.buf, b.buf, b.len) == 0);

    harp_hash absent = h;
    absent.b[5] ^= 0xff;
    CHECK(!harp_store_have(&s, &absent));

    /* refs: hierarchy, unborn read, write/read roundtrip */
    harp_ref r = {0};
    snprintf(r.name, sizeof r.name, "archive/2026-06-10T12:00:00Z");
    r.unborn = false;
    r.hash = h;
    r.generation = 7;
    r.dirty = true;
    CHECK(harp_store_ref_write(&s, &r) == 0);
    harp_ref rr;
    CHECK(harp_store_ref_read(&s, r.name, &rr) == 0);
    CHECK(!rr.unborn && harp_hash_eq(&rr.hash, &h) && rr.generation == 7 && rr.dirty);
    CHECK(harp_store_ref_read(&s, "live/project-not-here", &rr) == 0 && rr.unborn);

    /* hostile ref names rejected */
    CHECK(harp_store_ref_read(&s, "../../etc/passwd", &rr) != 0);
    CHECK(harp_store_ref_read(&s, "/abs", &rr) != 0);

    harp_cbuf_free(&b);
    harp_cbuf_free(&out);
}

/* ref_list must enumerate hierarchical refs and return their LOGICAL names —
 * including the colons in archive timestamps. On Windows those colons are
 * escaped on disk (illegal in NTFS names); this proves the escape/unescape
 * round-trips through the directory walk. On POSIX it's a plain walk check. */
struct namelist {
    char names[16][HARP_REF_NAME_MAX];
    int n;
};
static void collect_cb(const harp_ref *r, void *ud) {
    struct namelist *nl = ud;
    if (nl->n < 16) snprintf(nl->names[nl->n++], HARP_REF_NAME_MAX, "%s", r->name);
}
static bool has_name(const struct namelist *nl, const char *want) {
    for (int i = 0; i < nl->n; i++)
        if (strcmp(nl->names[i], want) == 0) return true;
    return false;
}

static void test_store_reflist(void) {
    harp_store s;
    CHECK(harp_store_open(&s, "/tmp/harp-test-reflist") == 0);

    harp_cbuf b;
    harp_cbuf_init(&b);
    harp_obj_encode_blob(&b, "application/x.test", "x", 1);
    harp_hash h;
    CHECK(harp_store_put(&s, b.buf, b.len, &h) == 0);

    static const char *names[3] = {
        "live/project",
        "archive/2026-06-10T12:00:00Z", /* colons: illegal raw on Windows */
        "archive/2026-06-11T08:30:15Z",
    };
    for (int i = 0; i < 3; i++) {
        harp_ref r = {0};
        snprintf(r.name, sizeof r.name, "%s", names[i]);
        r.unborn = false;
        r.hash = h;
        r.generation = (uint64_t)(i + 1);
        r.dirty = (i & 1) != 0;
        CHECK(harp_store_ref_write(&s, &r) == 0);
        /* and each reads back with its colons intact */
        harp_ref rr;
        CHECK(harp_store_ref_read(&s, names[i], &rr) == 0);
        CHECK(strcmp(rr.name, names[i]) == 0 && !rr.unborn && harp_hash_eq(&rr.hash, &h));
    }

    struct namelist nl = {0};
    CHECK(harp_store_ref_list(&s, collect_cb, &nl) == 0);
    CHECK(nl.n == 3);
    for (int i = 0; i < 3; i++) CHECK(has_name(&nl, names[i]));

    harp_cbuf_free(&b);
}

static void test_envelope(void) {
    harp_cbuf b;
    harp_cbuf_init(&b);
    harp_env_head(&b, HARP_MSG_REQUEST, 42, "state.refs", true);
    harp_cbor_map(&b, 1);
    harp_cbor_uint(&b, 0);
    harp_cbor_text(&b, "x");

    harp_env e;
    CHECK(harp_env_parse(b.buf, b.len, &e));
    CHECK(e.msgtype == HARP_MSG_REQUEST && e.rid == 42 &&
          strcmp(e.method, "state.refs") == 0 && e.has_body);

    /* body span is a complete CBOR item */
    harp_cdec d;
    harp_cdec_init(&d, e.body, e.body_len);
    CHECK(harp_cdec_skip(&d) && d.p == d.end);

    harp_cbuf_free(&b);
}

static void test_audio_codec(void) {
    harp_audio_hdr h = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D, 0, 7,
                        0x123456789abcdef0ull, 256, HARP_AUDIO_FMT_F32};
    uint8_t enc[HARP_AUDIO_HDR_LEN];
    harp_audio_hdr_encode(&h, enc);
    harp_audio_hdr o;
    CHECK(harp_audio_hdr_decode(enc, &o));
    CHECK(o.dirflags == HARP_AUDIO_DIR_H2D && o.slots == 0 && o.epoch == 7 &&
          o.ts == 0x123456789abcdef0ull && o.nsamples == 256 &&
          o.fmt == HARP_AUDIO_FMT_F32);
    CHECK(harp_audio_payload_len(&o) == 0); /* slots=0 pacing frame (§8.3) */
    o.slots = 2;
    CHECK(harp_audio_payload_len(&o) == 2048);

    enc[0] = 0x02; /* bad fver */
    CHECK(!harp_audio_hdr_decode(enc, &o));
    enc[0] = HARP_AUDIO_FVER;
    enc[18] = 0xff; /* unknown fmt */
    CHECK(!harp_audio_hdr_decode(enc, &o));

    harp_audio_hdr z = h;
    z.nsamples = 0;
    harp_audio_hdr_encode(&z, enc);
    CHECK(!harp_audio_hdr_decode(enc, &z));

    /* event fence flag (§8.3.1, spec 0.3.4): a fenced pacing frame
     * round-trips the flag; the 4-byte fence word travels after the
     * header and never perturbs payload accounting */
    harp_audio_hdr f = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D | HARP_AUDIO_FENCE,
                        0, 0, 12345, 256, HARP_AUDIO_FMT_F32};
    harp_audio_hdr_encode(&f, enc);
    harp_audio_hdr fo;
    CHECK(harp_audio_hdr_decode(enc, &fo));
    CHECK((fo.dirflags & HARP_AUDIO_FENCE) && (fo.dirflags & HARP_AUDIO_DIR_H2D));
    CHECK(harp_audio_payload_len(&fo) == 0); /* fence word is not payload */
}

/* Multitimbral channel key (§9.4 key 5) on param/ramp event bodies. The real
 * encoders (shell/runtime.cpp) and decoder (device/session.c) are C++/device
 * code, so this pins the WIRE CONTRACT they share via harpcore's codec: the
 * channel round-trips, the device's skip-unknown loop tolerates it, and —
 * the back-compat guarantee — channel 0 is byte-identical to the legacy form
 * (encoders omit the key for part 0). End-to-end host→device→part routing is
 * validated on hardware once the engine routes channels (P2). */
static void enc_param_body(harp_cbuf *b, uint64_t id, double val, unsigned channel) {
    harp_cbor_map(b, channel ? 3 : 2);
    harp_cbor_uint(b, 0);
    harp_cbor_uint(b, id);
    harp_cbor_uint(b, 1);
    harp_cbor_float(b, val);
    if (channel) {
        harp_cbor_uint(b, 5);
        harp_cbor_uint(b, channel);
    }
}
/* the device's param-body decode loop (keys 0,1,5; unknown skipped; chan⇒0) */
static unsigned dec_param_channel(const harp_cbuf *b, uint64_t *id_out) {
    harp_cdec d;
    harp_cdec_init(&d, b->buf, b->len);
    uint64_t n, key, id = 0, channel = 0;
    double v = 0;
    if (!harp_cdec_map(&d, &n)) return 0xffff;
    for (uint64_t i = 0; i < n; i++) {
        if (!harp_cdec_uint(&d, &key)) return 0xffff;
        if (key == 0) harp_cdec_uint(&d, &id);
        else if (key == 1) harp_cdec_float(&d, &v);
        else if (key == 5) harp_cdec_uint(&d, &channel);
        else harp_cdec_skip(&d);
    }
    if (id_out) *id_out = id;
    return d.err ? 0xffff : (unsigned)channel;
}

static void test_event_channel(void) {
    harp_cbuf legacy, ch0, ch15;
    harp_cbuf_init(&legacy);
    harp_cbuf_init(&ch0);
    harp_cbuf_init(&ch15);

    /* byte-identity: channel 0 must equal the legacy 2-key body exactly */
    harp_cbor_map(&legacy, 2);
    harp_cbor_uint(&legacy, 0);
    harp_cbor_uint(&legacy, 7);
    harp_cbor_uint(&legacy, 1);
    harp_cbor_float(&legacy, 0.25);
    enc_param_body(&ch0, 7, 0.25, 0);
    CHECK(ch0.len == legacy.len && memcmp(ch0.buf, legacy.buf, ch0.len) == 0);

    /* channel round-trips, and an absent key decodes as part 0 */
    uint64_t id = 0;
    CHECK(dec_param_channel(&ch0, &id) == 0 && id == 7);
    enc_param_body(&ch15, 3, 0.5, 15);
    CHECK(dec_param_channel(&ch15, &id) == 15 && id == 3);
    CHECK(ch15.len == ch0.len + 2); /* exactly the "5 => 15" pair added */

    /* an unknown trailing key (e.g. a future field) must be skipped, not fail */
    harp_cbuf evt;
    harp_cbuf_init(&evt);
    harp_cbor_map(&evt, 3);
    harp_cbor_uint(&evt, 0);
    harp_cbor_uint(&evt, 9);
    harp_cbor_uint(&evt, 1);
    harp_cbor_float(&evt, 0.1);
    harp_cbor_uint(&evt, 4); /* txn-id (§9.4 key 4) — we don't consume it */
    harp_cbor_uint(&evt, 42);
    CHECK(dec_param_channel(&evt, &id) == 0 && id == 9);

    harp_cbuf_free(&legacy);
    harp_cbuf_free(&ch0);
    harp_cbuf_free(&ch15);
    harp_cbuf_free(&evt);
}

int main(void) {
    test_sha256();
    test_event_channel();
    test_cbor_encode();
    test_cbor_roundtrip();
    test_frame();
    test_objects();
    test_store();
    test_store_reflist();
    test_envelope();
    test_audio_codec();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
