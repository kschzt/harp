/* session.c — the protocol session (split from harp-deviced.c; see device.h).
 *
 * Wire helpers (all link writes serialize under send_mu), identity (§5),
 * every ctl/obj/evt handler, dispatch, and the per-connection session loop.
 * One session at a time; transports (TCP accept loop, FunctionFS) live in
 * harp-deviced.c and hand each connection to harp_deviced_run_session().
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/statvfs.h> /* free-space checks; Windows skips them (see handle_* below) */
#endif
#include <time.h>
#include <unistd.h>

#include "device.h"

#ifdef __linux__
extern _Atomic uint64_t g_usb_errors; /* §14.2: FunctionFS transport errors — device/ffs_link.c */
#endif

/* ---------------- wire helpers ---------------- */

int send_ctl(device *d, const harp_cbuf *msg) {
    pthread_mutex_lock(&d->send_mu);
    int rc = d->io ? harp_link_send(d->io, HARP_STREAM_CTL, msg->buf, msg->len) : -1;
    pthread_mutex_unlock(&d->send_mu);
    return rc;
}

/* Echo a base-value change as a param event on the evt stream (§9.4:
 * REQUIRED for front-panel and internally-driven changes; host-driven
 * param events are NOT echoed back). Timestamp (0,0) = "now" until the
 * timestamping slice lands. P3: `channel` is the multitimbral part — a
 * front-panel edit on part k echoes as part k. channel 0 OMITS the §9.4
 * channel key (key 5) so part-0 echoes are byte-identical to P2.2; non-zero
 * emits it (a 3-entry body map instead of 2). */
void evt_echo_param(device *d, uint32_t id, float v, uint8_t channel) {
    /* hello gate is atomic; the io check lives under send_mu below (the
     * panel thread echoes concurrently with session teardown) */
    if (!atomic_load_explicit(&d->hello_done, memory_order_acquire)) return;
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_cbor_array(&m, 3);
    harp_cbor_array(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 1); /* etype: param */
    harp_cbor_map(&m, channel ? 3 : 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, id);
    harp_cbor_uint(&m, 1);
    harp_cbor_float(&m, v);
    if (channel) { /* §9.4 key 5: part (only when non-zero — ascending key order) */
        harp_cbor_uint(&m, 5);
        harp_cbor_uint(&m, channel);
    }
    pthread_mutex_lock(&d->send_mu);
    if (d->io) harp_link_send(d->io, HARP_STREAM_EVT, m.buf, m.len);
    pthread_mutex_unlock(&d->send_mu);
    harp_cbuf_free(&m);
}

/* ---------------- §9.9 output-metering echo pump ----------------
 *
 * A dedicated CONTROL-PLANE thread (never the render thread) that streams the
 * readonly meter params as 'param' echoes while a stream is running. It paces
 * itself to METER_RATE_HZ (§9.9 "no more than the meter rate hint"), reads the
 * render thread's relaxed meter atomics, and emits via evt_echo_param — the
 * SAME path front-panel echoes use, so it inherits the hello gate + send_mu
 * serialization. It NEVER calls live_ref_touch, NEVER dirties state, and NEVER
 * touches engine render state: a meter is an output, not an edit (§9.9 — hosts
 * MUST NOT record these as automation; they are readonly).
 *
 * Coalescing (§9.9 / §10.3 spirit): a meter id is re-emitted only when its
 * value changed since the last tick, so a silent or steady part costs no wire
 * traffic. The first tick after start emits every id once (last[] seeded NaN)
 * so the host gets an initial reading. */
static void *meter_pump_thread(void *arg) {
    device *d = arg;
    audio_state *a = &d->audio;
    /* last value sent per meter id (peak,rms interleaved per slot). NaN forces
     * the first emission; thereafter we only send on change. */
    float last_peak[METER_NSLOTS], last_rms[METER_NSLOTS];
    for (int i = 0; i < METER_NSLOTS; i++)
        last_peak[i] = last_rms[i] = (float)NAN;

    const long tick_ns = 1000000000L / (long)METER_RATE_HZ;
    while (atomic_load_explicit(&a->meter_running, memory_order_relaxed)) {
        struct timespec ts = {tick_ns / 1000000000L, tick_ns % 1000000000L};
        nanosleep(&ts, NULL);
        if (!atomic_load_explicit(&a->meter_running, memory_order_relaxed)) break;
        for (int slot = 0; slot < METER_NSLOTS; slot++) {
            float peak = atomic_load_explicit(&g_meter_peak[slot], memory_order_relaxed);
            float rms = atomic_load_explicit(&g_meter_rms[slot], memory_order_relaxed);
            /* coalesce unchanged values (a steady reading costs no traffic).
             * The NaN-seeded first comparison always differs -> initial emit. */
            if (!(peak == last_peak[slot])) {
                evt_echo_param(d, METER_ID_PEAK(slot), peak, 0); /* device output:
                                                        channel 0 wire shape */
                last_peak[slot] = peak;
            }
            if (!(rms == last_rms[slot])) {
                evt_echo_param(d, METER_ID_RMS(slot), rms, 0);
                last_rms[slot] = rms;
            }
        }
    }
    return NULL;
}

void meter_pump_start(device *d) {
    audio_state *a = &d->audio;
    if (atomic_load_explicit(&a->meter_live, memory_order_relaxed)) return;
    engine_meters_reset(); /* new stream = new meter timeline; no stale peaks */
    atomic_store_explicit(&a->meter_running, true, memory_order_relaxed);
    if (pthread_create(&a->meter_thread, NULL, meter_pump_thread, d) != 0) {
        atomic_store_explicit(&a->meter_running, false, memory_order_relaxed);
        return; /* metering is best-effort telemetry; a failed pump never
                   blocks the audio stream from starting */
    }
    atomic_store_explicit(&a->meter_live, true, memory_order_relaxed);
}

void meter_pump_stop(device *d) {
    audio_state *a = &d->audio;
    if (!atomic_load_explicit(&a->meter_live, memory_order_relaxed)) return;
    atomic_store_explicit(&a->meter_running, false, memory_order_relaxed);
    pthread_join(a->meter_thread, NULL);
    atomic_store_explicit(&a->meter_live, false, memory_order_relaxed);
}

