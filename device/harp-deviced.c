/* harp-deviced — HARP reference device daemon (spec draft 0.3).
 *
 * Implements the device side of `harp-core` + `harp-recall` over a TCP dev
 * transport (the framed link of §4.2 over a socket). On the Pi 4B the same
 * daemon will later speak FunctionFS bulk endpoints; the link layer is
 * fd-based so only the accept path changes.
 *
 * NOTE (§4.4): TCP here is a development transport for the simulator and for
 * Linux boards (KR260), not a conformance-bearing `harp` network binding.
 *
 * The "engine" is a bank of named parameters — enough to make state real:
 * knob turns dirty the live ref, snapshots serialize it, refsets restore it.
 *
 * Implementation choices where the spec is silent (candidate HEP notes):
 *   - state.want responds {0: count-of-objects-queued} before sending.
 *   - refset closure validation covers root/tree/blob reachability but does
 *     NOT require snapshot parent ancestry to be present (§15.3 "full
 *     closure" would otherwise mean unbounded history in every bundle).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/object.h"
#include "harp/store.h"

#define PROTO_MAJOR 1
#define PROTO_MINOR 0
#define ENGINE_ID "refdev-null"
#define ENGINE_VERSION "1.0.0"
#define FW_VERSION "0.1.0"
#define CREDIT_GRANT (16u << 20)
#define LIVE_REF "live/project"
#define PARAMS_MEDIA "application/x.harp-refdev.params"

typedef struct {
    uint32_t id;
    const char *name;
    float value;
} dev_param;

static dev_param g_params[] = {
    {1, "Osc Pitch", 0.5f},   {2, "Osc Shape", 0.5f},    {3, "Filter Cutoff", 0.5f},
    {4, "Filter Reso", 0.5f}, {5, "Env Attack", 0.5f},   {6, "Env Release", 0.5f},
    {7, "FX Send", 0.5f},     {8, "Master Level", 0.5f},
};
#define NPARAMS (sizeof g_params / sizeof g_params[0])

typedef struct {
    harp_store store;
    char serial[64];
    harp_hash param_map_hash;
    uint64_t boot_count;

    harp_io *io; /* NULL when no session transport is attached */
    harp_link link;
    bool hello_done;
    bool closing;
    uint64_t peer_credit;   /* bytes we may still send on obj */
    uint64_t granted;       /* unconsumed credit we granted the peer */

    /* counters (§14.2) */
    uint64_t frame_errors, session_resets, snapshots_taken;
} device;

static device g_dev;

/* ---------------- engine state <-> objects ---------------- */

static void engine_encode_params_blob(harp_cbuf *out) {
    harp_cbuf payload;
    harp_cbuf_init(&payload);
    harp_cbor_map(&payload, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) { /* ascending ids == deterministic order */
        harp_cbor_uint(&payload, g_params[i].id);
        harp_cbor_float(&payload, g_params[i].value);
    }
    harp_obj_encode_blob(out, PARAMS_MEDIA, payload.buf, payload.len);
    harp_cbuf_free(&payload);
}

/* Serialize live state: params blob -> tree -> snapshot. Returns 0 and the
 * snapshot hash, or -1. */
static int engine_snapshot_objects(device *d, const harp_hash *parent, const char *msg,
                                   harp_hash *out_snap) {
    harp_cbuf enc;
    harp_cbuf_init(&enc);

    engine_encode_params_blob(&enc);
    harp_hash blob_h;
    if (harp_store_put(&d->store, enc.buf, enc.len, &blob_h) != 0) goto fail;

    harp_cbuf_reset(&enc);
    harp_tree_entry entries[1] = {{"params", blob_h, HARP_OBJ_BLOB}};
    harp_obj_encode_tree(&enc, entries, 1);
    harp_hash tree_h;
    if (harp_store_put(&d->store, enc.buf, enc.len, &tree_h) != 0) goto fail;

    harp_cbuf_reset(&enc);
    harp_obj_encode_snapshot(&enc, &tree_h, parent, parent ? 1 : 0,
                             (uint64_t)time(NULL), "device", ENGINE_VERSION, msg);
    if (harp_store_put(&d->store, enc.buf, enc.len, out_snap) != 0) goto fail;

    harp_cbuf_free(&enc);
    return 0;
fail:
    harp_cbuf_free(&enc);
    return -1;
}

