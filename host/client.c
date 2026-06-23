/* harp_client implementation — see client.h. The flows here were unified
 * from harp-probe.c and shell/runtime.cpp; where the two copies differed
 * the safer variant won (noted inline). */
#include "client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CREDIT_GRANT (16u << 20)

void harp_client_init(harp_client *c, harp_io *io, harp_link *link, harp_store *store,
                      void (*on_ntf)(void *ud, const harp_env *e), void *ud) {
    memset(c, 0, sizeof *c);
    c->io = io;
    c->link = link;
    c->store = store;
    c->on_ntf = on_ntf;
    c->ud = ud;
    harp_cbuf_init(&c->msg);
}

void harp_client_free(harp_client *c) {
    harp_cbuf_free(&c->msg);
}

static void handle_ntf(harp_client *c, const harp_env *e) {
    if (strcmp(e->method, "core.credit") == 0 && e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n, key, v;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_uint(&b, &v))
            c->peer_credit += v;
        return;
    }
    if (strcmp(e->method, "core.changed") == 0 && e->has_body) {
        /* §5.5: record the "re-query topic X" hint {0 => tstr}, then still surface it. */
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n, key;
        const char *s;
        size_t sl;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_text(&b, &s, &sl)) {
            size_t cl = sl < sizeof c->last_changed_topic - 1 ? sl : sizeof c->last_changed_topic - 1;
            memcpy(c->last_changed_topic, s, cl);
            c->last_changed_topic[cl] = 0;
            c->changed_pending = true;
        }
        if (c->on_ntf) c->on_ntf(c->ud, e);
        return;
    }
    if (c->on_ntf) c->on_ntf(c->ud, e);
}

/* §4.2.1: grant the device a fresh credit window on the CTL stream and track it in our
 * sliding `granted`. Used at hello and as a re-grant when consumption halves the window. */
static int client_grant(harp_client *c) {
    /* §4.2.1b: HARP_FORCE_CREDIT_GRANT shrinks the window for the starvation test; the
     * 16 MiB default otherwise (the queue stays empty and this path is zero-overhead). */
    uint64_t amt = CREDIT_GRANT;
    const char *f = getenv("HARP_FORCE_CREDIT_GRANT");
    if (f && *f) { uint64_t v = strtoull(f, NULL, 10); if (v) amt = v; } /* 0/garbage -> real grant */
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "core.credit", true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, amt);
    int rc = harp_link_send(c->io, HARP_STREAM_CTL, m.buf, m.len);
    harp_cbuf_free(&m);
    if (rc == 0) c->granted += amt;
    return rc;
}

/* Receive one frame; obj frames land in the store, notifications dispatch.
 * Returns the stream id, or -1 on transport failure. */
static int pump_one(harp_client *c, harp_env *ctl_env, bool *is_ctl) {
    uint8_t stream;
    *is_ctl = false;
    if (harp_link_recv(c->io, c->link, &stream, &c->msg) != 0) return -1;
    if (stream == HARP_STREAM_OBJ) {
        if (c->store) harp_store_put(c->store, c->msg.buf, c->msg.len, NULL);
        /* §4.2.1: re-grant the device on consume (sliding window, symmetric to the
         * device's handle_obj) so a long D->H pull never stalls once our grant drains. */
        uint64_t got = c->msg.len;
        if (c->granted >= got) c->granted -= got; else c->granted = 0;
        if (c->granted < CREDIT_GRANT / 2) client_grant(c);
        return stream;
    }
    if (stream != HARP_STREAM_CTL) return stream; /* evt/log: already consumed off the wire
                                                     (sits in c->msg, overwritten on the next
                                                     recv) — a caller awaiting a ctl rsp drops
                                                     it; the shell's idle pollEcho is what
                                                     normally drains evt, not this path */
    if (!harp_env_parse(c->msg.buf, c->msg.len, ctl_env)) return -1;
    if (ctl_env->msgtype == HARP_MSG_NOTIFICATION) {
        handle_ntf(c, ctl_env);
        return stream;
    }
    *is_ctl = true;
    return stream;
}