void ntf_state_changed(device *d, const harp_ref *r) {
    if (!atomic_load_explicit(&d->hello_done, memory_order_acquire)) return;
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

void grant_credit(device *d) {
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

void send_error(device *d, uint64_t rid, const char *method, const char *code,
                       const char *msg) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_error(&m, rid, method, code, msg);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* ---------------- identity (§6.2) ---------------- */

static void encode_identity(device *d, harp_cbuf *m) {
    /* P3: top-level identity keys 0..12 (13 pairs). The count was previously
     * 11 while 12 keys (0..11) were emitted — harmless only because the host
     * (host/client.c parse_identity) reads exactly `n` pairs and identity is
     * the last value in the hello body, so the under-counted tail was silently
     * ignored. P3 adds key 12 (part count) and corrects the count to 13 so the
     * map is well-formed CBOR and every advertised key is actually parseable.
     * Identity is not byte-constrained by the golden (which checks rendered
     * audio) nor by param-map-hash (computed from encode_param_array). */
    harp_cbor_map(m, 13);
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
    harp_cbor_array(m, d->no_rate_lock ? 17 : 18); /* P3: +evt.multitimbral; §9.9: +evt.param.meter;
                               +evt.param.mod; §8.7: +audio.rate-lock (dropped under
                               --no-rate-lock to force the host ASRC fallback);
                               §14.4: +diag.bundle; §14.3: +diag.loopback.digital */
    harp_cbor_text(m, "harp-core");
    harp_cbor_text(m, "harp-recall");
    harp_cbor_text(m, "harp-stream");
    harp_cbor_text(m, "harp-perf"); /* ±1-sample event timing (§9.2): proven
                                       by scripts/timing-test.sh + tempo-lock,
                                       which gate this claim in the hw suite */
    harp_cbor_text(m, "audio.host-paced");
    harp_cbor_text(m, "audio.deterministic");
    harp_cbor_text(m, "audio.offline-rate");
    if (!d->no_rate_lock)
        harp_cbor_text(m, "audio.rate-lock"); /* §8.7 bit-exact: device honors audio.trim to
                                             slave its free-running emit rate to the host's
                                             consumption, so the host plays 1:1 (bit-exact).
                                             Absent => the host must ASRC-resample instead. */
    harp_cbor_text(m, "evt.param");
    harp_cbor_text(m, "evt.param.echo");
    harp_cbor_text(m, "evt.param.mod"); /* §9.4 non-destructive modulation: device
                                           decodes mod events (etype 6) + the §9.5
                                           per-voice key; the per-voice mod layer
                                           that renders them is Phase 2 */
    harp_cbor_text(m, "evt.transport");
    harp_cbor_text(m, "evt.ump"); /* note input as UMP (§9.10); group map = key 11 */
    harp_cbor_text(m, "evt.multitimbral"); /* P3: 16 independent per-part timbres;
                                              params/notes/ramps route by §9.4 part
                                              (channel key 5). Part count = key 12 */
    harp_cbor_text(m, "evt.param.meter"); /* §9.9 output metering: readonly meter
                                             params (per-part + main peak/RMS)
                                             streamed via evt.param.echo at the
                                             descriptor's meter-rate hint */
    harp_cbor_text(m, "diag.bundle"); /* §14.4: rsp diag.bundle returns the device-section
                                         (identity + counters) the host embeds verbatim */
    harp_cbor_text(m, "diag.loopback.digital"); /* §14.3: pure-DSP H->D-in -> D->H-out echo
                                                   (no analog stage) for round-trip latency */
    harp_cbor_text(m, "x.harp-refdev.sim");
    harp_cbor_uint(m, 7); /* channel map (§6.3): 34 slots, all D→H. §P2.2
                             exposes per-part outputs: slots 0/1 are the stereo
                             main mix; slots 2+2k/3+2k are part k+1's stereo
                             output (k=0..15). The host requests a subset via
                             audio.start active-slots-out (key 4). */
    harp_cbor_array(m, 34);
    /* one stereo pair per group: pair 0 = "Mix"/"main", pairs 1..16 =
     * "Part N"/"partN". Each entry keeps the P2.1 shape (keys 0..5); built in
     * a loop so the 17 groups stay in lockstep. */
    for (int slot = 0; slot < 34; slot++) {
        int pair = slot / 2;      /* 0 = main mix, 1..16 = part 1..16 */
        bool right = (slot & 1);  /* even = L, odd = R */
        char name[16], path[16], group[16];
        if (pair == 0) {
            snprintf(name, sizeof name, "Main %c", right ? 'R' : 'L');
            snprintf(group, sizeof group, "Mix");
            snprintf(path, sizeof path, "main");
        } else {
            snprintf(name, sizeof name, "Part %d %c", pair, right ? 'R' : 'L');
            snprintf(group, sizeof group, "Part %d", pair);
            snprintf(path, sizeof path, "part%d", pair);
        }
        harp_cbor_map(m, 6);
        harp_cbor_uint(m, 0);
        harp_cbor_uint(m, slot); /* slot */
        harp_cbor_uint(m, 1);
        harp_cbor_uint(m, 0); /* direction: device -> host */
        harp_cbor_uint(m, 2);
        harp_cbor_text(m, name);
        harp_cbor_uint(m, 3);
        harp_cbor_text(m, group);
        harp_cbor_uint(m, 4);
        harp_cbor_text(m, path);
        harp_cbor_uint(m, 5);
        harp_cbor_bool(m, true); /* host-paced capable: pure-digital path */
    }
    harp_cbor_uint(m, 8); /* latency profile (§6.4): pure-digital engine */
    harp_cbor_array(m, 1);
    harp_cbor_map(m, 4);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 48000);
    harp_cbor_uint(m, 1);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 3);
    harp_cbor_uint(m, 256);
    harp_cbor_uint(m, 9); /* build id */
    harp_cbor_text(m, "refdev sim " __DATE__);
    harp_cbor_uint(m, 10); /* boot count */
    harp_cbor_uint(m, d->boot_count);
    harp_cbor_uint(m, 11); /* UMP group map (§9.10): which groups carry what.
                              refdev consumes notes on group 0 -> the mono
                              voice; that's the whole map. */
    harp_cbor_array(m, 1);
    harp_cbor_map(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 0); /* group index */
    harp_cbor_uint(m, 1);
    harp_cbor_text(m, "notes"); /* role */
    harp_cbor_uint(m, 12); /* P3 multitimbral part count: 16 independent timbres.
                              A new OPTIONAL identity key (mirrors the key-11
                              ump-group-map style); hosts that don't know it skip
                              it. Pairs with the "evt.multitimbral" capability. */
    harp_cbor_uint(m, NPARTS);
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
        /* §5.4: reply 'incompatible' WITH the device's supported major range
         * (machine-readable, details key 2 = {0 min-major, 1 max-major}) so the
         * host can surface a firmware/host-update prompt with specifics instead
         * of an opaque failure. */
        harp_cbuf m;
        harp_cbuf_init(&m);
        harp_env_head(&m, HARP_MSG_ERROR, e->rid, e->method, true);
        harp_cbor_map(&m, 3);
        harp_cbor_uint(&m, 0);
        harp_cbor_text(&m, "incompatible");
        harp_cbor_uint(&m, 1);
        harp_cbor_text(&m, "device supports protocol 1.x only");
        harp_cbor_uint(&m, 2);
        harp_cbor_map(&m, 2);
        harp_cbor_uint(&m, 0);
        harp_cbor_uint(&m, PROTO_MAJOR); /* min supported major */
        harp_cbor_uint(&m, 1);
        harp_cbor_uint(&m, PROTO_MAJOR); /* max supported major */
        send_ctl(d, &m);
        harp_cbuf_free(&m);
        return;
    }
    /* §5.4: hello resets all per-session state — including a running stream */
    audio_stop(d);
    atomic_store_explicit(&d->hello_done, true, memory_order_release);
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
    live_cache_flush(d);
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
                    harp_hash h;
                    if (!harp_hash_read(&b, &h)) break;
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
                    harp_hash h;
                    if (!harp_hash_read(&b, &h)) break;
                    if (nq < 256) {
                        queue[nq] = h;
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
#ifndef _WIN32
    struct statvfs vs;
    if (statvfs(d->store.dir, &vs) == 0) {
        uint64_t freeb = (uint64_t)vs.f_bavail * vs.f_frsize;
        if (total > freeb) { /* §11.5: refuse BEFORE transfer */
            send_error(d, e->rid, e->method, "storage", "insufficient free space");
            return;
        }
    }
#endif
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
                        } else if (harp_hash_read(&b, &expect)) {
                            have_expect = true;
                        }
                        break;
                    case 2:
                        if (harp_hash_read(&b, &newh)) have_new = true;
                        break;
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

    if (strcmp(refname, LIVE_REF) == 0) live_cache_flush(d);
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