/* Load a snapshot into the live engine. Returns 0, or -1 (closure incomplete
 * or malformed — nothing is applied; §11.3 atomic apply). */
struct load_ctx {
    device *d;
    float staged[NPARAMS];
    bool ok;
};

static bool load_tree_entry(const char *name, size_t name_len, const harp_hash *h,
                            uint32_t kind, void *ud) {
    struct load_ctx *ctx = ud;
    if (name_len != 6 || memcmp(name, "params", 6) != 0 || kind != HARP_OBJ_BLOB)
        return true; /* unknown entries are ignored, not fatal */
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    if (harp_store_get(&ctx->d->store, h, &enc) != 0) {
        harp_cbuf_free(&enc);
        ctx->ok = false;
        return false;
    }
    const uint8_t *payload;
    size_t payload_len;
    if (!harp_obj_parse_blob(enc.buf, enc.len, NULL, NULL, &payload, &payload_len)) {
        harp_cbuf_free(&enc);
        ctx->ok = false;
        return false;
    }
    harp_cdec dec;
    harp_cdec_init(&dec, payload, payload_len);
    uint64_t n;
    if (harp_cdec_map(&dec, &n)) {
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id;
            double v;
            if (!harp_cdec_uint(&dec, &id) || !harp_cdec_float(&dec, &v)) break;
            for (size_t j = 0; j < NPARAMS; j++)
                if (g_params[j].id == id) ctx->staged[j] = (float)v;
        }
    }
    harp_cbuf_free(&enc);
    return true;
}

static int engine_load_snapshot(device *d, const harp_hash *snap_h) {
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    if (harp_store_get(&d->store, snap_h, &enc) != 0) goto fail;
    harp_hash root;
    if (!harp_obj_parse_snapshot_root(enc.buf, enc.len, &root)) goto fail;

    harp_cbuf tree_enc;
    harp_cbuf_init(&tree_enc);
    if (harp_store_get(&d->store, &root, &tree_enc) != 0) {
        harp_cbuf_free(&tree_enc);
        goto fail;
    }
    struct load_ctx ctx = {d, {0}, true};
    for (size_t i = 0; i < NPARAMS; i++) ctx.staged[i] = g_params[i].value;
    bool walked = harp_obj_tree_foreach(tree_enc.buf, tree_enc.len, load_tree_entry, &ctx);
    harp_cbuf_free(&tree_enc);
    if (!walked || !ctx.ok) goto fail;

    /* stage, then commit: all-or-nothing into the live engine */
    for (size_t i = 0; i < NPARAMS; i++) g_params[i].value = ctx.staged[i];
    harp_cbuf_free(&enc);
    return 0;
fail:
    harp_cbuf_free(&enc);
    return -1;
}

static void compute_param_map_hash(device *d) {
    harp_cbuf b;
    harp_cbuf_init(&b);
    harp_cbor_array(&b, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) {
        harp_cbor_map(&b, 2);
        harp_cbor_uint(&b, 0);
        harp_cbor_uint(&b, g_params[i].id);
        harp_cbor_uint(&b, 1);
        harp_cbor_text(&b, g_params[i].name);
    }
    d->param_map_hash = harp_hash_compute(b.buf, b.len);
    harp_cbuf_free(&b);
}

/* ---------------- wire helpers ---------------- */

static int send_ctl(device *d, const harp_cbuf *msg) {
    return harp_link_send(d->io, HARP_STREAM_CTL, msg->buf, msg->len);
}

