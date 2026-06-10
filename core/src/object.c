#include "harp/object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harp/sha256.h"

bool harp_hash_eq(const harp_hash *a, const harp_hash *b) {
    return memcmp(a->b, b->b, HARP_HASH_LEN) == 0;
}

void harp_hash_hex(const harp_hash *h, char out[2 * HARP_HASH_LEN + 1]) {
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < HARP_HASH_LEN; i++) {
        out[2 * i] = hexd[h->b[i] >> 4];
        out[2 * i + 1] = hexd[h->b[i] & 0xf];
    }
    out[2 * HARP_HASH_LEN] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool harp_hash_from_hex(const char *hex, harp_hash *out) {
    if (strlen(hex) != 2 * HARP_HASH_LEN) return false;
    for (int i = 0; i < HARP_HASH_LEN; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->b[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

harp_hash harp_hash_compute(const void *data, size_t len) {
    harp_hash h;
    h.b[0] = HARP_HASH_ALG_SHA256;
    harp_sha256_digest(data, len, h.b + 1);
    return h;
}

/* ---- encoders ---- */

void harp_obj_encode_blob(harp_cbuf *out, const char *media_type, const void *payload,
                          size_t payload_len) {
    harp_cbor_map(out, 3);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, HARP_OBJ_BLOB);
    harp_cbor_uint(out, 1);
    harp_cbor_text(out, media_type);
    harp_cbor_uint(out, 2);
    harp_cbor_bytes(out, payload, payload_len);
}

void harp_obj_encode_list(harp_cbuf *out, const char *media_type, const harp_hash *chunks,
                          size_t nchunks) {
    harp_cbor_map(out, 3);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, HARP_OBJ_LIST);
    harp_cbor_uint(out, 1);
    harp_cbor_text(out, media_type);
    harp_cbor_uint(out, 2);
    harp_cbor_array(out, nchunks);
    for (size_t i = 0; i < nchunks; i++)
        harp_cbor_bytes(out, chunks[i].b, HARP_HASH_LEN);
}

/* Deterministic map-key order for text keys: shorter first, then bytewise —
 * identical to comparing the encoded keys, since the CBOR length header
 * sorts below any longer length (RFC 8949 §4.2.1). */
static int entry_cmp(const void *a, const void *b) {
    const harp_tree_entry *ea = a, *eb = b;
    size_t la = strlen(ea->name), lb = strlen(eb->name);
    if (la != lb) return la < lb ? -1 : 1;
    return memcmp(ea->name, eb->name, la);
}

void harp_obj_encode_tree(harp_cbuf *out, const harp_tree_entry *entries, size_t n) {
    harp_tree_entry sorted[256];
    if (n > 256) {
        out->oom = true; /* implementation limit; trees this wide should nest */
        return;
    }
    memcpy(sorted, entries, n * sizeof *entries);
    qsort(sorted, n, sizeof *sorted, entry_cmp);
    harp_cbor_map(out, 2);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, HARP_OBJ_TREE);
    harp_cbor_uint(out, 1);
    harp_cbor_map(out, n);
    for (size_t i = 0; i < n; i++) {
        harp_cbor_text(out, sorted[i].name);
        harp_cbor_array(out, 2);
        harp_cbor_bytes(out, sorted[i].hash.b, HARP_HASH_LEN);
        harp_cbor_uint(out, sorted[i].kind);
    }
}

void harp_obj_encode_snapshot(harp_cbuf *out, const harp_hash *root_tree,
                              const harp_hash *parents, size_t nparents,
                              uint64_t unix_time, const char *author,
                              const char *engine_semver, const char *message) {
    harp_cbor_map(out, message ? 7 : 6);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, HARP_OBJ_SNAPSHOT);
    harp_cbor_uint(out, 1);
    harp_cbor_bytes(out, root_tree->b, HARP_HASH_LEN);
    harp_cbor_uint(out, 2);
    harp_cbor_array(out, nparents);
    for (size_t i = 0; i < nparents; i++)
        harp_cbor_bytes(out, parents[i].b, HARP_HASH_LEN);
    harp_cbor_uint(out, 3);
    harp_cbor_uint(out, unix_time);
    harp_cbor_uint(out, 4);
    harp_cbor_text(out, author);
    harp_cbor_uint(out, 5);
    harp_cbor_text(out, engine_semver);
    if (message) {
        harp_cbor_uint(out, 6);
        harp_cbor_text(out, message);
    }
}

/* ---- parsers ---- */

/* Walk the top-level object map, invoking visit(key, dec) per entry with the
 * decoder positioned at the value. visit must consume exactly the value. */
typedef bool (*field_cb)(uint64_t key, harp_cdec *d, void *ud);

static bool obj_fields(const uint8_t *enc, size_t len, field_cb visit, void *ud) {
    harp_cdec d;
    harp_cdec_init(&d, enc, len);
    uint64_t n;
    if (!harp_cdec_map(&d, &n)) return false;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        if (!visit(key, &d, ud)) return false;
    }
    return !d.err;
}

static bool kind_cb(uint64_t key, harp_cdec *d, void *ud) {
    if (key == 0) {
        uint64_t k;
        if (!harp_cdec_uint(d, &k) || k > 3) return false;
        *(int *)ud = (int)k;
        return true;
    }
    return harp_cdec_skip(d);
}

int harp_obj_kind(const uint8_t *enc, size_t len) {
    int kind = -1;
    if (!obj_fields(enc, len, kind_cb, &kind)) return -1;
    return kind;
}

struct blob_out {
    const char *media;
    size_t media_len;
    const uint8_t *payload;
    size_t payload_len;
    int kind;
};