/* §14.2 counters MAP (the 16-pair, text-keyed map), emitted into `m`. SHARED by
 * handle_diag_counters (as the rsp body) and handle_diag_bundle (device-section key
 * 1) so both emit BYTE-IDENTICAL counters — the §14.4 diag.bundle conformance gate
 * is the device-section round-tripping verbatim, which requires one emitter. */
static void emit_counters(device *d, harp_cbuf *m) {
    uint64_t total = 0, freeb = 0;
#ifndef _WIN32
    struct statvfs vs;
    if (statvfs(d->store.dir, &vs) == 0) {
        total = (uint64_t)vs.f_blocks * vs.f_frsize;
        freeb = (uint64_t)vs.f_bavail * vs.f_frsize;
    }
#endif
    harp_cbor_map(m, 16);
    harp_cbor_text(m, "x.harp-refdev.fence_waits");
    harp_cbor_uint(m, CTR_GET(g_fence_waits));
    harp_cbor_text(m, "x.harp-refdev.fence_timeouts");
    harp_cbor_uint(m, CTR_GET(g_fence_timeouts));
    harp_cbor_text(m, "usb_errors");
#ifdef __linux__
    harp_cbor_uint(m, CTR_GET(g_usb_errors)); /* §14.2: abnormal FunctionFS transport errors */
#else
    harp_cbor_uint(m, 0); /* no FunctionFS transport on this build (TCP/eth dev link) */
#endif
    harp_cbor_text(m, "frame_errors");
    harp_cbor_uint(m, CTR_GET(d->frame_errors));
    /* §14.2: 0 by construction on this reference device — the refdev synthesizes every block
     * on demand (free-run) or strictly per received pacing frame (host-paced), so it has no
     * input-fed buffer that can starve; the transport-stall side is counted as audio_overruns. */
    harp_cbor_text(m, "audio_underruns");
    harp_cbor_uint(m, 0);
    harp_cbor_text(m, "audio_overruns");
    harp_cbor_uint(m, CTR_GET(d->audio_overruns));
    /* §8.2 host-paced late-frame discard (frames whose ts is behind the render cursor) is
     * wired separately; 0 until then. */
    harp_cbor_text(m, "audio_late_frames");
    harp_cbor_uint(m, 0);
    /* §14.2: 0 by construction — the refdev's MSC is monotone (msc += nsamples each block);
     * a transport stall sets the DISCONT header bit + audio_overruns but never gaps the MSC,
     * so there is no unplanned MSC discontinuity to count. */
    harp_cbor_text(m, "msc_discontinuities");
    harp_cbor_uint(m, 0);
    /* §14.2: 0 on the device side — the refdev free-runs its master clock (it IS the clock
     * source, §8.3); drift is a host-side measurement between the DAW clock and this device's
     * MSC (§7.3), reported in the bundle's host section. Signed: a sensing device could report
     * negative drift. */
    harp_cbor_text(m, "clock_drift_ppb");
    harp_cbor_int(m, 0);
    harp_cbor_text(m, "evt_late");
    harp_cbor_uint(m, CTR_GET(g_evt_late));
    harp_cbor_text(m, "evt_stale_epoch");
    harp_cbor_uint(m, CTR_GET(d->evt_stale_epoch));
    harp_cbor_text(m, "x.harp-refdev.evq_drops");
    harp_cbor_uint(m, CTR_GET(g_evq_drops));
    harp_cbor_text(m, "x.harp-refdev.ramp_late");
    harp_cbor_uint(m, CTR_GET(g_ramp_late));
    harp_cbor_text(m, "session_resets");
    harp_cbor_uint(m, CTR_GET(d->session_resets));
    harp_cbor_text(m, "storage_bytes_total");
    harp_cbor_uint(m, total);
    harp_cbor_text(m, "storage_bytes_free");
    harp_cbor_uint(m, freeb);
}

static void handle_diag_counters(device *d, const harp_env *e) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    emit_counters(d, &m);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* diag.bundle (§14.4): the DEVICE-SECTION the host embeds VERBATIM at bundle key 4.
 * Integer-keyed map { 0 => identity (encode_identity — byte-identical to hello),
 *                     1 => counters (emit_counters — byte-identical to diag.counters) }.
 * device-section keys 2 (device logs) and 3 (audio-config) are OPTIONAL and omitted
 * for now: the refdev has no drainable §4.2 log ring yet, and audio-config is a later
 * enrichment. The byte-identical round-trip of THIS map is the device conformance gate
 * (host stores it verbatim under key 4 in the non-anonymized path). See
 * docs/diag-bundle-design.md. §16 anonymization stays HOST-side (host re-encodes). */