static void ntf_state_changed(device *d, const harp_ref *r) {
    if (!d->io || !d->hello_done) return;
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "state.changed", true);
    harp_cbor_map(&m, 4);
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, r->name);
    harp_cbor_uint(&m, 1);
    if (r->unborn)
        harp_cbor_null(&m);
    else
        harp_cbor_bytes(&m, r->hash.b, HARP_HASH_LEN);
    harp_cbor_uint(&m, 2);
    harp_cbor_uint(&m, r->generation);
    harp_cbor_uint(&m, 3);
    harp_cbor_bool(&m, r->dirty);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void grant_credit(device *d) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "core.credit", true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, CREDIT_GRANT);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    d->granted += CREDIT_GRANT;
}

static void send_error(device *d, uint64_t rid, const char *method, const char *code,
                       const char *msg) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_error(&m, rid, method, code, msg);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* ---------------- identity (§6.2) ---------------- */

static void encode_identity(device *d, harp_cbuf *m) {
    harp_cbor_map(m, 11);
    harp_cbor_uint(m, 0); /* vendor */
    harp_cbor_map(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 0x1209); /* pid.codes prototype VID */
    harp_cbor_uint(m, 1);
    harp_cbor_text(m, "HARP Reference Project");
    harp_cbor_uint(m, 1); /* product */
    harp_cbor_map(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 0x0001);
    harp_cbor_uint(m, 1);
    harp_cbor_text(m, "harp-refdev");
    harp_cbor_uint(m, 2); /* serial */
    harp_cbor_text(m, d->serial);
    harp_cbor_uint(m, 3); /* firmware */
    harp_cbor_text(m, FW_VERSION);
    harp_cbor_uint(m, 4); /* engine */
    harp_cbor_map(m, 3);
    harp_cbor_uint(m, 0);
    harp_cbor_text(m, ENGINE_ID);
    harp_cbor_uint(m, 1);
    harp_cbor_text(m, ENGINE_VERSION);
    harp_cbor_uint(m, 2);
    harp_cbor_bytes(m, d->param_map_hash.b, HARP_HASH_LEN);
    harp_cbor_uint(m, 5); /* protocol in effect */
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, PROTO_MAJOR);
    harp_cbor_uint(m, PROTO_MINOR);
    harp_cbor_uint(m, 6); /* capabilities */
    harp_cbor_array(m, 3);
    harp_cbor_text(m, "harp-core");
    harp_cbor_text(m, "harp-recall");
    harp_cbor_text(m, "x.harp-refdev.sim");
    harp_cbor_uint(m, 7); /* channel map: no audio yet */
    harp_cbor_array(m, 0);
    harp_cbor_uint(m, 8); /* latency profile: no audio yet */
    harp_cbor_array(m, 0);
    harp_cbor_uint(m, 9); /* build id */
    harp_cbor_text(m, "refdev sim " __DATE__);
    harp_cbor_uint(m, 10); /* boot count */
    harp_cbor_uint(m, d->boot_count);
}

/* ---------------- ref helpers ---------------- */

/* Bump generation +/- dirty on the live ref and notify. */
static void live_ref_touch(device *d, bool dirty) {
    harp_ref r;
    if (harp_store_ref_read(&d->store, LIVE_REF, &r) != 0) return;
    r.generation++;
    r.dirty = dirty;
    if (harp_store_ref_write(&d->store, &r) == 0) ntf_state_changed(d, &r);
}

static int do_snapshot(device *d, const char *msg, harp_hash *out, uint64_t *out_gen) {
    harp_ref r;
    if (harp_store_ref_read(&d->store, LIVE_REF, &r) != 0) return -1;
    harp_hash snap;
    if (engine_snapshot_objects(d, r.unborn ? NULL : &r.hash, msg, &snap) != 0) return -1;
    r.unborn = false;
    r.hash = snap;
    r.generation++;
    r.dirty = false;
    if (harp_store_ref_write(&d->store, &r) != 0) return -1;
    d->snapshots_taken++;
    ntf_state_changed(d, &r);
    *out = snap;
    if (out_gen) *out_gen = r.generation;
    return 0;
}

/* Closure check for refset (§11.3): root snapshot -> tree -> children present.
 * Parent ancestry deliberately not required (see header note). */
struct closure_ctx {
    device *d;
    bool complete;
    int depth;
};

static bool closure_visit(const harp_hash *h, void *ud);