/* §4.2.1 host obj-send queue (FIFO ring of hashes; single-owner client, no lock). */
static void csendq_push(harp_client *c, const harp_hash *h) {
    if (c->sendq_count == HARP_CLIENT_SENDQ_CAP) { c->obj_drops++; return; } /* overflow: drop + count */
    c->sendq[c->sendq_tail] = *h;
    c->sendq_tail = (c->sendq_tail + 1) % HARP_CLIENT_SENDQ_CAP;
    c->sendq_count++;
}

/* Drain the obj-send queue in FIFO order, SELF-PUMPING the link for grants when the head
 * doesn't fit peer_credit. Unlike the device (whose recv thread flushes on each grant),
 * the host has NO background thread that raises peer_credit — pollEcho try-locks the held
 * ctlMutex_ and drops ctl notifications — so push_closure must drive pump_one itself until
 * the queue empties. Mirrors the fetch_closure receive loop. Returns 0 on full drain,
 * HARP_CLIENT_EIO on transport death or the spin cap (a silent peer). */
static int csendq_flush(harp_client *c) {
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    int rc = 0, spins = 0;
    while (c->sendq_count) {
        const harp_hash *h = &c->sendq[c->sendq_head];
        harp_cbuf_reset(&enc); /* harp_store_get APPENDS — reset before each get */
        if (harp_store_get(c->store, h, &enc) != 0) { rc = HARP_CLIENT_EIO; break; } /* local closure incomplete */
        while (enc.len > c->peer_credit) { /* self-pump for a grant on the ungated CTL stream */
            if (++spins > 100000) { rc = HARP_CLIENT_EIO; goto done; } /* dead/silent peer guard */
            harp_env pe;
            bool is_ctl;
            if (pump_one(c, &pe, &is_ctl) < 0) { rc = HARP_CLIENT_EIO; goto done; }
            /* is_ctl==true is a stray response/error (no request in flight) -> ignore;
             * core.credit notifications were already applied inside pump_one (handle_ntf).
             * An EVT frame (a front-panel echo) consumed here is dropped from the echo mirror
             * — acceptable: this self-pump only runs under credit STARVATION (never the
             * 16 MiB production window), and the mirror is best-effort, re-derived on re-fetch. */
        }
        c->peer_credit -= enc.len;
        if (harp_link_send(c->io, HARP_STREAM_OBJ, enc.buf, enc.len) != 0) { rc = HARP_CLIENT_EIO; break; }
        c->sendq_head = (c->sendq_head + 1) % HARP_CLIENT_SENDQ_CAP;
        c->sendq_count--;
    }
done:
    harp_cbuf_free(&enc);
    if (rc != 0) c->sendq_head = c->sendq_tail = c->sendq_count = 0; /* error: don't leak stale order */
    return rc;
}

void harp_client_req_head(harp_client *c, harp_cbuf *out, const char *method,
                          bool has_body) {
    harp_cbuf_reset(out);
    c->next_rid++;
    harp_env_head(out, HARP_MSG_REQUEST, c->next_rid, method, has_body);
}

int harp_client_send(harp_client *c, const harp_cbuf *req) {
    c->err_code[0] = c->err_msg[0] = 0;
    return harp_link_send(c->io, HARP_STREAM_CTL, req->buf, req->len) == 0
               ? 0
               : HARP_CLIENT_EIO;
}