static void handle_diag_bundle(device *d, const harp_env *e) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2); /* device-section: 0 identity, 1 counters */
    harp_cbor_uint(&m, 0);
    encode_identity(d, &m);
    harp_cbor_uint(&m, 1);
    emit_counters(d, &m);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* diag.loopback.start (§14.3): arm the digital round-trip loop. Body {0=>in-slot (H->D
 * the device reads the host stimulus from), 1=>out-slot (D->H it writes the echo to),
 * 2=>"digital"|"analog", ?3=>rate}. The refdev does DIGITAL only (a pure same-segment
 * copy in host_paced_loop, gated on loopback_on so the golden path is untouched) and
 * requires a live host-paced stream. Replies {0 armed,1 mode,2 eff-in,3 eff-out,4 eff-rate,
 * 5 device-internal-loop-latency=0 (the host subtracts it in T11)}. See evq/engine copy +
 * docs/diag-bundle-design.md. */
static void handle_diag_loopback_start(device *d, const harp_env *e) {
    uint64_t in_slot = 0, out_slot = 0, rate_hint = 0;
    char mode[16] = {0};
    bool have_mode = false;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 0) {
                    if (!harp_cdec_uint(&b, &in_slot)) break;
                } else if (key == 1) {
                    if (!harp_cdec_uint(&b, &out_slot)) break;
                } else if (key == 2) {
                    const char *s = NULL;
                    size_t sl = 0;
                    if (!harp_cdec_text(&b, &s, &sl)) break;
                    size_t c = sl < sizeof mode - 1 ? sl : sizeof mode - 1;
                    if (s) memcpy(mode, s, c);
                    mode[c] = 0;
                    have_mode = true;
                } else if (key == 3) {
                    if (!harp_cdec_uint(&b, &rate_hint)) break;
                } else if (!harp_cdec_skip(&b))
                    break;
            }
        }
    }
    (void)rate_hint; /* the refdev loops at the live stream rate; the hint is informational */
    if (!have_mode || strcmp(mode, "digital") != 0) {
        send_error(d, e->rid, e->method, "unsupported", "refdev supports diag.loopback.digital only");
        return;
    }
    if (in_slot > 33 || out_slot > 33) {
        send_error(d, e->rid, e->method, "bad-slot", "in/out slot outside the channel map");
        return;
    }
    if (!atomic_load_explicit(&d->audio.thread_live, memory_order_relaxed) || d->audio.mode != 1) {
        send_error(d, e->rid, e->method, "state",
                   "diag.loopback.digital requires a live host-paced stream");
        return;
    }
    /* publish the slots BEFORE raising the flag — the audio thread reads them only after
     * it observes loopback_on (release/acquire pairs the two). */
    d->audio.loopback_in_slot = (uint8_t)in_slot;
    d->audio.loopback_out_slot = (uint8_t)out_slot;
    atomic_store_explicit(&d->audio.loopback_on, true, memory_order_release);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 6);
    harp_cbor_uint(&m, 0);
    harp_cbor_bool(&m, true);              /* armed */
    harp_cbor_uint(&m, 1);
    harp_cbor_text(&m, "digital");         /* mode actually engaged */
    harp_cbor_uint(&m, 2);
    harp_cbor_uint(&m, in_slot);           /* effective in-slot */
    harp_cbor_uint(&m, 3);
    harp_cbor_uint(&m, out_slot);          /* effective out-slot */
    harp_cbor_uint(&m, 4);
    harp_cbor_uint(&m, d->audio.rate);     /* effective rate */
    harp_cbor_uint(&m, 5);
    harp_cbor_uint(&m, 0);                 /* device-internal loop latency: 0 (same-segment copy) */
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* diag.loopback.stop (§14.3): disengage the loop, restore normal routing. No request
 * body. Replies {0 stopped} (the host's cross-correlation is the oracle; the refdev does
 * not self-measure, so device-rtt/frames-echoed are omitted). */
static void handle_diag_loopback_stop(device *d, const harp_env *e) {
    atomic_store_explicit(&d->audio.loopback_on, false, memory_order_release);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_bool(&m, true); /* stopped */
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

#ifdef __linux__
int harp_ffs_audio_in_fd(void);  /* device/ffs.c */
int harp_ffs_audio_out_fd(void); /* device/ffs.c */
#endif

/* audio.start (§8.2): free-running mode only, stereo D→H, USB transport only */
static void handle_audio_start(device *d, const harp_env *e) {
    uint64_t rate = 48000, nsamples = 256, mode = 0;
    uint64_t rtp_port = 0; /* §8.7 key 6: host's RTP audio port; 0 => USB transport */
    uint64_t prebuf = 0;   /* §8.7 key 2: RTP prefill-burst depth in frames (0 = none) */
    uint64_t hp_port = 0;  /* §8.3-over-§8.7 key 7: host's host-paced TCP audio port */
    /* active-slots-out (§6.3, key 4): the output slots the host wants, in
     * request order. Parsed into a local array first; an absent or empty key
     * defaults to the stereo main mix {0,1} (the P2.1 byte-identical path). */
    uint8_t out_slots[34];
    uint8_t n_out_slots = 0;
    uint8_t in_slots[34]; /* §14.3: active-slots-IN (key 3, H->D) — parsed for diag.loopback */
    uint8_t n_in_slots = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 0) {
                    if (!harp_cdec_uint(&b, &rate)) break;
                } else if (key == 1) {
                    if (!harp_cdec_uint(&b, &nsamples)) break;
                } else if (key == 3) {
                    /* §14.3 active-slots-IN (H->D): the engine has no audio input on the
                     * golden path, but a digital loopback (diag.loopback) reads one of
                     * these columns from the pacing-frame input payload and copies it to
                     * an output slot. Parsed (clamped 0..33, capped 34) so the loopback
                     * can resolve its in-slot's column; unused by the normal render. */
                    uint64_t alen;
                    if (!harp_cdec_array(&b, &alen)) break;
                    for (uint64_t j = 0; j < alen; j++) {
                        uint64_t slot;
                        if (!harp_cdec_uint(&b, &slot)) break;
                        if (slot > 33) slot = 33;
                        if (n_in_slots < 34) in_slots[n_in_slots++] = (uint8_t)slot;
                    }
                } else if (key == 4) {
                    /* CBOR array of uint slot indices: clamp each to 0..33,
                     * cap the count at 34. */
                    uint64_t alen;
                    if (!harp_cdec_array(&b, &alen)) break;
                    for (uint64_t j = 0; j < alen; j++) {
                        uint64_t slot;
                        if (!harp_cdec_uint(&b, &slot)) break;
                        if (slot > 33) slot = 33;
                        if (n_out_slots < 34) out_slots[n_out_slots++] = (uint8_t)slot;
                    }
                } else if (key == 5) {
                    if (!harp_cdec_uint(&b, &mode)) break;
                } else if (key == 6) { /* §8.7: RTP audio destination port (Ethernet) */
                    if (!harp_cdec_uint(&b, &rtp_port)) break;
                } else if (key == 2) { /* §8.7: RTP prefill-burst depth, frames */
                    if (!harp_cdec_uint(&b, &prebuf)) break;
                } else if (key == 7) { /* §8.3-over-§8.7: host-paced TCP audio port */
                    if (!harp_cdec_uint(&b, &hp_port)) break;
                } else if (!harp_cdec_skip(&b))
                    break;
            }
        }
    }
    /* absent or empty active-slots-out -> default stereo main mix {0,1} */
    if (n_out_slots == 0) {
        out_slots[0] = 0;
        out_slots[1] = 1;
        n_out_slots = 2;
    }
    if (mode > 1) {
        send_error(d, e->rid, e->method, "unsupported", "unknown clock mode");
        return;
    }
    if (rate != 44100 && rate != 48000 && rate != 96000) {
        send_error(d, e->rid, e->method, "unsupported", "rate");
        return;
    }
    if (nsamples < 32 || nsamples > AUDIO_MAX_NSAMPLES) {
        send_error(d, e->rid, e->method, "unsupported", "nsamples");
        return;
    }
    /* §8.7 Ethernet/RTP: a host-negotiated audio port (key 6) selects the RTP
     * transport — free-running only, no USB endpoints. Absent key 6 (every USB
     * audio.start) this whole branch is skipped and the path stays byte-identical
     * to before, so the golden render is unaffected. */
    if (rtp_port && mode != 0) {
        send_error(d, e->rid, e->method, "unsupported", "RTP audio is free-running only");
        return;
    }
    /* §8.3-over-§8.7: a host-paced TCP audio port (key 7) is the deterministic
     * offline-bounce channel — mode MUST be host-paced (1), and it is mutually
     * exclusive with RTP (key 6 is the free-running plane). */
    if (hp_port && (mode != 1 || rtp_port)) {
        send_error(d, e->rid, e->method, "unsupported",
                   "host-paced TCP audio requires clock mode 1 and no RTP port");
        return;
    }
    int fd = -1, out_fd = -1;