static void closure_walk(struct closure_ctx *ctx, const harp_hash *h) {
    if (!ctx->complete || ctx->depth > 16) return;
    if (!harp_store_have(&ctx->d->store, h)) {
        ctx->complete = false;
        return;
    }
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    if (harp_store_get(&ctx->d->store, h, &enc) != 0) {
        ctx->complete = false;
    } else {
        ctx->depth++;
        if (!harp_obj_foreach_child(enc.buf, enc.len, false, closure_visit, ctx))
            ctx->complete = false;
        ctx->depth--;
    }
    harp_cbuf_free(&enc);
}

static bool closure_visit(const harp_hash *h, void *ud) {
    closure_walk(ud, h);
    return ((struct closure_ctx *)ud)->complete;
}

/* ---------------- method handlers ---------------- */

static void rsp_head(harp_cbuf *m, uint64_t rid, const char *method, bool has_body) {
    harp_env_head(m, HARP_MSG_RESPONSE, rid, method, has_body);
}

static void handle_hello(device *d, const harp_env *e) {
    uint64_t peer_major = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 0) {
                    uint64_t alen, mj, mi;
                    if (harp_cdec_array(&b, &alen) && alen == 2 &&
                        harp_cdec_uint(&b, &mj) && harp_cdec_uint(&b, &mi))
                        peer_major = mj;
                } else {
                    harp_cdec_skip(&b);
                }
            }
        }
    }
    if (peer_major != PROTO_MAJOR) {
        send_error(d, e->rid, e->method, "incompatible", "device supports protocol 1.x only");
        return;
    }
    /* §5.4: hello resets all per-session state */
    d->hello_done = true;
    d->peer_credit = 0;
    d->granted = 0;

    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, 2);
    harp_cbor_uint(&m, PROTO_MAJOR);
    harp_cbor_uint(&m, PROTO_MINOR);
    harp_cbor_uint(&m, 1);
    encode_identity(d, &m);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    grant_credit(d);
}

struct refs_collect {
    harp_cbuf items; /* concatenated encoded refs */
    size_t count;
};

static void refs_cb(const harp_ref *r, void *ud) {
    struct refs_collect *c = ud;
    harp_ref_encode(&c->items, r);
    c->count++;
}

static void handle_state_refs(device *d, const harp_env *e) {
    struct refs_collect c = {{0}, 0};
    harp_cbuf_init(&c.items);
    harp_store_ref_list(&d->store, refs_cb, &c);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, c.count);
    harp_cbuf_put(&m, c.items.buf, c.items.len);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    harp_cbuf_free(&c.items);
}

static void handle_state_snapshot(device *d, const harp_env *e) {
    char refname[HARP_REF_NAME_MAX] = "";
    char msg[256] = "";
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                const char *s;
                size_t sl;
                if (key == 0 && harp_cdec_text(&b, &s, &sl) && sl < sizeof refname) {
                    memcpy(refname, s, sl);
                    refname[sl] = 0;
                } else if (key == 1 && harp_cdec_text(&b, &s, &sl) && sl < sizeof msg) {
                    memcpy(msg, s, sl);
                    msg[sl] = 0;
                } else if (key > 1) {
                    harp_cdec_skip(&b);
                }
            }
        }
    }
    if (strcmp(refname, LIVE_REF) != 0) {
        send_error(d, e->rid, e->method, "unsupported", "refdev snapshots live/project only");
        return;
    }
    harp_hash snap;
    uint64_t gen;
    if (do_snapshot(d, msg[0] ? msg : NULL, &snap, &gen) != 0) {
        send_error(d, e->rid, e->method, "storage", NULL);
        return;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_bytes(&m, snap.b, HARP_HASH_LEN);
    harp_cbor_uint(&m, 1);
    harp_cbor_uint(&m, gen);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_state_have(device *d, const harp_env *e) {
    harp_cbuf bools;
    harp_cbuf_init(&bools);
    size_t count = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n) && n >= 1) {
            uint64_t key, alen;
            if (harp_cdec_uint(&b, &key) && key == 0 && harp_cdec_array(&b, &alen)) {
                for (uint64_t i = 0; i < alen; i++) {
                    const uint8_t *p;
                    size_t pl;
                    if (!harp_cdec_bytes(&b, &p, &pl) || pl != HARP_HASH_LEN) break;
                    harp_hash h;
                    memcpy(h.b, p, HARP_HASH_LEN);
                    harp_cbor_bool(&bools, harp_store_have(&d->store, &h));
                    count++;
                }
            }
        }
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, count);
    harp_cbuf_put(&m, bools.buf, bools.len);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    harp_cbuf_free(&bools);
}

