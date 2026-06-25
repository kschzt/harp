/* Seed-corpus generator: valid wire bytes via the canonical encoders, so
 * coverage-guided fuzzing starts inside the deep parser paths instead of
 * spending its budget rediscovering the envelope grammar.
 *
 *   gen-corpus OUTDIR    writes one seed file per message shape
 */
#include <stdio.h>
#include <string.h>

#include "harp/audio.h"
#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/object.h"
#include "harp/store.h"

static void dump(const char *dir, const char *name, const void *buf, size_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fwrite(buf, 1, len, f);
    fclose(f);
}

/* capture harp_link_send output as raw bytes */
typedef struct {
    harp_io io;
    uint8_t buf[8192];
    size_t len;
} cap_io;

static bool cap_read(harp_io *io, void *b, size_t n) {
    (void)io;
    (void)b;
    (void)n;
    return false;
}
static bool cap_write(harp_io *io, const void *b, size_t n) {
    cap_io *c = (cap_io *)io;
    if (c->len + n > sizeof c->buf) return false;
    memcpy(c->buf + c->len, b, n);
    c->len += n;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: gen-corpus OUTDIR\n");
        return 2;
    }
    const char *dir = argv[1];
    harp_cbuf m;
    harp_cbuf_init(&m);

    /* envelopes: request with body, response, error, notification */
    harp_env_head(&m, HARP_MSG_REQUEST, 7, "core.hello", true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, 2);
    harp_cbor_uint(&m, 1);
    harp_cbor_uint(&m, 0);
    dump(dir, "env-hello-req", m.buf, m.len);

    harp_cbuf_reset(&m);
    harp_env_head(&m, HARP_MSG_RESPONSE, 7, "state.refs", true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, 1);
    harp_ref r;
    memset(&r, 0, sizeof r);
    snprintf(r.name, sizeof r.name, "live/project");
    r.generation = 42;
    r.dirty = true;
    harp_ref_encode(&m, &r);
    dump(dir, "env-refs-rsp", m.buf, m.len);

    harp_cbuf_reset(&m);
    harp_env_error(&m, 9, "state.refset", "cas-mismatch", "expected hash differs");
    dump(dir, "env-error", m.buf, m.len);

    /* objects: blob -> tree -> snapshot, the whole state closure */
    harp_cbuf_reset(&m);
    const uint8_t params[] = {0xa1, 0x01, 0xf9, 0x38, 0x00}; /* {1: 0.5f16} */
    harp_obj_encode_blob(&m, "application/x.harp-refdev.params", params, sizeof params);
    dump(dir, "obj-blob", m.buf, m.len);
    harp_hash blob_h = harp_hash_compute(m.buf, m.len);

    harp_cbuf_reset(&m);
    harp_tree_entry te[1] = {{"params", blob_h, HARP_OBJ_BLOB}};
    harp_obj_encode_tree(&m, te, 1);
    dump(dir, "obj-tree", m.buf, m.len);
    harp_hash tree_h = harp_hash_compute(m.buf, m.len);

    harp_cbuf_reset(&m);
    harp_obj_encode_snapshot(&m, &tree_h, &blob_h, 1, 1750000000, "device", "1.0.0",
                             "seed snapshot");
    dump(dir, "obj-snapshot", m.buf, m.len);

    /* params-blob parser (§10 / P3 closer) seeds: the INNER CBOR map that
     * refdev_parse_params_blob consumes (no media wrapper — fuzz_state feeds
     * raw bytes straight to the codec). Hand-encoded here rather than via the
     * device encoder so the generator stays on harpcore and so the edge seeds
     * (empty / nested / out-of-range) are expressible. Param ids are 1..13;
     * 16 parts. These start coverage inside the codec's two format arms and
     * its skip/abort branches — the fuzzer mutates outward from there. */

    /* valid NEW 16-part blob: { part(0..15) => { id(1..13) => f32 } } —
     * mirrors refdev_encode_params_blob's shape so it lands in the per-part
     * arm (first outer value is a map). */
    harp_cbuf_reset(&m);
    harp_cbor_map(&m, 16);
    for (uint64_t part = 0; part < 16; part++) {
        harp_cbor_uint(&m, part);
        harp_cbor_map(&m, 13);
        for (uint64_t id = 1; id <= 13; id++) {
            harp_cbor_uint(&m, id);
            harp_cbor_float(&m, 0.5);
        }
    }
    dump(dir, "params-perpart", m.buf, m.len);

    /* valid LEGACY flat blob: { id => f32 } -> part 0. First outer value is a
     * float, so the codec takes the migration arm. */
    harp_cbuf_reset(&m);
    harp_cbor_map(&m, 13);
    for (uint64_t id = 1; id <= 13; id++) {
        harp_cbor_uint(&m, id);
        harp_cbor_float(&m, 0.25);
    }
    dump(dir, "params-legacy", m.buf, m.len);

    /* edge: empty map — well-formed no-op (the n==0 early return). */
    harp_cbuf_reset(&m);
    harp_cbor_map(&m, 0);
    dump(dir, "params-empty", m.buf, m.len);

    /* edge: out-of-range part index (99 >= NPARTS) — the part map is fully
     * consumed but discarded; cursor must stay aligned for the next part. */
    harp_cbuf_reset(&m);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 99); /* out of range -> skipped */
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 3);
    harp_cbor_float(&m, 0.7);
    harp_cbor_uint(&m, 0); /* in range, must still parse after the skip */
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 1);
    harp_cbor_float(&m, 0.9);
    dump(dir, "params-oob-part", m.buf, m.len);

    /* edge: deeply-nested value where a float/map is expected — the codec
     * must reject (peek != map/float) without recursing. A nested-array
     * chain stresses the decoder's container handling on a hostile shape. */
    harp_cbuf_reset(&m);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    for (int depth = 0; depth < 32; depth++) harp_cbor_array(&m, 1);
    harp_cbor_uint(&m, 0);
    dump(dir, "params-nested", m.buf, m.len);

    /* framed link byte stream: one message split across two frames */
    cap_io cap = {{cap_read, cap_write}, {0}, 0};
    harp_cbuf big;
    harp_cbuf_init(&big);
    harp_env_head(&big, HARP_MSG_NOTIFICATION, 0, "core.credit", true);
    harp_cbor_map(&big, 1);
    harp_cbor_uint(&big, 0);
    harp_cbor_uint(&big, 1 << 20);
    harp_link_send(&cap.io, HARP_STREAM_CTL, big.buf, big.len);
    harp_link_send(&cap.io, HARP_STREAM_EVT, params, sizeof params);
    dump(dir, "link-stream", cap.buf, cap.len);
    harp_cbuf_free(&big);

    /* audio frame header */
    harp_audio_hdr h = {0};
    h.fver = HARP_AUDIO_FVER;
    h.dirflags = HARP_AUDIO_DIR_H2D;
    h.slots = 2;
    h.epoch = 3;
    h.ts = 123456;
    h.nsamples = 256;
    h.fmt = HARP_AUDIO_FMT_F32;
    uint8_t hdr[HARP_AUDIO_HDR_LEN];
    harp_audio_hdr_encode(&h, hdr);
    dump(dir, "audio-hdr", hdr, sizeof hdr);

    /* EVT-stream parser (§9.2) seeds for fuzz-evt: handle_evt_msg consumes the bare event
     * message body [ [epoch,msc], etype, body... ] (no envelope/frame wrapper — fuzz_evt feeds
     * raw bytes straight to the parser). These start coverage inside each etype arm so the
     * fuzzer mutates outward from valid wire instead of rediscovering the grammar. epoch 0 ==
     * "now" (always current). Mirrors the shapes the host's evt encoders emit. */