static bool blob_cb(uint64_t key, harp_cdec *d, void *ud) {
    struct blob_out *o = ud;
    switch (key) {
        case 0: {
            uint64_t k;
            if (!harp_cdec_uint(d, &k)) return false;
            o->kind = (int)k;
            return true;
        }
        case 1:
            return harp_cdec_text(d, &o->media, &o->media_len);
        case 2:
            return harp_cdec_bytes(d, &o->payload, &o->payload_len);
        default:
            return harp_cdec_skip(d);
    }
}

bool harp_obj_parse_blob(const uint8_t *enc, size_t len, const char **media,
                         size_t *media_len, const uint8_t **payload, size_t *payload_len) {
    struct blob_out o = {0};
    o.kind = -1;
    if (!obj_fields(enc, len, blob_cb, &o)) return false;
    if (o.kind != HARP_OBJ_BLOB || !o.payload) return false;
    if (media) *media = o.media;
    if (media_len) *media_len = o.media_len;
    *payload = o.payload;
    *payload_len = o.payload_len;
    return true;
}

struct snap_out {
    harp_hash root;
    bool have_root;
    int kind;
};

static bool read_hash(harp_cdec *d, harp_hash *h) {
    const uint8_t *p;
    size_t n;
    if (!harp_cdec_bytes(d, &p, &n) || n != HARP_HASH_LEN) return false;
    memcpy(h->b, p, HARP_HASH_LEN);
    return true;
}

static bool snap_cb(uint64_t key, harp_cdec *d, void *ud) {
    struct snap_out *o = ud;
    if (key == 0) {
        uint64_t k;
        if (!harp_cdec_uint(d, &k)) return false;
        o->kind = (int)k;
        return true;
    }
    if (key == 1) {
        if (!read_hash(d, &o->root)) return false;
        o->have_root = true;
        return true;
    }
    return harp_cdec_skip(d);
}

bool harp_obj_parse_snapshot_root(const uint8_t *enc, size_t len, harp_hash *root) {
    struct snap_out o = {0};
    o.kind = -1;
    if (!obj_fields(enc, len, snap_cb, &o)) return false;
    if (o.kind != HARP_OBJ_SNAPSHOT || !o.have_root) return false;
    *root = o.root;
    return true;
}

struct tree_iter {
    harp_tree_cb cb;
    void *ud;
    bool stopped;
};

static bool tree_cb(uint64_t key, harp_cdec *d, void *ud) {
    struct tree_iter *it = ud;
    if (key != 1) return harp_cdec_skip(d);
    uint64_t n;
    if (!harp_cdec_map(d, &n)) return false;
    for (uint64_t i = 0; i < n; i++) {
        const char *name;
        size_t name_len;
        uint64_t alen, kind;
        harp_hash h;
        if (!harp_cdec_text(d, &name, &name_len)) return false;
        if (!harp_cdec_array(d, &alen) || alen != 2) return false;
        if (!read_hash(d, &h)) return false;
        if (!harp_cdec_uint(d, &kind)) return false;
        if (!it->stopped && !it->cb(name, name_len, &h, (uint32_t)kind, it->ud))
            it->stopped = true;
    }
    return true;
}

bool harp_obj_tree_foreach(const uint8_t *enc, size_t len, harp_tree_cb cb, void *ud) {
    if (harp_obj_kind(enc, len) != HARP_OBJ_TREE) return false;
    struct tree_iter it = {cb, ud, false};
    return obj_fields(enc, len, tree_cb, &it);
}

/* ---- child enumeration ---- */

struct child_iter {
    harp_hash_cb cb;
    void *ud;
    bool include_parents;
    int kind;
    bool ok;
};

static bool child_tree_entry(const char *name, size_t name_len, const harp_hash *h,
                             uint32_t kind, void *ud) {
    (void)name;
    (void)name_len;
    (void)kind;
    struct child_iter *it = ud;
    return it->cb(h, it->ud);
}

static bool child_cb(uint64_t key, harp_cdec *d, void *ud) {
    struct child_iter *it = ud;
    switch (it->kind) {
        case HARP_OBJ_LIST:
            if (key == 2) {
                uint64_t n;
                if (!harp_cdec_array(d, &n)) return false;
                for (uint64_t i = 0; i < n; i++) {
                    harp_hash h;
                    if (!read_hash(d, &h)) return false;
                    if (!it->cb(&h, it->ud)) it->ok = false;
                }
                return true;
            }
            break;
        case HARP_OBJ_SNAPSHOT:
            if (key == 1) {
                harp_hash h;
                if (!read_hash(d, &h)) return false;
                if (!it->cb(&h, it->ud)) it->ok = false;
                return true;
            }
            if (key == 2 && it->include_parents) {
                uint64_t n;
                if (!harp_cdec_array(d, &n)) return false;
                for (uint64_t i = 0; i < n; i++) {
                    harp_hash h;
                    if (!read_hash(d, &h)) return false;
                    if (!it->cb(&h, it->ud)) it->ok = false;
                }
                return true;
            }
            break;
        default:
            break;
    }
    return harp_cdec_skip(d);
}

bool harp_obj_foreach_child(const uint8_t *enc, size_t len, bool include_parents,
                            harp_hash_cb cb, void *ud) {
    int kind = harp_obj_kind(enc, len);
    if (kind < 0) return false;
    if (kind == HARP_OBJ_BLOB) return true; /* leaves have no children */
    if (kind == HARP_OBJ_TREE) {
        struct child_iter it = {cb, ud, include_parents, kind, true};
        return harp_obj_tree_foreach(enc, len, child_tree_entry, &it);
    }
    struct child_iter it = {cb, ud, include_parents, kind, true};
    return obj_fields(enc, len, child_cb, &it);
}