static void handle_state_want(device *d, const harp_env *e) {
    /* collect hashes we hold, respond with queued count, then send objects */
    harp_hash queue[256];
    size_t nq = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n) && n >= 1) {
            uint64_t key, alen;
            if (harp_cdec_uint(&b, &key) && key == 0 && harp_cdec_array(&b, &alen)) {
                for (uint64_t i = 0; i < alen; i++) {
                    const uint8_t *p;
                    size_t pl;
                    if (!harp_cdec_bytes(&b, &p, &pl) || pl != HARP_HASH_LEN) break;
                    if (nq < 256) {
                        memcpy(queue[nq].b, p, HARP_HASH_LEN);
                        if (harp_store_have(&d->store, &queue[nq])) nq++;
                    }
                }
            }
        }
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, nq);
    send_ctl(d, &m);
    harp_cbuf_free(&m);

    harp_cbuf enc;
    harp_cbuf_init(&enc);
    for (size_t i = 0; i < nq; i++) {
        harp_cbuf_reset(&enc);
        if (harp_store_get(&d->store, &queue[i], &enc) != 0) continue;
        /* §4.2.1: never exceed granted credit. The simulator grants 16 MiB up
         * front and objects are small; a full implementation would queue. */
        if (enc.len <= d->peer_credit) d->peer_credit -= enc.len;
        harp_link_send(d->io, HARP_STREAM_OBJ, enc.buf, enc.len);
    }
    harp_cbuf_free(&enc);
}

