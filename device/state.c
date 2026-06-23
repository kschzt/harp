/* state.c — engine state <-> content-addressed objects (split from
 * harp-deviced.c; see device.h).
 *
 * Serialization (params blob / tree / snapshot, §10), the param-map hash,
 * live-ref write coalescing (§10.3), the canonical front-panel set path,
 * snapshot capture (§11.2), and the refset closure walk (§11.3).
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "device.h"

/* ---------------- engine state <-> objects ---------------- */

/* P3: the params blob is the SHARED CONTRACT multitimbral format —
 *     CBOR map { partIndex => { paramId => value } }
 * for ALL 16 parts, parts in ascending index, params in ascending id (the
 * g_params table order). Deterministic (content-addressed): same values ->
 * same bytes. The format change alters the state HASH vs P2.2 — EXPECTED and
 * fine (the golden checks rendered AUDIO; recall round-trips the new format
 * both ways). The OLD flat { paramId => value } map still loads (part 0).
 *
 * P3 closer: the byte-level serialization is the PURE codec below
 * (refdev_encode_params_blob / refdev_parse_params_blob). engine_encode_params_blob
 * is now just the engine glue — snapshot the 16 parts into a float grid, run the
 * pure encoder, wrap the result in the PARAMS_MEDIA blob header. The emitted bytes
 * are byte-identical to the prior inline encoder (same media, same ascending
 * order), so recall hashes and existing snapshots are unchanged. */

/* PURE encoder: float grid -> inner CBOR map (no media wrapper, no store, no
 * g_parts). Mirrors the prior inline order exactly: parts 0..15 ascending,
 * params in g_params (== ascending id) order, f32 values via harp_cbor_float. */
void refdev_encode_params_blob(const float v[NPARTS][NPARAMS], harp_cbuf *payload) {
    harp_cbor_map(payload, NPARTS); /* part index => params map */
    for (int pi = 0; pi < NPARTS; pi++) {
        harp_cbor_uint(payload, (uint64_t)pi);
        harp_cbor_map(payload, NPARAMS);
        for (size_t i = 0; i < NPARAMS; i++) { /* ascending ids == deterministic order */
            harp_cbor_uint(payload, g_params[i].id);
            harp_cbor_float(payload, v[pi][i]);
        }
    }
}

/* PURE decoder: inner CBOR map -> float grid + presence mask. Fails clean on
 * ANY structural error (bad CBOR / truncation) by returning false; an
 * out-of-range partIdx or unknown paramId is skipped, never fatal on its own,
 * and never writes outside the grid. See header for the format contract.
 *
 * THE SMELL FIX (P3 critic): the prior inline parser broke out of an inner map
 * on a malformed pair WITHOUT consuming the rest, leaving the outer decoder
 * misaligned — it then read the following bytes as a bogus part index. Here
 * every inner pair that fails to decode is a STRUCTURAL error -> abort whole
 * parse; a well-formed-but-unknown id is consumed (value too) and skipped, so
 * the cursor stays aligned for the remaining parts. */

/* Stage one part's params from an inner { paramId => value } map at *dec.
 * pi is -1 to discard (out-of-range part): the pairs are still fully consumed
 * so the outer cursor stays aligned. Returns false on a structural decode
 * error (the caller aborts the whole parse). */
/* Store one (id, value) onto part pi's grid column, if the id is one of ours.
 * Unknown ids are ignored (forward-compat). Single source of the id->column
 * lookup — the parse paths below all route through here. */
static void grid_set(int pi, uint64_t id, double val,
                     float v[NPARTS][NPARAMS], bool present[NPARTS][NPARAMS]) {
    for (size_t j = 0; j < NPARAMS; j++)
        if (g_params[j].id == id) {
            v[pi][j] = (float)val;
            present[pi][j] = true;
            return;
        }
}

static bool parse_part_map(harp_cdec *dec, int pi,
                           float v[NPARTS][NPARAMS], bool present[NPARTS][NPARAMS]) {
    uint64_t n;
    if (!harp_cdec_map(dec, &n)) return false;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t id;
        double val;
        /* both halves of the pair are always consumed — id then value — so a
         * truncated/garbled pair is a structural failure (false), not a silent
         * break that strands the cursor mid-map (the old bug) */
        if (!harp_cdec_uint(dec, &id) || !harp_cdec_float(dec, &val)) return false;
        if (pi >= 0) grid_set(pi, id, val, v, present); /* pi<0: consumed, discarded */
    }
    return true;
}