#define EVT_HEAD(et)                     \
    harp_cbuf_reset(&m);                  \
    harp_cbor_array(&m, 3);               \
    harp_cbor_array(&m, 2);               \
    harp_cbor_uint(&m, 0);    /* epoch */ \
    harp_cbor_uint(&m, 1000); /* msc */   \
    harp_cbor_uint(&m, (et))

    /* etype 0: §9.10 UMP carriage — one MIDI-1.0-in-UMP note-on (mt=2, status 0x9). */
    EVT_HEAD(0);
    {
        uint8_t ump[4] = {0x29, 0x90, 60, 100}; /* mt=2, chan=9 -> part 9; note 60 vel 100 */
        harp_cbor_bytes(&m, ump, sizeof ump);
    }
    dump(dir, "evt-ump-noteon", m.buf, m.len);

    /* etype 5: §9.4 ramp { 0 param-id, 1 target, 2 end [epoch,msc] }. */
    EVT_HEAD(5);
    harp_cbor_map(&m, 3);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 3); /* Filter Cutoff */
    harp_cbor_uint(&m, 1);
    harp_cbor_float(&m, 0.75);
    harp_cbor_uint(&m, 2);
    harp_cbor_array(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 2000);
    dump(dir, "evt-ramp", m.buf, m.len);

    /* etype 6: §9.4 non-destructive mod { 0 param-id, 1 signed offset }. */
    EVT_HEAD(6);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 3);
    harp_cbor_uint(&m, 1);
    harp_cbor_float(&m, -0.2);
    dump(dir, "evt-mod", m.buf, m.len);

    /* etype 7: §9.7 transport anchor { 0 flags, 1 tempo, 4 ppq }. */
    EVT_HEAD(7);
    harp_cbor_map(&m, 3);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 1);
    harp_cbor_uint(&m, 1);
    harp_cbor_float(&m, 120.0);
    harp_cbor_uint(&m, 4);
    harp_cbor_float(&m, 4.0);
    dump(dir, "evt-transport", m.buf, m.len);

    /* etype 2/3: §9.6 txn begin { 0 txn-id } then a commit { 0 id } — opens then atomically
     * applies, so the fuzzer reaches both the buffer and the all-or-nothing apply arms. */
    EVT_HEAD(2);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 7); /* txn-id 7 */
    dump(dir, "evt-txn-begin", m.buf, m.len);

    EVT_HEAD(3);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 7);
    dump(dir, "evt-txn-commit", m.buf, m.len);
#undef EVT_HEAD

    harp_cbuf_free(&m);
    printf("corpus written to %s\n", dir);
    return 0;
}