static void handle_state_send(device *d, const harp_env *e) {
    uint64_t total = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 1) {
                    if (!harp_cdec_uint(&b, &total)) break;
                } else {
                    harp_cdec_skip(&b);
                }
            }
        }
    }
    struct statvfs vs;
    if (statvfs(d->store.dir, &vs) == 0) {
        uint64_t freeb = (uint64_t)vs.f_bavail * vs.f_frsize;
        if (total > freeb) { /* §11.5: refuse BEFORE transfer */
            send_error(d, e->rid, e->method, "storage", "insufficient free space");
            return;
        }
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_state_refset(device *d, const harp_env *e) {
    char refname[HARP_REF_NAME_MAX] = "";
    bool expect_null = false, have_expect = false, have_new = false;
    harp_hash expect, newh;
    uint64_t flags = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                switch (key) {
                    case 0: {
                        const char *s;
                        size_t sl;
                        if (harp_cdec_text(&b, &s, &sl) && sl < sizeof refname) {
                            memcpy(refname, s, sl);
                            refname[sl] = 0;
                        }
                        break;
                    }
                    case 1:
                        if (harp_cdec_peek_null(&b)) {
                            harp_cdec_null(&b);
                            expect_null = true;
                            have_expect = true;
                        } else {
                            const uint8_t *p;
                            size_t pl;
                            if (harp_cdec_bytes(&b, &p, &pl) && pl == HARP_HASH_LEN) {
                                memcpy(expect.b, p, HARP_HASH_LEN);
                                have_expect = true;
                            }
                        }
                        break;
                    case 2: {
                        const uint8_t *p;
                        size_t pl;
                        if (harp_cdec_bytes(&b, &p, &pl) && pl == HARP_HASH_LEN) {
                            memcpy(newh.b, p, HARP_HASH_LEN);
                            have_new = true;
                        }
                        break;
                    }
                    case 3:
                        harp_cdec_uint(&b, &flags);
                        break;
                    default:
                        harp_cdec_skip(&b);
                }
            }
        }
    }
    if (!refname[0] || !have_expect || !have_new) {
        send_error(d, e->rid, e->method, "malformed", NULL);
        return;
    }
    bool create = flags & 1, force = flags & 2;

    harp_ref r;
    if (harp_store_ref_read(&d->store, refname, &r) != 0) {
        send_error(d, e->rid, e->method, "malformed", "bad ref name");
        return;
    }

    /* CAS (§11.3): expect matches AND !dirty, unless force */
    bool expect_ok =
        r.unborn ? (expect_null || create)
                 : (!expect_null && harp_hash_eq(&r.hash, &expect));
    if (!force && (!expect_ok || r.dirty)) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        harp_env_head(&m, HARP_MSG_ERROR, e->rid, e->method, true);
        harp_cbor_map(&m, 3);
        harp_cbor_uint(&m, 0);
        harp_cbor_text(&m, "conflict");
        harp_cbor_uint(&m, 1);
        harp_cbor_text(&m, r.dirty ? "ref is dirty" : "expect mismatch");
        harp_cbor_uint(&m, 2);
        harp_cbor_map(&m, 3);
        harp_cbor_uint(&m, 0);
        if (r.unborn)
            harp_cbor_null(&m);
        else
            harp_cbor_bytes(&m, r.hash.b, HARP_HASH_LEN);
        harp_cbor_uint(&m, 1);
        harp_cbor_uint(&m, r.generation);
        harp_cbor_uint(&m, 2);
        harp_cbor_bool(&m, r.dirty);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
        return;
    }

    /* closure must be fully present before any effect (§11.3 atomic apply) */
    struct closure_ctx cc = {d, true, 0};
    closure_walk(&cc, &newh);
    if (!cc.complete) {
        send_error(d, e->rid, e->method, "not-found", "object closure incomplete");
        return;
    }

    /* live refs activate in the engine; others are storage-only */
    if (strcmp(refname, LIVE_REF) == 0) {
        if (engine_load_snapshot(d, &newh) != 0) {
            send_error(d, e->rid, e->method, "malformed", "target is not a loadable snapshot");
            return;
        }
    }
    r.unborn = false;
    r.hash = newh;
    r.generation++;
    r.dirty = false;
    if (harp_store_ref_write(&d->store, &r) != 0) {
        send_error(d, e->rid, e->method, "storage", NULL);
        return;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, r.generation);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    ntf_state_changed(d, &r);
}

static void handle_diag_counters(device *d, const harp_env *e) {
    struct statvfs vs;
    uint64_t total = 0, freeb = 0;
    if (statvfs(d->store.dir, &vs) == 0) {
        total = (uint64_t)vs.f_blocks * vs.f_frsize;
        freeb = (uint64_t)vs.f_bavail * vs.f_frsize;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 12);
    harp_cbor_text(&m, "usb_errors");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "frame_errors");
    harp_cbor_uint(&m, d->frame_errors);
    harp_cbor_text(&m, "audio_underruns");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "audio_overruns");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "audio_late_frames");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "msc_discontinuities");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "clock_drift_ppb");
    harp_cbor_int(&m, 0);
    harp_cbor_text(&m, "evt_late");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "evt_stale_epoch");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "session_resets");
    harp_cbor_uint(&m, d->session_resets);
    harp_cbor_text(&m, "storage_bytes_total");
    harp_cbor_uint(&m, total);
    harp_cbor_text(&m, "storage_bytes_free");
    harp_cbor_uint(&m, freeb);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* x.harp-refdev.knob {0: param-id, 1: value} — front-panel simulation */
