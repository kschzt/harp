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

    harp_cbuf_free(&m);
    printf("corpus written to %s\n", dir);
    return 0;
}