int harp_client_wait(harp_client *c, uint64_t rid, harp_cbuf *rsp, harp_env *e) {
    for (;;) {
        harp_env pe;
        bool is_rsp;
        if (pump_one(c, &pe, &is_rsp) < 0) return HARP_CLIENT_EIO;
        if (!is_rsp) continue;
        if (pe.rid != rid) continue; /* single in-flight: stale, skip */
        if (pe.msgtype == HARP_MSG_ERROR) {
            snprintf(c->err_method, sizeof c->err_method, "%s", pe.method);
            snprintf(c->err_code, sizeof c->err_code, "?");
            if (pe.has_body) {
                harp_cdec b;
                harp_cdec_init(&b, pe.body, pe.body_len);
                uint64_t n, key;
                const char *s;
                size_t sl;
                if (harp_cdec_map(&b, &n)) {
                    for (uint64_t i = 0; i < n; i++) {
                        if (!harp_cdec_uint(&b, &key)) break;
                        if (key == 0 && harp_cdec_text(&b, &s, &sl) &&
                            sl < sizeof c->err_code) {
                            memcpy(c->err_code, s, sl);
                            c->err_code[sl] = 0;
                        } else if (key == 1 && harp_cdec_text(&b, &s, &sl) &&
                                   sl < sizeof c->err_msg) {
                            memcpy(c->err_msg, s, sl);
                            c->err_msg[sl] = 0;
                        } else if (key == 2 && strcmp(c->err_code, "incompatible") == 0) {
                            /* §5.4: device's supported major range {0 => min, 1 => max}.
                             * Key 0 (code) precedes key 2 in the map, so err_code is set. */
                            uint64_t dn, dk, dv;
                            if (harp_cdec_map(&b, &dn)) {
                                for (uint64_t j = 0; j < dn; j++) {
                                    if (!harp_cdec_uint(&b, &dk) || !harp_cdec_uint(&b, &dv)) break;
                                    if (dk == 0) c->incompat_major_min = (uint32_t)dv;
                                    else if (dk == 1) c->incompat_major_max = (uint32_t)dv;
                                }
                            }
                        } else if (key > 1 && !harp_cdec_skip(&b))
                            break;
                    }
                }
            }
            return HARP_CLIENT_EDEV;
        }
        harp_cbuf_reset(rsp);
        harp_cbuf_put(rsp, c->msg.buf, c->msg.len);
        return harp_env_parse(rsp->buf, rsp->len, e) ? 0 : HARP_CLIENT_EIO;
    }
}

int harp_client_request(harp_client *c, harp_cbuf *req, harp_cbuf *rsp, harp_env *e) {
    uint64_t rid = c->next_rid;
    int rc = harp_client_send(c, req);
    return rc != 0 ? rc : harp_client_wait(c, rid, rsp, e);
}

/* ---------------- hello + identity ---------------- */