static void handle_knob(device *d, const harp_env *e) {
    uint64_t id = 0;
    double v = 0;
    bool ok = false;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n) && n >= 2) {
            uint64_t key;
            if (harp_cdec_uint(&b, &key) && key == 0 && harp_cdec_uint(&b, &id) &&
                harp_cdec_uint(&b, &key) && key == 1 && harp_cdec_float(&b, &v))
                ok = true;
        }
    }
    if (!ok) {
        send_error(d, e->rid, e->method, "malformed", NULL);
        return;
    }
    bool found = false;
    for (size_t i = 0; i < NPARAMS; i++) {
        if (g_params[i].id == id) {
            if (v < 0) v = 0;
            if (v > 1) v = 1;
            g_params[i].value = (float)v;
            found = true;
        }
    }
    if (!found) {
        send_error(d, e->rid, e->method, "not-found", "no such param");
        return;
    }
    live_ref_touch(d, true); /* any knob turn: dirty=true, generation++ (§10.3) */
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_dev_params(device *d, const harp_env *e) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) {
        harp_cbor_array(&m, 3);
        harp_cbor_uint(&m, g_params[i].id);
        harp_cbor_text(&m, g_params[i].name);
        harp_cbor_float(&m, g_params[i].value);
    }
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* ---------------- dispatch ---------------- */

static void handle_ctl(device *d, const uint8_t *buf, size_t len) {
    harp_env e;
    if (!harp_env_parse(buf, len, &e)) {
        d->frame_errors++;
        return;
    }
    if (e.msgtype == HARP_MSG_NOTIFICATION) {
        if (strcmp(e.method, "core.credit") == 0 && e.has_body) {
            harp_cdec b;
            harp_cdec_init(&b, e.body, e.body_len);
            uint64_t n, key, v;
            if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
                harp_cdec_uint(&b, &v))
                d->peer_credit += v;
        }
        return; /* unknown notifications are ignored (§5.2) */
    }
    if (e.msgtype != HARP_MSG_REQUEST) return;

    if (!d->hello_done && strcmp(e.method, "core.hello") != 0) {
        send_error(d, e.rid, e.method, "denied", "core.hello required first");
        return;
    }
    if (strcmp(e.method, "core.hello") == 0)
        handle_hello(d, &e);
    else if (strcmp(e.method, "core.identify") == 0) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, true);
        encode_identity(d, &m);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
    } else if (strcmp(e.method, "core.ping") == 0) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, e.has_body);
        if (e.has_body) harp_cbuf_put(&m, e.body, e.body_len);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
    } else if (strcmp(e.method, "core.bye") == 0) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, false);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
        d->closing = true;
    } else if (strcmp(e.method, "state.refs") == 0)
        handle_state_refs(d, &e);
    else if (strcmp(e.method, "state.snapshot") == 0)
        handle_state_snapshot(d, &e);
    else if (strcmp(e.method, "state.have") == 0)
        handle_state_have(d, &e);
    else if (strcmp(e.method, "state.want") == 0)
        handle_state_want(d, &e);
    else if (strcmp(e.method, "state.send") == 0)
        handle_state_send(d, &e);
    else if (strcmp(e.method, "state.refset") == 0)
        handle_state_refset(d, &e);
    else if (strcmp(e.method, "diag.counters") == 0)
        handle_diag_counters(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.knob") == 0)
        handle_knob(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.params") == 0)
        handle_dev_params(d, &e);
    else
        send_error(d, e.rid, e.method, "unsupported", NULL);
}

static void handle_obj(device *d, const uint8_t *buf, size_t len) {
    /* one object per obj-stream message; verify-on-receipt is intrinsic */
    if (harp_store_put(&d->store, buf, len, NULL) != 0) d->frame_errors++;
    if (d->granted >= len)
        d->granted -= len;
    else
        d->granted = 0;
    if (d->granted < CREDIT_GRANT / 2) grant_credit(d);
}

/* ---------------- session / main ---------------- */

void harp_deviced_run_session(device *d, harp_io *io) {
    d->io = io;
    d->hello_done = false;
    d->closing = false;
    harp_link_init(&d->link);
    harp_cbuf msg;
    harp_cbuf_init(&msg);
    for (;;) {
        uint8_t stream;
        int rc = harp_link_recv(io, &d->link, &stream, &msg);
        if (rc == -1) break; /* peer gone */
        if (rc == -2) {      /* protocol violation: session reset (§12.4) */
            d->session_resets++;
            break;
        }
        if (stream == HARP_STREAM_CTL)
            handle_ctl(d, msg.buf, msg.len);
        else if (stream == HARP_STREAM_OBJ)
            handle_obj(d, msg.buf, msg.len);
        /* evt/log: not yet */
        if (d->closing) break;
    }
    harp_cbuf_free(&msg);
    harp_link_free(&d->link);
    d->io = NULL;
}

#ifdef __linux__
/* FunctionFS gadget transport (device/ffs.c) */
int harp_ffs_serve(const char *ffs_dir, const char *gadget_path,
                   void (*session)(void *ud, harp_io *io), void *ud);

static void ffs_session_cb(void *ud, harp_io *io) {
    harp_deviced_run_session(ud, io);
    fprintf(stderr, "harp-deviced: usb session ended; awaiting reattach\n");
}
#endif

static uint64_t bump_boot_count(const char *dir) {
    char path[600];
    snprintf(path, sizeof path, "%s/bootcount", dir);
    uint64_t n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%llu", (unsigned long long *)&n) != 1) n = 0;
        fclose(f);
    }
    n++;
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%llu\n", (unsigned long long)n);
        fclose(f);
    }
    return n;
}

