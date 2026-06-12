/* state.c — engine state <-> content-addressed objects (split from
 * harp-deviced.c; see device.h).
 *
 * Serialization (params blob / tree / snapshot, §10), the param-map hash,
 * live-ref write coalescing (§10.3), the canonical front-panel set path,
 * snapshot capture (§11.2), and the refset closure walk (§11.3).
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "device.h"

/* ---------------- engine state <-> objects ---------------- */

static void engine_encode_params_blob(harp_cbuf *out) {
    harp_cbuf payload;
    harp_cbuf_init(&payload);
    harp_cbor_map(&payload, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) { /* ascending ids == deterministic order */
        harp_cbor_uint(&payload, g_params[i].id);
        harp_cbor_float(&payload, param_get(&g_params[i]));
    }
    harp_obj_encode_blob(out, PARAMS_MEDIA, payload.buf, payload.len);
    harp_cbuf_free(&payload);
}

/* Serialize live state: params blob -> tree -> snapshot. Returns 0 and the
 * snapshot hash, or -1. */
int engine_snapshot_objects(device *d, const harp_hash *parent, const char *msg,
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

int engine_load_snapshot(device *d, const harp_hash *snap_h) {
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
    for (size_t i = 0; i < NPARAMS; i++) ctx.staged[i] = param_get(&g_params[i]);
    bool walked = harp_obj_tree_foreach(tree_enc.buf, tree_enc.len, load_tree_entry, &ctx);
    harp_cbuf_free(&tree_enc);
    if (!walked || !ctx.ok) goto fail;

    /* stage, then commit: all-or-nothing into the live engine */
    for (size_t i = 0; i < NPARAMS; i++) param_put(&g_params[i], ctx.staged[i]);
    harp_cbuf_free(&enc);
    return 0;
fail:
    harp_cbuf_free(&enc);
    return -1;
}

/* Canonical parameter descriptor array (§9.3). param-map-hash is the
 * SHA-256 of this exact deterministic encoding — identity and evt.params
 * MUST agree. */
void encode_param_array(harp_cbuf *b) {
    harp_cbor_array(b, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) {
        harp_cbor_map(b, 3);
        harp_cbor_uint(b, 0);
        harp_cbor_uint(b, g_params[i].id);
        harp_cbor_uint(b, 1);
        harp_cbor_text(b, g_params[i].name);
        harp_cbor_uint(b, 8);
        harp_cbor_uint(b, 0x1); /* flags: automatable */
    }
}

void compute_param_map_hash(device *d) {
    harp_cbuf b;
    harp_cbuf_init(&b);
    encode_param_array(&b);
    d->param_map_hash = harp_hash_compute(b.buf, b.len);
    harp_cbuf_free(&b);
}

/* ---------------- ref helpers ---------------- */

/* Bump generation + dirty on the live ref. Persists only the clean->dirty
 * transition; later bumps coalesce in memory (see device struct comment). */
void live_ref_touch(device *d, bool dirty) {
    pthread_mutex_lock(&d->state_mu);
    if (!d->live_cache_valid) {
        if (harp_store_ref_read(&d->store, LIVE_REF, &d->live_cache) != 0) {
            pthread_mutex_unlock(&d->state_mu);
            return;
        }
        d->live_cache_valid = true;
    }
    bool transition = dirty && !d->live_cache.dirty;
    d->live_cache.generation++;
    d->live_cache.dirty = dirty;
    if (transition) harp_store_ref_write(&d->store, &d->live_cache);
    uint64_t now = now_ms();
    bool do_ntf = transition || now - d->last_live_ntf_ms >= 250;
    harp_ref snapshot_ref = d->live_cache;
    if (do_ntf) d->last_live_ntf_ms = now;
    pthread_mutex_unlock(&d->state_mu);
    if (do_ntf) ntf_state_changed(d, &snapshot_ref);
}

/* Flush the coalesced live ref before anything reads it from storage. */
void live_cache_flush(device *d) {
    pthread_mutex_lock(&d->state_mu);
    if (d->live_cache_valid) {
        harp_store_ref_write(&d->store, &d->live_cache);
        d->live_cache_valid = false; /* re-read after external mutation */
    }
    pthread_mutex_unlock(&d->state_mu);
}

/* The one true front-panel path: web panel, vendor knob method, and any
 * future GPIO encoders all come through here — set, dirty, echo. */
bool front_panel_set(device *d, uint32_t id, double v) {
    bool found = false;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    for (size_t i = 0; i < NPARAMS; i++)
        if (g_params[i].id == id) {
            param_put(&g_params[i], (float)v);
            found = true;
        }
    if (!found) return false;
    live_ref_touch(d, true);
    evt_echo_param(d, id, (float)v);
    return true;
}

int do_snapshot(device *d, const char *msg, harp_hash *out, uint64_t *out_gen) {
    live_cache_flush(d);
    harp_ref r;
    if (harp_store_ref_read(&d->store, LIVE_REF, &r) != 0) return -1;
    harp_hash snap;
    if (engine_snapshot_objects(d, r.unborn ? NULL : &r.hash, msg, &snap) != 0) return -1;
    r.unborn = false;
    r.hash = snap;
    r.generation++;
    r.dirty = false;
    if (harp_store_ref_write(&d->store, &r) != 0) return -1;
    CTR_INC(d->snapshots_taken);
    ntf_state_changed(d, &r);
    *out = snap;
    if (out_gen) *out_gen = r.generation;
    return 0;
}

/* Closure check for refset (§11.3): root snapshot -> tree -> children present.
 * Parent ancestry deliberately not required (see header note). */
static bool closure_visit(const harp_hash *h, void *ud);

void closure_walk(struct closure_ctx *ctx, const harp_hash *h) {
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