static bool parse_identity(harp_cdec *b, harp_client_identity *id) {
    memset(id, 0, sizeof *id);
    uint64_t n;
    if (!harp_cdec_map(b, &n)) return false;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t key;
        if (!harp_cdec_uint(b, &key)) return false;
        const char *s;
        size_t sl;
        switch (key) {
            case 0:
            case 1: { /* vendor / product: {0: uint id, 1: text name} */
                uint64_t mn, mkey, uid = 0;
                char *dst = key == 0 ? id->vendor : id->product;
                if (!harp_cdec_map(b, &mn)) return false;
                for (uint64_t j = 0; j < mn; j++) {
                    if (!harp_cdec_uint(b, &mkey)) return false;
                    if (mkey == 0) {
                        if (!harp_cdec_uint(b, &uid)) return false;
                    } else if (mkey == 1) {
                        if (!harp_cdec_text(b, &s, &sl)) return false;
                        if (sl < 64) {
                            memcpy(dst, s, sl);
                            dst[sl] = 0;
                        }
                    } else if (!harp_cdec_skip(b))
                        return false;
                }
                if (key == 0)
                    id->vendor_id = (uint32_t)uid;
                else
                    id->product_id = (uint32_t)uid;
                break;
            }
            case 2:
                if (!harp_cdec_text(b, &s, &sl) || sl >= sizeof id->serial) return false;
                memcpy(id->serial, s, sl);
                id->serial[sl] = 0;
                break;
            case 3:
                if (!harp_cdec_text(b, &s, &sl) || sl >= sizeof id->fw) return false;
                memcpy(id->fw, s, sl);
                id->fw[sl] = 0;
                break;
            case 4: { /* engine triple */
                uint64_t mn, mkey;
                if (!harp_cdec_map(b, &mn)) return false;
                for (uint64_t j = 0; j < mn; j++) {
                    if (!harp_cdec_uint(b, &mkey)) return false;
                    if (mkey == 0) {
                        if (!harp_cdec_text(b, &s, &sl) || sl >= 64) return false;
                        memcpy(id->engine_id, s, sl);
                        id->engine_id[sl] = 0;
                    } else if (mkey == 1) {
                        if (!harp_cdec_text(b, &s, &sl) || sl >= 32) return false;
                        memcpy(id->engine_ver, s, sl);
                        id->engine_ver[sl] = 0;
                    } else if (mkey == 2) {
                        if (!harp_hash_read(b, &id->param_map_hash)) return false;
                    } else if (!harp_cdec_skip(b))
                        return false;
                }
                break;
            }
            case 6: { /* capabilities: [* tstr] */
                uint64_t cn;
                if (!harp_cdec_array(b, &cn)) return false;
                for (uint64_t j = 0; j < cn; j++) {
                    if (!harp_cdec_text(b, &s, &sl)) return false;
                    if (id->ncaps < HARP_CLIENT_MAX_CAPS && sl < 32) {
                        memcpy(id->caps[id->ncaps], s, sl);
                        id->caps[id->ncaps][sl] = 0;
                        id->ncaps++;
                    }
                }
                break;
            }
            case 8: { /* §6.4 latency-profile: [* {0 rate,1 in-lat,2 out-lat,3 buf-depth}] */
                uint64_t pn;
                if (!harp_cdec_array(b, &pn)) return false;
                for (uint64_t j = 0; j < pn; j++) {
                    uint64_t mn, mkey, v;
                    if (!harp_cdec_map(b, &mn)) return false;
                    uint32_t rate = 0, il = 0, ol = 0, bd = 0;
                    for (uint64_t k = 0; k < mn; k++) {
                        if (!harp_cdec_uint(b, &mkey)) return false;
                        if (mkey == 0) {
                            if (!harp_cdec_uint(b, &v)) return false;
                            rate = (uint32_t)v;
                        } else if (mkey == 1) {
                            if (!harp_cdec_uint(b, &v)) return false;
                            il = (uint32_t)v;
                        } else if (mkey == 2) {
                            if (!harp_cdec_uint(b, &v)) return false;
                            ol = (uint32_t)v;
                        } else if (mkey == 3) {
                            if (!harp_cdec_uint(b, &v)) return false;
                            bd = (uint32_t)v;
                        } else if (!harp_cdec_skip(b))
                            return false;
                    }
                    if (id->nlat < HARP_CLIENT_MAX_LAT) {
                        id->lat[id->nlat].rate = rate;
                        id->lat[id->nlat].in_lat = il;
                        id->lat[id->nlat].out_lat = ol;
                        id->lat[id->nlat].buf_depth = bd;
                        id->nlat++;
                    }
                }
                break;
            }
            case 10:
                if (!harp_cdec_uint(b, &id->boot_count)) return false;
                break;
            case 13: { /* §9.6 transaction limits { 0: max concurrent, 1: max events/txn } */
                uint64_t tn, k;
                if (!harp_cdec_map(b, &tn)) return false;
                for (uint64_t j = 0; j < tn; j++) {
                    if (!harp_cdec_uint(b, &k)) return false;
                    if (k == 0) {
                        if (!harp_cdec_uint(b, &id->txn_max)) return false;
                    } else if (k == 1) {
                        if (!harp_cdec_uint(b, &id->txn_events)) return false;
                    } else if (!harp_cdec_skip(b))
                        return false;
                }
                break;
            }
            case 14: { /* §6.4 rt-profile: { ?0: eth-target-floor frames, ?1: RTP packet nsamples } */
                uint64_t tn, k, v;
                if (!harp_cdec_map(b, &tn)) return false;
                for (uint64_t j = 0; j < tn; j++) {
                    if (!harp_cdec_uint(b, &k)) return false;
                    if (k == 0) {
                        if (!harp_cdec_uint(b, &v)) return false;
                        id->eth_target_floor = (uint32_t)v;
                    } else if (k == 1) {
                        if (!harp_cdec_uint(b, &v)) return false;
                        id->eth_nsamples = (uint32_t)v;
                    } else if (!harp_cdec_skip(b))
                        return false;
                }
                break;
            }
            default:
                if (!harp_cdec_skip(b)) return false;
        }
    }
    return true;
}

bool harp_client_has_cap(const harp_client_identity *id, const char *cap) {
    for (size_t i = 0; i < id->ncaps; i++)
        if (strcmp(id->caps[i], cap) == 0) return true;
    return false;
}