#ifdef __linux__
    if (!rtp_port && !hp_port) { /* USB transport: the FFS audio endpoints */
        fd = harp_ffs_audio_in_fd();
        out_fd = harp_ffs_audio_out_fd();
    }
#endif
    /* host-paced TCP (hp_port) sets fd/out_fd on the audio thread after the
     * connect-back, so it bypasses the USB-endpoint requirement too. */
    if (!rtp_port && !hp_port && (fd < 0 || (mode == 1 && out_fd < 0))) {
        send_error(d, e->rid, e->method, "unsupported",
                   "HARP stream requires the USB transport");
        return;
    }
    if (atomic_load_explicit(&d->audio.thread_live, memory_order_relaxed)) {
        send_error(d, e->rid, e->method, "busy", "stream already running");
        return;
    }
    /* Open the RTP dest AFTER the busy gate so a rejected start leaks no socket.
     * The peer IP was captured at accept; key 6 is the host's chosen port. */
    int rtp_fd = -1;
    if (rtp_port) {
        rtp_fd = audio_open_rtp_dest(d->rtp_peer_ip, (int)rtp_port);
        if (rtp_fd < 0) {
            send_error(d, e->rid, e->method, "unsupported", "cannot open RTP destination");
            return;
        }
    }
    evq_reset_for_new_stream();
    d->audio.fd = fd;
    d->audio.out_fd = out_fd;
    d->audio.rtp_fd = rtp_fd;   /* >=0 only for RTP; USB stays <0 => emit is a no-op */
    /* §8.3-over-§8.7: the audio thread connect()s back and points fd+out_fd at
     * this socket when host_paced_port > 0 (off the session thread — see device.h).
     * 0 for USB/RTP, so audio_thread takes the unchanged path there. */
    d->audio.host_paced_port = (int)hp_port;
    d->audio.host_paced_sock = -1;
    if (rtp_fd >= 0) {          /* fresh RTP stream identity */
        d->audio.rtp_ssrc = 0x48415250u; /* "HARP" */
        d->audio.rtp_seq = 0;
    }
    d->audio.rtp_prebuffer = (rtp_fd >= 0) ? (uint32_t)prebuf : 0; /* startup burst (RTP only) */
    /* §7.1/§8.6: a FREE-RUNNING sample-rate change opens a new clock epoch. Bump it and
     * announce (ntf time.epoch) BEFORE the audio thread spawns, so every frame of the new
     * stream carries the new epoch. Host-paced mode has no device-clock change, so its
     * rate change does NOT bump the epoch (§8.6). On a spawn failure below the bump+rate
     * are rolled back (a retry then re-detects). The host stopped the old stream first
     * (busy gate above), so the audio thread already published its final MSC (msc_final). */
    uint32_t old_rate = d->audio.rate, old_epoch = d->audio.epoch;
    bool new_epoch = (mode == 0 && old_rate != 0 && (uint32_t)rate != old_rate);
    if (new_epoch) {
        d->audio.epoch++;
        harp_cbuf ev;
        harp_cbuf_init(&ev);
        harp_env_head(&ev, HARP_MSG_NOTIFICATION, 0, "time.epoch", true);
        harp_cbor_map(&ev, 4);
        harp_cbor_uint(&ev, 0);
        harp_cbor_uint(&ev, d->audio.epoch);     /* new-epoch */
        harp_cbor_uint(&ev, 1);
        harp_cbor_uint(&ev, rate);               /* new-rate-hz */
        harp_cbor_uint(&ev, 2);
        harp_cbor_uint(&ev, old_epoch);          /* old-epoch */
        harp_cbor_uint(&ev, 3);
        harp_cbor_uint(&ev, d->audio.msc_final); /* old-msc-final */
        send_ctl(d, &ev);
        harp_cbuf_free(&ev);
    }
    d->audio.mode = (uint32_t)mode;
    d->audio.rate = (uint32_t)rate;
    d->audio.nsamples = (uint32_t)nsamples;
    d->audio.reanchors = 0;
    /* publish the requested output slots for the render thread (§P2.2) */
    memcpy(d->audio.out_slots, out_slots, n_out_slots);
    d->audio.n_out_slots = n_out_slots;
    /* §14.3: publish the input slots; a fresh stream starts with loopback OFF (a prior
     * diag.loopback does not survive a stop/start — set BEFORE the audio thread spawns). */
    memcpy(d->audio.in_slots, in_slots, n_in_slots);
    d->audio.n_in_slots = n_in_slots;
    atomic_store_explicit(&d->audio.loopback_on, false, memory_order_relaxed);
    atomic_store_explicit(&d->audio.running, true, memory_order_relaxed);
    if (pthread_create(&d->audio.thread, NULL, audio_thread, d) != 0) {
        atomic_store_explicit(&d->audio.running, false, memory_order_relaxed);
        audio_rtp_close(&d->audio); /* don't leak the RTP socket on a failed start */
        if (new_epoch) { d->audio.epoch = old_epoch; d->audio.rate = old_rate; } /* roll back: no stream started */
        send_error(d, e->rid, e->method, "internal", "thread");
        return;
    }
    atomic_store_explicit(&d->audio.thread_live, true, memory_order_relaxed);
    meter_pump_start(d); /* §9.9: stream the readonly meters while streaming */
    fprintf(stderr, "harp-deviced: audio stream started (%u Hz, %u-sample blocks, %s%s)\n",
            d->audio.rate, d->audio.nsamples, mode ? "host-paced" : "free-running",
            d->audio.rtp_fd >= 0 ? ", RTP/UDP" : "");

    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, mode); /* clock-mode in effect */
    harp_cbor_uint(&m, 1);
    /* device pipeline: one block free-running; zero host-paced (pure render) */
    harp_cbor_uint(&m, mode ? 0 : d->audio.nsamples);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_audio_stop(device *d, const harp_env *e) {
    audio_stop(d);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* audio.trim (§8.7 bit-exact): the host's rate correction, key 0 = ppb (float,
 * signed). Stored for the free-running render loop to apply to its emit period.
 * Fire-and-forget — no response (the host streams these ~20/s and never waits),
 * so it never head-of-line-blocks the control plane. */