bool refdev_parse_params_blob(const uint8_t *payload, size_t len,
                              float v[NPARTS][NPARAMS], bool present[NPARTS][NPARAMS]) {
    harp_cdec dec;
    harp_cdec_init(&dec, payload, len);
    uint64_t n;
    if (!harp_cdec_map(&dec, &n)) return false; /* not a map at all: malformed */
    if (n == 0) return true;                    /* empty map: no-op, well-formed */

    /* Format discrimination (graceful migration): peek the FIRST pair's value
     * type after consuming its key. MAP value (CBOR major 5) => NEW per-part
     * { partIdx => {id=>v} }; FLOAT value (major 7) => OLD flat { id => v }
     * (-> part 0). Any other value type is a malformed blob. */
    uint64_t first_key;
    if (!harp_cdec_uint(&dec, &first_key)) return false;
    int major = harp_cdec_peek(&dec);

    if (major == 5) { /* NEW per-part: first value is a map */
        /* first pair (key already consumed) */
        if (!parse_part_map(&dec, first_key < (uint64_t)NPARTS ? (int)first_key : -1,
                            v, present))
            return false;
        for (uint64_t i = 1; i < n; i++) {
            uint64_t pidx;
            if (!harp_cdec_uint(&dec, &pidx)) return false;
            if (!parse_part_map(&dec, pidx < (uint64_t)NPARTS ? (int)pidx : -1, v, present))
                return false;
        }
        return true;
    }

    if (major == 7) { /* OLD flat { paramId => value } -> part 0 */
        double val;
        if (!harp_cdec_float(&dec, &val)) return false;
        grid_set(0, first_key, val, v, present);
        for (uint64_t i = 1; i < n; i++) {
            uint64_t id;
            if (!harp_cdec_uint(&dec, &id) || !harp_cdec_float(&dec, &val)) return false;
            grid_set(0, id, val, v, present);
        }
        return true;
    }

    return false; /* first value neither map nor float: malformed */
}

/* Engine glue: snapshot the 16 parts into a float grid, run the pure encoder,
 * wrap in the PARAMS_MEDIA blob. Output is byte-identical to the prior inline
 * encoder (same grid order -> same CBOR). */
static void engine_encode_params_blob(harp_cbuf *out) {
    float v[NPARTS][NPARAMS];
    for (int pi = 0; pi < NPARTS; pi++)
        for (size_t i = 0; i < NPARAMS; i++)
            v[pi][i] = engine_part_param_get(pi, g_params[i].id);
    harp_cbuf payload;
    harp_cbuf_init(&payload);
    refdev_encode_params_blob(v, &payload);
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
 * or malformed — nothing is applied; §11.3 atomic apply). P3: stages ALL 16
 * parts, commits atomically (all-or-nothing, like before).
 *
 * P3 closer: the per-part blob parse now lives in the PURE
 * refdev_parse_params_blob codec above; the engine staging here only fetches
 * the blob, runs the codec, and overlays the params it reports PRESENT onto the
 * staged grid (which started from the CURRENT live values — see
 * engine_load_snapshot). Params/parts the blob does not carry keep their
 * current value, exactly as before. A malformed blob fails the whole load
 * (ctx->ok = false) so nothing is committed. */
struct load_ctx {
    device *d;
    float staged[NPARTS][NPARAMS];
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
    /* Run the pure codec into a scratch grid + presence mask, then overlay the
     * present values onto the staged grid (absent ones keep the current value
     * staged by the caller). A structural decode error fails the whole load. */
    float v[NPARTS][NPARAMS];
    bool present[NPARTS][NPARAMS] = {{false}};
    if (!refdev_parse_params_blob(payload, payload_len, v, present)) {
        harp_cbuf_free(&enc);
        ctx->ok = false;
        return false;
    }
    for (int pi = 0; pi < NPARTS; pi++)
        for (size_t i = 0; i < NPARAMS; i++)
            if (present[pi][i]) ctx->staged[pi][i] = v[pi][i];
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
    /* stage from CURRENT live values so a partial/old blob leaves untouched
     * parts and params exactly as they are (idempotent for absent entries) */
    struct load_ctx ctx;
    ctx.d = d;
    ctx.ok = true;
    for (int pi = 0; pi < NPARTS; pi++)
        for (size_t i = 0; i < NPARAMS; i++)
            ctx.staged[pi][i] = engine_part_param_get(pi, g_params[i].id);
    bool walked = harp_obj_tree_foreach(tree_enc.buf, tree_enc.len, load_tree_entry, &ctx);
    harp_cbuf_free(&tree_enc);
    if (!walked || !ctx.ok) goto fail;