int harp_client_hello(harp_client *c, const char *agent, harp_client_identity *out) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "core.hello", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    const char *fmaj = getenv("HARP_FORCE_PROTO_MAJOR"); /* §5.4 test seam: force a version mismatch */
    harp_cbor_array(&req, 2);
    harp_cbor_uint(&req, fmaj && fmaj[0] ? strtoull(fmaj, NULL, 10) : 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 1);
    harp_cbor_text(&req, agent);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    bool ok = false;
    if (rc == 0 && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n && !b.err; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 1)
                    ok = parse_identity(&b, out);
                else
                    harp_cdec_skip(&b);
            }
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc != 0) {
        if (rc == HARP_CLIENT_EDEV && strcmp(c->err_code, "incompatible") == 0)
            return HARP_CLIENT_EINCOMPAT; /* §5.4: distinct signal -> host prompts a firmware/host update */
        return rc;
    }
    if (!ok) return HARP_CLIENT_EIO;

    /* §4.2.1: grant the device obj credit so pulls flow immediately; granted seeds the
     * sliding window that pump_one re-grants on consume. */
    c->granted = 0;
    return client_grant(c) == 0 ? 0 : HARP_CLIENT_EIO;
}

/* §5.5: re-fetch identity without a session reset. The response body IS the bare
 * identity map (unlike core.hello, which wraps it under key 1). */
int harp_client_identify(harp_client *c, harp_client_identity *out) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "core.identify", false);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    bool ok = false;
    if (rc == 0 && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        ok = parse_identity(&b, out);
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc != 0) return rc;
    return ok ? 0 : HARP_CLIENT_EIO;
}

/* §5.5: liveness — the device echoes the request body verbatim. Send a fixed nonce
 * and verify the echo, so a silent/garbled link is caught (not just a missing reply). */
int harp_client_ping(harp_client *c) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "core.ping", true);
    /* per-call nonce (from the rid this request will carry) so each ping is unique — a fresh
     * echo must traverse the live link; a replayed/canned response would not match. */
    uint64_t r = c->next_rid;
    const uint8_t nonce[4] = {(uint8_t)r, (uint8_t)(r >> 8), (uint8_t)(r >> 16), (uint8_t)(r >> 24)};
    harp_cbor_bytes(&req, nonce, sizeof nonce);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    bool ok = false;
    if (rc == 0 && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        const uint8_t *p;
        size_t pl;
        ok = harp_cdec_bytes(&b, &p, &pl) && pl == sizeof nonce && memcmp(p, nonce, pl) == 0;
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc != 0) return rc;
    return ok ? 0 : HARP_CLIENT_EIO;
}

/* §5.5: orderly session end. The device acks then closes its end (the session loop
 * breaks on d->closing); callers should not issue further requests on this client. */
int harp_client_bye(harp_client *c) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "core.bye", false);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return rc;
}

/* ---------------- refs / snapshot ---------------- */

int harp_client_refs(harp_client *c, harp_ref *out, size_t cap, size_t *count) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "state.refs", false);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    *count = 0;
    if (rc == 0) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key, alen;
        if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
            key == 0 && harp_cdec_array(&b, &alen)) {
            for (uint64_t i = 0; i < alen && *count < cap; i++)
                if (harp_ref_decode(&b, &out[*count]))
                    (*count)++;
                else
                    break;
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return rc;
}

int harp_client_find_ref(harp_client *c, const char *name, harp_ref *out) {
    /* Ask the device to filter to just `name` (state.refs body { 0 => name }), so we don't
     * pull the whole ref list — which grows with the §11.4 archive count and exceeds the ctl
     * bound on a long-lived device, breaking refsLocked/getState (debt #22). We still SEARCH
     * the returned array for `name`, so an OLD device that ignores the filter and returns the
     * full list still works (as long as it fits the bound). */
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "state.refs", true);
    harp_cbor_map(&req, 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, name);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    bool found = false;
    if (rc == 0 && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key, alen;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_array(&b, &alen)) {
            for (uint64_t i = 0; i < alen; i++) {
                harp_ref r;
                if (!harp_ref_decode(&b, &r)) break;
                if (strcmp(r.name, name) == 0) {
                    *out = r;
                    found = true;
                    break;
                }
            }
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc != 0) return rc;
    return found ? 0 : HARP_CLIENT_EIO;
}

int harp_client_snapshot(harp_client *c, const char *refname, const char *msg,
                         harp_hash *out_hash) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "state.snapshot", true);
    harp_cbor_map(&req, msg ? 2 : 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, refname);
    if (msg) {
        harp_cbor_uint(&req, 1);
        harp_cbor_text(&req, msg);
    }
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    if (rc == 0) {
        rc = HARP_CLIENT_EIO;
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key;
        if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
            key == 0 && harp_hash_read(&b, out_hash)) {
            rc = 0;
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return rc;
}