static void handle_audio_trim(device *d, const harp_env *e) {
    if (!e->has_body) return;
    harp_cdec b;
    harp_cdec_init(&b, e->body, e->body_len);
    uint64_t n, key;
    double v = 0;
    if (!harp_cdec_map(&b, &n)) return;
    for (uint64_t i = 0; i < n; i++) {
        if (!harp_cdec_uint(&b, &key)) break;
        if (key == 0) {
            if (!harp_cdec_float(&b, &v)) break;
        } else if (!harp_cdec_skip(&b))
            break;
    }
    if (v > 200000.0) v = 200000.0; /* clamp to +/-200 ppm */
    else if (v < -200000.0) v = -200000.0;
    atomic_store_explicit(&d->audio.rate_trim_ppb, (int)v, memory_order_relaxed);
}

/* evt.params (§9.3): the descriptor set */
static void handle_evt_params(device *d, const harp_env *e) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 4);
    harp_cbor_uint(&m, 0);
    harp_cbor_bytes(&m, d->param_map_hash.b, HARP_HASH_LEN);
    harp_cbor_uint(&m, 1);
    encode_param_array(&m);
    harp_cbor_uint(&m, 2);
    harp_cbor_uint(&m, 1000); /* control rate, Hz */
    harp_cbor_uint(&m, 3);
    harp_cbor_uint(&m, 4000); /* max sustained events/s */
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* evt stream (§9.2): timestamped event messages. Slice 1: etype 1 (param
 * set) applied at "now"; other types skipped. Events have no responses.
 * Host-driven sets do NOT echo (§9.4). */