    /* stage, then commit: all-or-nothing into the live engine — every part */
    for (int pi = 0; pi < NPARTS; pi++)
        for (size_t i = 0; i < NPARAMS; i++)
            engine_part_param_put(pi, g_params[i].id, ctx.staged[pi][i]);
    harp_cbuf_free(&enc);
    return 0;
fail:
    harp_cbuf_free(&enc);
    return -1;
}

/* §9.3 param FLAG bits (key 8). Bit 0 automatable; bit 1 readonly/output (the
 * meters); P2 adds bit 5 MODULATABLE and bit 6 PER-VOICE MODULATABLE — the
 * §9.4/§9.5 capability that a host needs to know a param accepts non-destructive
 * (and per-voice) modulation. */
#define PFLAG_AUTOMATABLE 0x01u
#define PFLAG_MODULATABLE 0x20u  /* bit 5: accepts §9.4 non-destructive mod */
#define PFLAG_PER_VOICE_MOD 0x40u /* bit 6: accepts §9.5 per-voice mod */

/* Which params advertise the P2 modulation capability. Filter Cutoff (id 3) is
 * the canonical per-voice modulation target (the kernel applies the per-voice
 * mod offset to it through voice_param); kept as a single predicate so the set
 * is one place to grow. */
static bool param_modulatable(const dev_param *p) { return p->id == 3; }

/* The §9.3 flag word for a param. `include_caps` gates the P2 capability bits:
 * the param-map-hash input (encode_param_array_automatable) passes false so the
 * hash sees the SAME bytes as pre-P2 firmware (a capability-add never
 * invalidates stored automation — 0.3.8 §9.3), keeping param-map-hash and
 * recall byte-identical; the advertised array passes true so hosts learn the
 * capability. */
static uint64_t param_flags(const dev_param *p, bool include_caps) {
    uint64_t f = PFLAG_AUTOMATABLE;
    if (include_caps && param_modulatable(p)) f |= PFLAG_MODULATABLE | PFLAG_PER_VOICE_MOD;
    return f;
}

/* Encode ONE automatable device-param descriptor (§9.3) into `b`. Shared by
 * the hash input (automatable subset, include_caps=false) and the full
 * advertised array (include_caps=true) so the two cannot drift in shape — the
 * 13 params are byte-identical in both EXCEPT the flag word (key 8), which the
 * hash path emits WITHOUT the P2 capability bits (see param_flags). */
static void encode_one_param(harp_cbuf *b, const dev_param *p, bool include_caps) {
    bool stepped = p->steps > 0;
    harp_cbor_map(b, stepped ? 5 : 3);
    harp_cbor_uint(b, 0);
    harp_cbor_uint(b, p->id);
    harp_cbor_uint(b, 1);
    harp_cbor_text(b, p->name);
    if (stepped) { /* §9.3 keys 5 + 9: step count, enum labels */
        harp_cbor_uint(b, 5);
        harp_cbor_uint(b, p->steps);
    }
    harp_cbor_uint(b, 8);
    harp_cbor_uint(b, param_flags(p, include_caps)); /* automatable [+ P2 mod caps] */
    if (stepped) {
        harp_cbor_uint(b, 9);
        harp_cbor_array(b, p->steps);
        for (int s = 0; s < p->steps; s++) harp_cbor_text(b, p->labels[s]);
    }
}

/* The AUTOMATABLE subset (§9.3): the 13 device params, in id order, EXACTLY as
 * before metering existed — byte-for-byte. This is the SOLE input to
 * param-map-hash (see compute_param_map_hash): the readonly meter params are
 * deliberately excluded so the hash is unchanged from pre-meter firmware. */
void encode_param_array_automatable(harp_cbuf *b) {
    harp_cbor_array(b, NPARAMS);
    /* include_caps=false: the P2 modulatable bits are MASKED OUT so this byte
     * stream — the SOLE param-map-hash input — is identical to pre-P2 firmware.
     * A capability-add must never invalidate stored automation (§9.3). */
    for (size_t i = 0; i < NPARAMS; i++) encode_one_param(b, &g_params[i], false);
}

/* The FULL advertised array (§9.3 + §9.9): the 13 automatable params FOLLOWED
 * BY the readonly output meters. Emitted on identity / evt.params so hosts can
 * present meters in their UI; NOT fed to param-map-hash. Each meter descriptor
 * is readonly (key 8 flag bit 1 = output) with a meter-rate hint (key 11), in
 * the collision-free id range METER_ID_BASE+ (slot*2 + {0 peak,1 rms}). */