/* ---------------- closure transfer ---------------- */

/* dedup'ing visit list: closure walks must not revisit shared children
 * (the shell's HashList semantics — the probe's array version could
 * re-expand duplicates) */
struct hash_list {
    harp_hash h[HARP_CLIENT_MAX_CLOSURE];
    size_t n;
};

static bool list_contains(const struct hash_list *l, const harp_hash *x) {
    for (size_t i = 0; i < l->n; i++)
        if (harp_hash_eq(&l->h[i], x)) return true;
    return false;
}

static bool list_collect(const harp_hash *h, void *ud) {
    struct hash_list *l = ud;
    if (!list_contains(l, h) && l->n < HARP_CLIENT_MAX_CLOSURE) l->h[l->n++] = *h;
    return true;
}

int harp_client_fetch_closure(harp_client *c, const harp_hash *root, size_t *fetched) {
    if (!c->store) return HARP_CLIENT_EIO;
    struct hash_list pending = {{{{0}}}, 0};
    list_collect(root, &pending);
    size_t cursor = 0, got = 0;
    while (cursor < pending.n) {
        /* want what we don't hold, in wire-friendly batches */
        harp_hash want[64];
        size_t nwant = 0;
        for (size_t i = cursor; i < pending.n && nwant < 64; i++)
            if (!harp_store_have(c->store, &pending.h[i])) want[nwant++] = pending.h[i];
        if (nwant) {
            harp_cbuf req, rsp;
            harp_cbuf_init(&req);
            harp_cbuf_init(&rsp);
            harp_client_req_head(c, &req, "state.want", true);
            harp_cbor_map(&req, 1);
            harp_cbor_uint(&req, 0);
            harp_cbor_array(&req, nwant);
            for (size_t i = 0; i < nwant; i++)
                harp_cbor_bytes(&req, want[i].b, HARP_HASH_LEN);
            harp_env e;
            int rc = harp_client_request(c, &req, &rsp, &e);
            harp_cbuf_free(&req);
            harp_cbuf_free(&rsp);
            if (rc != 0) return rc;
            /* objects arrive on the obj stream; pump until all held */
            int spins = 0;
            for (;;) {
                bool all = true;
                for (size_t i = 0; i < nwant; i++)
                    if (!harp_store_have(c->store, &want[i])) all = false;
                if (all) break;
                if (++spins > 4096) return HARP_CLIENT_EIO;
                harp_env e2;
                bool is_rsp;
                if (pump_one(c, &e2, &is_rsp) < 0) return HARP_CLIENT_EIO;
            }
            got += nwant;
        }
        /* expand children of this generation (parents excluded: bundle
         * closure = current state, not history — spec note in deviced) */
        size_t end = pending.n;
        for (; cursor < end; cursor++) {
            harp_cbuf enc;
            harp_cbuf_init(&enc);
            if (harp_store_get(c->store, &pending.h[cursor], &enc) == 0)
                harp_obj_foreach_child(enc.buf, enc.len, false, list_collect, &pending);
            harp_cbuf_free(&enc);
        }
    }
    if (fetched) *fetched = got;
    return 0;
}