static void handle_evt_msg(device *d, const uint8_t *buf, size_t len) {
    harp_cdec dec;
    harp_cdec_init(&dec, buf, len);
    uint64_t alen, tn, ep, msc, etype;
    if (!harp_cdec_array(&dec, &alen) || alen < 3 || !harp_cdec_array(&dec, &tn) ||
        tn != 2 || !harp_cdec_uint(&dec, &ep) || !harp_cdec_uint(&dec, &msc) ||
        !harp_cdec_uint(&dec, &etype)) {
        CTR_INC(d->frame_errors);
        return;
    }
    /* §7.1: an event timestamped in a stale (older) epoch is discarded + counted.
     * epoch 0 = "now" (always current); a future epoch shouldn't arrive but is not stale. */
    if (ep != 0 && ep < d->audio.epoch) {
        CTR_INC(d->evt_stale_epoch);
        return;
    }
    if (etype == 0) { /* UMP carriage (§9.10): body = one packet, words big-endian */
        const uint8_t *p;
        size_t pl;
        if (!harp_cdec_bytes(&dec, &p, &pl) || pl < 4) return;
        uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
                     p[3];
        uint32_t mt = w >> 28;
        if (mt == 2) { /* MIDI 1.0 channel voice in UMP */
            uint32_t status = (w >> 20) & 0xf, note = (w >> 8) & 0x7f, vel = w & 0x7f;
            dev_event ev = {msc, 0, note, 0, 0, 0,
                            (uint8_t)((w >> 16) & 0xf), 0}; /* channel = part (§15.2); voice 0 */
            if (status == 0x9 && vel > 0) {
                ev.kind = DEV_EV_NOTE_ON;
                ev.v = (float)vel / 127.0f;
            } else if (status == 0x8 || (status == 0x9 && vel == 0)) {
                ev.kind = DEV_EV_NOTE_OFF;
            } else if (status == 0xB && (note == 120 || note == 123)) {
                /* CC 120 all-sound-off / 123 all-notes-off: PANIC. Applied
                 * immediately AND queued — belt and suspenders. */
                ev.kind = DEV_EV_ALL_OFF;
                engine_all_notes_off();
            } else
                return;
            /* note-offs must never be lost: if the queue is full, apply NOW
             * (slightly early beats stuck forever) */
            if (ev.kind != DEV_EV_NOTE_ON && evq_full()) {
                if (ev.kind == DEV_EV_ALL_OFF)
                    engine_all_notes_off();
                else
                    engine_note_off_if(note);
                return;
            }
            evq_push(ev); /* ts 0 = asap; else applied at the exact sample (§9.2) */
        }
        return;
    }
    if (etype == 5) { /* ramp (§9.4): {0 param, 1 target, 2 end tstamp} */
        uint64_t n, key, id = 0, eep = 0, ets = 0, channel = 0, voice = 0;
        double target = 0;
        bool have_id = false, have_t = false, have_end = false;
        if (!harp_cdec_map(&dec, &n)) {
            CTR_INC(d->frame_errors);
            return;
        }
        for (uint64_t i = 0; i < n; i++) {
            if (!harp_cdec_uint(&dec, &key)) return;
            if (key == 0) {
                if (!harp_cdec_uint(&dec, &id)) return;
                have_id = true;
            } else if (key == 1) {
                if (!harp_cdec_float(&dec, &target)) return;
                have_t = true;
            } else if (key == 2) {
                uint64_t tn;
                if (!harp_cdec_array(&dec, &tn) || tn != 2 || !harp_cdec_uint(&dec, &eep) ||
                    !harp_cdec_uint(&dec, &ets))
                    return;
                have_end = true;
            } else if (key == 3) { /* §9.5 per-voice (decoded; applied in Phase 2) */
                if (!harp_cdec_uint(&dec, &voice)) return;
            } else if (key == 5) { /* multitimbral part (§9.4): 0..15, default 0 */
                if (!harp_cdec_uint(&dec, &channel)) return;
            } else if (!harp_cdec_skip(&dec))
                return;
        }
        if (!have_id || !have_t || !have_end) return;
        if (target < 0) target = 0;
        if (target > 1) target = 1;
        dev_event ev = {msc, DEV_EV_RAMP, (uint32_t)id, (float)target, ets, 0,
                        (uint8_t)(channel & 0xf), (uint32_t)voice};
        evq_push(ev);
        live_ref_touch(d, true);
        return;
    }
    if (etype == 7) { /* transport (§9.7): the (ts, ppq, tempo) anchor */
        uint64_t n, key, flags = 0;
        double tempo = 0, ppq = 0;
        if (!harp_cdec_map(&dec, &n)) {
            CTR_INC(d->frame_errors);
            return;
        }
        for (uint64_t i = 0; i < n; i++) {
            if (!harp_cdec_uint(&dec, &key)) return;
            if (key == 0) {
                if (!harp_cdec_uint(&dec, &flags)) return;
            } else if (key == 1) {
                if (!harp_cdec_float(&dec, &tempo)) return;
            } else if (key == 4) {
                if (!harp_cdec_float(&dec, &ppq)) return;
            } else if (!harp_cdec_skip(&dec))
                return;
        }
        dev_event ev = {msc, DEV_EV_TRANSPORT, (uint32_t)flags, (float)tempo, 0, ppq, 0, 0};
        evq_push(ev);
        return;
    }
    if (etype == 6) { /* mod (§9.4): {0 param, 1 signed offset, ?3 voice, ?4 txn,
                       * ?5 channel}. NON-DESTRUCTIVE — decoded here; the per-voice
                       * mod layer that APPLIES it is Phase 2, so the engine ignores
                       * DEV_EV_MOD for now (no render effect → golden byte-identical).
                       * MUST NOT alter the base value / dirty state (§9.4): no
                       * live_ref_touch, and the offset is NOT clamped to [0,1] (it is
                       * signed, clamped only after summation onto the base). */
        uint64_t n, key, id = 0, channel = 0, voice = 0;
        double offset = 0;
        bool have_id = false, have_o = false;
        if (!harp_cdec_map(&dec, &n)) { CTR_INC(d->frame_errors); return; }
        for (uint64_t i = 0; i < n; i++) {
            if (!harp_cdec_uint(&dec, &key)) return;
            if (key == 0) { if (!harp_cdec_uint(&dec, &id)) return; have_id = true; }
            else if (key == 1) { if (!harp_cdec_float(&dec, &offset)) return; have_o = true; }
            else if (key == 3) { if (!harp_cdec_uint(&dec, &voice)) return; }
            else if (key == 5) { if (!harp_cdec_uint(&dec, &channel)) return; }
            else if (!harp_cdec_skip(&dec)) return;
        }
        if (!have_id || !have_o) return;
        dev_event ev = {msc, DEV_EV_MOD, (uint32_t)id, (float)offset, 0, 0,
                        (uint8_t)(channel & 0xf), (uint32_t)voice};
        evq_push(ev);
        return;
    }
    if (etype != 1) return; /* unknown event types are skipped, not fatal */
    uint64_t n, key, id = 0;
    double v = 0;
    bool have_id = false, have_v = false;
    if (!harp_cdec_map(&dec, &n)) {
        CTR_INC(d->frame_errors);
        return;
    }
    uint64_t channel = 0, voice = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (!harp_cdec_uint(&dec, &key)) return;
        if (key == 0) {
            if (!harp_cdec_uint(&dec, &id)) return;
            have_id = true;
        } else if (key == 1) {
            if (!harp_cdec_float(&dec, &v)) return;
            have_v = true;
        } else if (key == 3) { /* §9.5 per-voice (decoded; applied in Phase 2) */
            if (!harp_cdec_uint(&dec, &voice)) return;
        } else if (key == 5) { /* multitimbral part (§9.4): 0..15, default 0 */
            if (!harp_cdec_uint(&dec, &channel)) return;
        } else if (!harp_cdec_skip(&dec))
            return;
    }
    if (!have_id || !have_v) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    /* ts 0 = "now" = next render subblock; routing it through the queue
     * (instead of the old direct write) keeps ramp state render-owned —
     * the direct path raced ramps_advance on g_ramps[i].active */
    dev_event ev = {msc, DEV_EV_PARAM_SET, (uint32_t)id, (float)v, 0, 0,
                    (uint8_t)(channel & 0xf), (uint32_t)voice};
    evq_push(ev);
    live_ref_touch(d, true);
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
    if (!front_panel_set(d, (uint32_t)id, v)) {
        send_error(d, e->rid, e->method, "not-found", "no such param");
        return;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* x.harp.reconcile.offer {0: expect (short-hex text), 1: live (short-hex text),
 * 2: dirty (optional)} — a shell whose saved Recall Bundle differs from the device's
 * live ref POSTs the §11.4 conflict; the panel frontend force-opens its Reconcile
 * screen and the user's pick returns via .poll. The device only relays for display:
 * the shell owns the hash comparison and the chosen action. */
static void handle_reconcile_offer(device *d, const harp_env *e) {
    char expect[16] = "", live[16] = "";
    uint64_t dirty = 0;
    bool ok = false;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n, key;
        const char *s;
        size_t sl;
        if (harp_cdec_map(&b, &n) && n >= 2 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_text(&b, &s, &sl)) {
            snprintf(expect, sizeof expect, "%.*s", (int)(sl < 15 ? sl : 15), s);
            if (harp_cdec_uint(&b, &key) && key == 1 && harp_cdec_text(&b, &s, &sl)) {
                snprintf(live, sizeof live, "%.*s", (int)(sl < 15 ? sl : 15), s);
                ok = true;
                if (n >= 3 && harp_cdec_uint(&b, &key) && key == 2)
                    harp_cdec_uint(&b, &dirty); /* optional */
            }
        }
    }
    if (!ok) {
        send_error(d, e->rid, e->method, "malformed", NULL);
        return;
    }
    reconcile_post_offer(expect, live, (int)dirty);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* x.harp.reconcile.poll {} -> {0: pending, 1: choice (-1 none, 0..3)} — the shell
 * polls for the front-panel pick, then executes the §11.4 action. */
static void handle_reconcile_poll(device *d, const harp_env *e) {
    int pending = 0, choice = -1;
    reconcile_read(&pending, NULL, NULL, NULL, &choice);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_bool(&m, pending != 0);
    harp_cbor_uint(&m, 1);
    harp_cbor_int(&m, choice);
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
        /* P3: this vendor read reports PART 0's values (the web panel /
         * dev tooling dimension is part 0 for now — see panel.c). */
        harp_cbor_float(&m, engine_part_param_get(0, g_params[i].id));
    }
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* x.harp-refdev.meters -> {0: [ [slot, peak, rms], ... ]} — a control-plane READ
 * of the live §9.9 output meters (the same _Atomic floats the render thread folds
 * and the 30 Hz pump echoes). slot 0..15 = parts, 16 = main mix. Pure read, no
 * render/state effect (golden unaffected) — a diag + test convenience alongside
 * the asynchronous meter echo. */
static void handle_dev_meters(device *d, const harp_env *e) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, METER_NSLOTS);
    for (int s = 0; s < METER_NSLOTS; s++) {
        harp_cbor_array(&m, 3);
        harp_cbor_uint(&m, (uint64_t)s);
        harp_cbor_float(&m, atomic_load_explicit(&g_meter_peak[s], memory_order_relaxed));
        harp_cbor_float(&m, atomic_load_explicit(&g_meter_rms[s], memory_order_relaxed));
    }
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* ---------------- dispatch ---------------- */

