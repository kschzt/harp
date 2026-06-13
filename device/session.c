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
#include <sys/statvfs.h>
#include <unistd.h>

#include "device.h"

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
 * timestamping slice lands. */
void evt_echo_param(device *d, uint32_t id, float v) {
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
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, id);
    harp_cbor_uint(&m, 1);
    harp_cbor_float(&m, v);
    pthread_mutex_lock(&d->send_mu);
    if (d->io) harp_link_send(d->io, HARP_STREAM_EVT, m.buf, m.len);
    pthread_mutex_unlock(&d->send_mu);
    harp_cbuf_free(&m);
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
    harp_cbor_array(m, 12);
    harp_cbor_text(m, "harp-core");
    harp_cbor_text(m, "harp-recall");
    harp_cbor_text(m, "harp-stream");
    harp_cbor_text(m, "harp-perf"); /* ±1-sample event timing (§9.2): proven
                                       by scripts/timing-test.sh + tempo-lock,
                                       which gate this claim in the hw suite */
    harp_cbor_text(m, "audio.host-paced");
    harp_cbor_text(m, "audio.deterministic");
    harp_cbor_text(m, "audio.offline-rate");
    harp_cbor_text(m, "evt.param");
    harp_cbor_text(m, "evt.param.echo");
    harp_cbor_text(m, "evt.transport");
    harp_cbor_text(m, "evt.ump"); /* note input as UMP (§9.10); group map = key 11 */
    harp_cbor_text(m, "x.harp-refdev.sim");
    harp_cbor_uint(m, 7); /* channel map (§6.3): stereo main mix, D→H */
    harp_cbor_array(m, 2);
    for (int ch = 0; ch < 2; ch++) {
        harp_cbor_map(m, 6);
        harp_cbor_uint(m, 0);
        harp_cbor_uint(m, ch); /* slot */
        harp_cbor_uint(m, 1);
        harp_cbor_uint(m, 0); /* direction: device -> host */
        harp_cbor_uint(m, 2);
        harp_cbor_text(m, ch ? "Main R" : "Main L");
        harp_cbor_uint(m, 3);
        harp_cbor_text(m, "Mix");
        harp_cbor_uint(m, 4);
        harp_cbor_text(m, "main");
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
    harp_cbor_map(&m, 16);
    harp_cbor_text(&m, "x.harp-refdev.fence_waits");
    harp_cbor_uint(&m, CTR_GET(g_fence_waits));
    harp_cbor_text(&m, "x.harp-refdev.fence_timeouts");
    harp_cbor_uint(&m, CTR_GET(g_fence_timeouts));
    harp_cbor_text(&m, "usb_errors");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "frame_errors");
    harp_cbor_uint(&m, CTR_GET(d->frame_errors));
    harp_cbor_text(&m, "audio_underruns");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "audio_overruns");
    harp_cbor_uint(&m, CTR_GET(d->audio_overruns));
    harp_cbor_text(&m, "audio_late_frames");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "msc_discontinuities");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "clock_drift_ppb");
    harp_cbor_int(&m, 0);
    harp_cbor_text(&m, "evt_late");
    harp_cbor_uint(&m, CTR_GET(g_evt_late));
    harp_cbor_text(&m, "evt_stale_epoch");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "x.harp-refdev.evq_drops");
    harp_cbor_uint(&m, CTR_GET(g_evq_drops));
    harp_cbor_text(&m, "x.harp-refdev.ramp_late");
    harp_cbor_uint(&m, CTR_GET(g_ramp_late));
    harp_cbor_text(&m, "session_resets");
    harp_cbor_uint(&m, CTR_GET(d->session_resets));
    harp_cbor_text(&m, "storage_bytes_total");
    harp_cbor_uint(&m, total);
    harp_cbor_text(&m, "storage_bytes_free");
    harp_cbor_uint(&m, freeb);
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
                } else if (key == 5) {
                    if (!harp_cdec_uint(&b, &mode)) break;
                } else if (!harp_cdec_skip(&b))
                    break;
            }
        }
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
    int fd = -1, out_fd = -1;
#ifdef __linux__
    fd = harp_ffs_audio_in_fd();
    out_fd = harp_ffs_audio_out_fd();
#endif
    if (fd < 0 || (mode == 1 && out_fd < 0)) {
        send_error(d, e->rid, e->method, "unsupported",
                   "HARP stream requires the USB transport");
        return;
    }
    if (atomic_load_explicit(&d->audio.thread_live, memory_order_relaxed)) {
        send_error(d, e->rid, e->method, "busy", "stream already running");
        return;
    }
    evq_reset_for_new_stream();
    d->audio.fd = fd;
    d->audio.out_fd = out_fd;
    d->audio.mode = (uint32_t)mode;
    d->audio.rate = (uint32_t)rate;
    d->audio.nsamples = (uint32_t)nsamples;
    d->audio.reanchors = 0;
    atomic_store_explicit(&d->audio.running, true, memory_order_relaxed);
    if (pthread_create(&d->audio.thread, NULL, audio_thread, d) != 0) {
        atomic_store_explicit(&d->audio.running, false, memory_order_relaxed);
        send_error(d, e->rid, e->method, "internal", "thread");
        return;
    }
    atomic_store_explicit(&d->audio.thread_live, true, memory_order_relaxed);
    fprintf(stderr, "harp-deviced: audio stream started (%u Hz, %u-sample blocks, %s)\n",
            d->audio.rate, d->audio.nsamples,
            mode ? "host-paced" : "free-running");

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
    if (etype == 0) { /* UMP carriage (§9.10): body = one packet, words big-endian */
        const uint8_t *p;
        size_t pl;
        if (!harp_cdec_bytes(&dec, &p, &pl) || pl < 4) return;
        uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
                     p[3];
        uint32_t mt = w >> 28;
        if (mt == 2) { /* MIDI 1.0 channel voice in UMP */
            uint32_t status = (w >> 20) & 0xf, note = (w >> 8) & 0x7f, vel = w & 0x7f;
            dev_event ev = {msc, 0, note, 0, 0, 0};
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
        uint64_t n, key, id = 0, eep = 0, ets = 0;
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
            } else if (!harp_cdec_skip(&dec))
                return;
        }
        if (!have_id || !have_t || !have_end) return;
        if (target < 0) target = 0;
        if (target > 1) target = 1;
        dev_event ev = {msc, DEV_EV_RAMP, (uint32_t)id, (float)target, ets, 0};
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
        dev_event ev = {msc, DEV_EV_TRANSPORT, (uint32_t)flags, (float)tempo, 0, ppq};
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
    for (uint64_t i = 0; i < n; i++) {
        if (!harp_cdec_uint(&dec, &key)) return;
        if (key == 0) {
            if (!harp_cdec_uint(&dec, &id)) return;
            have_id = true;
        } else if (key == 1) {
            if (!harp_cdec_float(&dec, &v)) return;
            have_v = true;
        } else if (!harp_cdec_skip(&dec))
            return;
    }
    if (!have_id || !have_v) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    /* ts 0 = "now" = next render subblock; routing it through the queue
     * (instead of the old direct write) keeps ramp state render-owned —
     * the direct path raced ramps_advance on g_ramps[i].active */
    dev_event ev = {msc, DEV_EV_PARAM_SET, (uint32_t)id, (float)v, 0, 0};
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
        harp_cbor_float(&m, param_get(&g_params[i]));
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
    else if (strcmp(e.method, "diag.counters") == 0)
        handle_diag_counters(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.knob") == 0)
        handle_knob(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.params") == 0)
        handle_dev_params(d, &e);
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