int main(int argc, char **argv) {
    const char *state_dir = "./refdev-state";
    const char *serial = "SIM-0001";
    const char *ffs_dir = NULL;
    const char *gadget = "/sys/kernel/config/usb_gadget/harp";
    int port = 47800;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc)
            state_dir = argv[++i];
        else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc)
            serial = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ffs") == 0 && i + 1 < argc)
            ffs_dir = argv[++i];
        else if (strcmp(argv[i], "--gadget") == 0 && i + 1 < argc)
            gadget = argv[++i];
        else {
            fprintf(stderr,
                    "usage: harp-deviced [--state-dir DIR] [--serial S] "
                    "[--port P | --ffs FFS_DIR [--gadget CONFIGFS_PATH]]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    device *d = &g_dev;
    memset(d, 0, sizeof *d);
    d->io = NULL;
    snprintf(d->serial, sizeof d->serial, "%s", serial);
    if (harp_store_open(&d->store, state_dir) != 0) {
        fprintf(stderr, "harp-deviced: cannot open state dir %s\n", state_dir);
        return 1;
    }
    d->boot_count = bump_boot_count(state_dir);
    compute_param_map_hash(d);

    /* Recall across power cycles: load the live ref if clean; first boot
     * snapshots the factory state so the ref is born. */
    harp_ref live;
    if (harp_store_ref_read(&d->store, LIVE_REF, &live) == 0) {
        if (live.unborn) {
            harp_hash snap;
            if (do_snapshot(d, "factory state", &snap, NULL) == 0)
                fprintf(stderr, "harp-deviced: initialized factory state\n");
        } else if (!live.dirty) {
            if (engine_load_snapshot(d, &live.hash) == 0)
                fprintf(stderr, "harp-deviced: restored live/project (gen %llu)\n",
                        (unsigned long long)live.generation);
        }
    }

    if (ffs_dir) {
#ifdef __linux__
        fprintf(stderr, "harp-deviced: serial %s, state %s, USB gadget via %s (boot %llu)\n",
                d->serial, state_dir, ffs_dir, (unsigned long long)d->boot_count);
        return harp_ffs_serve(ffs_dir, gadget, ffs_session_cb, d);
#else
        (void)gadget;
        fprintf(stderr, "harp-deviced: --ffs requires Linux\n");
        return 2;
#endif
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(sfd, 4) != 0) {
        fprintf(stderr, "harp-deviced: cannot listen on port %d: %s\n", port,
                strerror(errno));
        return 1;
    }
    fprintf(stderr, "harp-deviced: serial %s, state %s, listening on %d (boot %llu)\n",
            d->serial, state_dir, port, (unsigned long long)d->boot_count);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        harp_io_fd tio;
        harp_io_fd_init(&tio, cfd, cfd);
        harp_deviced_run_session(d, &tio.io);
        close(cfd);
        fprintf(stderr, "harp-deviced: session ended; awaiting reattach\n");
    }
    return 0;
}