static void handle_ctl(device *d, const uint8_t *buf, size_t len) {
    /* dirty-flag work the render thread deferred (queued sets/ramps applied) */
    if (atomic_exchange_explicit(&g_touch_pending, 0, memory_order_acquire)) {
        live_ref_touch(d, true);
    }
    harp_env e;
    if (!harp_env_parse(buf, len, &e)) {
        CTR_INC(d->frame_errors);
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

    if (!atomic_load_explicit(&d->hello_done, memory_order_acquire) &&
        strcmp(e.method, "core.hello") != 0) {
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
    } else if (strcmp(e.method, "time.ping") == 0) {
        /* §7.2 host-device time correlation: {0 => (epoch,msc) at receipt,
         * 1 => (epoch,msc) at transmit}. msc is the device's monotonic
         * microsecond clock (the refdev has no analog sample clock in
         * host-paced mode; free-running MSC rides the stream header, §8.2).
         * The host brackets this with its own send/recv stamps and solves
         * the offset NTP-style; transmit-receipt = device turnaround. */
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t recv_us = (uint64_t)t0.tv_sec * 1000000 + (uint64_t)t0.tv_nsec / 1000;
        uint32_t epoch = d->audio.epoch;
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, true);
        harp_cbor_map(&m, 2);
        harp_cbor_uint(&m, 0);
        harp_cbor_array(&m, 2);
        harp_cbor_uint(&m, epoch);
        harp_cbor_uint(&m, recv_us);
        harp_cbor_uint(&m, 1);
        harp_cbor_array(&m, 2);
        harp_cbor_uint(&m, epoch);
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        harp_cbor_uint(&m, (uint64_t)t1.tv_sec * 1000000 + (uint64_t)t1.tv_nsec / 1000);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
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
    else if (strcmp(e.method, "evt.params") == 0)
        handle_evt_params(d, &e);
    else if (strcmp(e.method, "audio.start") == 0)
        handle_audio_start(d, &e);
    else if (strcmp(e.method, "audio.stop") == 0)
        handle_audio_stop(d, &e);
    else if (strcmp(e.method, "audio.trim") == 0)
        handle_audio_trim(d, &e);
    else if (strcmp(e.method, "diag.counters") == 0)
        handle_diag_counters(d, &e);
    else if (strcmp(e.method, "diag.bundle") == 0)
        handle_diag_bundle(d, &e); /* §14.4: device-section embedded verbatim host-side */
    else if (strcmp(e.method, "diag.loopback.start") == 0)
        handle_diag_loopback_start(d, &e); /* §14.3 round-trip: arm the digital loop */
    else if (strcmp(e.method, "diag.loopback.stop") == 0)
        handle_diag_loopback_stop(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.knob") == 0)
        handle_knob(d, &e);
    else if (strcmp(e.method, "x.harp.reconcile.offer") == 0)
        handle_reconcile_offer(d, &e);
    else if (strcmp(e.method, "x.harp.reconcile.poll") == 0)
        handle_reconcile_poll(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.params") == 0)
        handle_dev_params(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.meters") == 0)
        handle_dev_meters(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.restart") == 0) {
        /* dev-loop helper: exit cleanly so systemd (Restart=always) respawns
         * the daemon from the (possibly updated) binary on disk — the
         * fw.commit pattern in miniature, no root needed for deploys */
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, false);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
        fprintf(stderr, "harp-deviced: restart requested; exiting for respawn\n");
        audio_stop(d);
        exit(0);
    } else
        send_error(d, e.rid, e.method, "unsupported", NULL);
}

static void handle_obj(device *d, const uint8_t *buf, size_t len) {
    /* one object per obj-stream message; verify-on-receipt is intrinsic */
    if (harp_store_put(&d->store, buf, len, NULL) != 0) CTR_INC(d->frame_errors);
    if (d->granted >= len)
        d->granted -= len;
    else
        d->granted = 0;
    if (d->granted < CREDIT_GRANT / 2) grant_credit(d);
}

/* ---------------- session / main ---------------- */

void harp_deviced_run_session(device *d, harp_io *io) {
    pthread_mutex_lock(&d->send_mu);
    d->io = io;
    pthread_mutex_unlock(&d->send_mu);
    atomic_store_explicit(&d->hello_done, false, memory_order_release);
    d->closing = false;
    harp_link_init(&d->link);
    harp_cbuf msg;
    harp_cbuf_init(&msg);
    for (;;) {
        uint8_t stream;
        int rc = harp_link_recv(io, &d->link, &stream, &msg);
        if (rc == -1) break; /* peer gone */
        if (rc == -2) {      /* protocol violation: session reset (§12.4) */
            CTR_INC(d->session_resets);
            break;
        }
        if (stream == HARP_STREAM_CTL)
            handle_ctl(d, msg.buf, msg.len);
        else if (stream == HARP_STREAM_OBJ)
            handle_obj(d, msg.buf, msg.len);
        else if (stream == HARP_STREAM_EVT) {
            handle_evt_msg(d, msg.buf, msg.len);
            /* fence bookkeeping (§8.3.1): every evt message counts once it
             * is FULLY processed (events visible in evq) — including ones
             * handle_evt_msg rejected, or a malformed message would leave
             * the host's sequence unreachable and wedge every later fence */
            atomic_fetch_add_explicit(&g_evt_consumed, 1, memory_order_release);
        }
        if (d->closing) break;
    }
    harp_cbuf_free(&msg);
    harp_link_free(&d->link);
    audio_stop(d); /* session gone -> stream gone (§12) */
    live_cache_flush(d); /* persist the terminal generation (§10.3) */
    pthread_mutex_lock(&d->send_mu);
    d->io = NULL;
    pthread_mutex_unlock(&d->send_mu);
}