void encode_param_array(harp_cbuf *b) {
    harp_cbor_array(b, NPARAMS + METER_NPARAMS);
    /* the automatable 13. include_caps=true: the advertised descriptor carries
     * the P2 modulatable bits (key 8) so hosts can target §9.4/§9.5 modulation;
     * this is the ONLY byte-level difference from encode_param_array_automatable,
     * and it lives outside the param-map-hash input by construction. */
    for (size_t i = 0; i < NPARAMS; i++) encode_one_param(b, &g_params[i], true);
    /* the readonly meters: per part 0..15 then the main mix, peak then rms */
    for (int slot = 0; slot < METER_NSLOTS; slot++) {
        char name[24];
        bool main = (slot == METER_MAIN_IX);
        for (int kind = 0; kind < 2; kind++) { /* 0 = peak, 1 = rms */
            const char *what = kind ? "RMS" : "Peak";
            if (main)
                snprintf(name, sizeof name, "Main %s", what);
            else
                snprintf(name, sizeof name, "Part %d %s", slot + 1, what);
            /* keys 0 (id), 1 (name), 8 (flags=readonly), 11 (meter rate Hz) */
            harp_cbor_map(b, 4);
            harp_cbor_uint(b, 0);
            harp_cbor_uint(b, kind ? METER_ID_RMS(slot) : METER_ID_PEAK(slot));
            harp_cbor_uint(b, 1);
            harp_cbor_text(b, name);
            harp_cbor_uint(b, 8);
            harp_cbor_uint(b, 0x2); /* flags: bit 1 = readonly (output), §9.9 */
            harp_cbor_uint(b, 11);
            harp_cbor_uint(b, METER_RATE_HZ); /* §9.3 key 11 meter-rate hint */
        }
    }
}

void compute_param_map_hash(device *d) {
    /* P3 lockstep guard: every part boots from PVAL_DEFAULTS (engine.c), which
     * MUST equal g_params[].def in id order. This runs once at boot (main),
     * BEFORE the factory snapshot, so part values are still the boot defaults —
     * assert they match so a future edit to one table but not the other is
     * caught immediately instead of as a silent recall/golden drift. */
    for (size_t i = 0; i < NPARAMS; i++)
        assert(engine_part_param_get(0, g_params[i].id) == g_params[i].def);
    harp_cbuf b;
    harp_cbuf_init(&b);
    /* §9.3/§9.9 DETERMINISM HINGE: hash the AUTOMATABLE subset ONLY. The hash
     * protects stored automation lanes ("change iff invalidates stored
     * automation"); readonly outputs are never automation targets, so adding /
     * removing meters does NOT invalidate any lane and MUST NOT move the hash.
     * Hashing the 13 automatable params keeps param-map-hash byte-identical to
     * pre-meter firmware -> identity + recall stay byte-identical. */
    encode_param_array_automatable(&b);
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
 * future GPIO encoders all come through here — set, dirty, echo. P3: the
 * front panel operates on PART 0 for now (echo carries channel 0). A full
 * per-part panel dimension — a part selector on the panel API — is a
 * follow-up; see panel.c. */
bool front_panel_set_part(device *d, int part, uint32_t id, double v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    if (part < 0 || part >= NPARTS) return false; /* no such part */
    if (param_index(id) < 0) return false;        /* no such param */
    engine_part_param_put(part, id, (float)v);
    live_ref_touch(d, true);                        /* any part's edit dirties live */
    evt_echo_param(d, id, (float)v, (uint8_t)part); /* echo on the part's channel */
    return true;
}

bool front_panel_set(device *d, uint32_t id, double v) {
    return front_panel_set_part(d, 0, id, v); /* back-compat: the panel's part 0 */
}

int do_snapshot(device *d, const char *msg, harp_hash *out, uint64_t *out_gen) {
    live_cache_flush(d);
    /* state_mu spans put+ref_write: the snapshot's objects must become reachable
     * (the ref written) before any GC can observe the store. GC also runs under
     * state_mu (maybe_gc), so this mutual exclusion stops a session-thread GC from
     * sweeping a panel-thread snapshot's freshly-put objects before its ref lands. */
    pthread_mutex_lock(&d->state_mu);
    harp_ref r;
    harp_hash snap;
    int rc = -1;
    if (harp_store_ref_read(&d->store, LIVE_REF, &r) == 0 &&
        engine_snapshot_objects(d, r.unborn ? NULL : &r.hash, msg, &snap) == 0) {
        r.unborn = false;
        r.hash = snap;
        r.generation++;
        r.dirty = false;
        if (harp_store_ref_write(&d->store, &r) == 0) rc = 0;
    }
    pthread_mutex_unlock(&d->state_mu);
    if (rc != 0) return -1;
    CTR_INC(d->snapshots_taken);
    ntf_state_changed(d, &r); /* notify + count OUTSIDE the store lock */
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