int harp_client_push_closure(harp_client *c, const harp_hash *root, size_t *sent,
                             size_t *already) {
    if (!c->store) return HARP_CLIENT_EIO;
    /* collect the local closure; it must be complete here */
    struct hash_list clo = {{{{0}}}, 0};
    list_collect(root, &clo);
    for (size_t cur = 0; cur < clo.n; cur++) {
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        if (harp_store_get(c->store, &clo.h[cur], &enc) != 0) {
            harp_cbuf_free(&enc);
            return HARP_CLIENT_EIO; /* local closure incomplete */
        }
        harp_obj_foreach_child(enc.buf, enc.len, false, list_collect, &clo);
        harp_cbuf_free(&enc);
    }

    /* have: learn the diff so re-sync stays proportional to it */
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "state.have", true);
    harp_cbor_map(&req, 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_array(&req, clo.n);
    for (size_t i = 0; i < clo.n; i++) harp_cbor_bytes(&req, clo.h[i].b, HARP_HASH_LEN);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    bool missing[HARP_CLIENT_MAX_CLOSURE] = {false};
    size_t nmissing = 0;
    if (rc == 0 && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key, alen;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_array(&b, &alen) && alen == clo.n) {
            for (size_t i = 0; i < clo.n; i++) {
                bool have;
                if (!harp_cdec_bool(&b, &have)) break;
                if (!have) {
                    missing[i] = true;
                    nmissing++;
                }
            }
        }
    }
    if (rc != 0) goto out;

    /* send: announce sizes, then stream the missing objects */
    if (nmissing) {
        uint64_t total = 0;
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        for (size_t i = 0; i < clo.n; i++)
            if (missing[i]) {
                harp_cbuf_reset(&enc);
                if (harp_store_get(c->store, &clo.h[i], &enc) == 0) total += enc.len;
            }
        harp_client_req_head(c, &req, "state.send", true);
        harp_cbor_map(&req, 2);
        harp_cbor_uint(&req, 0);
        harp_cbor_array(&req, nmissing);
        for (size_t i = 0; i < clo.n; i++)
            if (missing[i]) harp_cbor_bytes(&req, clo.h[i].b, HARP_HASH_LEN);
        harp_cbor_uint(&req, 1);
        harp_cbor_uint(&req, total);
        rc = harp_client_request(c, &req, &rsp, &e);
        /* §4.2.1: enqueue every missing object (FIFO, fixed clo order) then self-pump the
         * link until the queue drains — push_closure must not return with a fitting object
         * stranded, and the host has no background thread that raises peer_credit. */
        for (size_t i = 0; rc == 0 && i < clo.n; i++)
            if (missing[i]) csendq_push(c, &clo.h[i]);
        if (rc == 0) rc = csendq_flush(c);
        harp_cbuf_free(&enc);
    }
out:
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc == 0) {
        if (sent) *sent = nmissing;
        if (already) *already = clo.n - nmissing;
    }
    return rc;
}

int harp_client_refset(harp_client *c, const char *name, const harp_hash *expect,
                       const harp_hash *target, bool create, bool force,
                       uint64_t *new_gen) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "state.refset", true);
    /* flags (key 3): bit 0 create-if-unborn, bit 1 force (§11.3 CAS override).
     * Omitted when 0, so a plain CAS is byte-identical to before (the device
     * defaults the absent key to 0). */
    int flags = (create ? 1 : 0) | (force ? 2 : 0);
    harp_cbor_map(&req, flags ? 4 : 3);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, name);
    harp_cbor_uint(&req, 1);
    if (expect)
        harp_cbor_bytes(&req, expect->b, HARP_HASH_LEN);
    else
        harp_cbor_null(&req);
    harp_cbor_uint(&req, 2);
    harp_cbor_bytes(&req, target->b, HARP_HASH_LEN);
    if (flags) {
        harp_cbor_uint(&req, 3);
        harp_cbor_uint(&req, (uint64_t)flags);
    }
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    if (rc == 0 && new_gen) {
        *new_gen = 0;
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key;
        if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
            key == 0)
            harp_cdec_uint(&b, new_gen);
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return rc;
}

int harp_client_reconcile_offer(harp_client *c, const char *expect, const char *live,
                                bool dirty) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "x.harp.reconcile.offer", true);
    harp_cbor_map(&req, 3);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, expect ? expect : "");
    harp_cbor_uint(&req, 1);
    harp_cbor_text(&req, live ? live : "");
    harp_cbor_uint(&req, 2);
    harp_cbor_uint(&req, dirty ? 1 : 0);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return rc;
}

int harp_client_reconcile_poll(harp_client *c, bool *pending, int *choice) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "x.harp.reconcile.poll", false);
    harp_env e;
    int rc = harp_client_request(c, &req, &rsp, &e);
    if (rc == 0 && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 0) {
                    bool v = false;
                    harp_cdec_bool(&b, &v);
                    if (pending) *pending = v;
                } else if (key == 1) {
                    int64_t v = -1;
                    harp_cdec_int(&b, &v);
                    if (choice) *choice = (int)v;
                } else {
                    harp_cdec_skip(&b);
                }
            }
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return rc;
}
