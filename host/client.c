/* harp_client implementation — see client.h. The flows here were unified
 * from harp-probe.c and shell/runtime.cpp; where the two copies differed
 * the safer variant won (noted inline). */
#include "client.h"

#include <stdio.h>
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
    if (c->on_ntf) c->on_ntf(c->ud, e);
}

/* Receive one frame; obj frames land in the store, notifications dispatch.
 * Returns the stream id, or -1 on transport failure. */
static int pump_one(harp_client *c, harp_env *ctl_env, bool *is_ctl) {
    uint8_t stream;
    *is_ctl = false;
    if (harp_link_recv(c->io, c->link, &stream, &c->msg) != 0) return -1;
    if (stream == HARP_STREAM_OBJ) {
        if (c->store) harp_store_put(c->store, c->msg.buf, c->msg.len, NULL);
        return stream;
    }
    if (stream != HARP_STREAM_CTL) return stream; /* evt during a wait: owner's
                                                     poll loop sees it next */
    if (!harp_env_parse(c->msg.buf, c->msg.len, ctl_env)) return -1;
    if (ctl_env->msgtype == HARP_MSG_NOTIFICATION) {
        handle_ntf(c, ctl_env);
        return stream;
    }
    *is_ctl = true;
    return stream;
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
                        const uint8_t *hp;
                        size_t hl;
                        if (!harp_cdec_bytes(b, &hp, &hl) || hl != HARP_HASH_LEN)
                            return false;
                        memcpy(id->param_map_hash.b, hp, HARP_HASH_LEN);
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
    harp_cbor_array(&req, 2);
    harp_cbor_uint(&req, 1);
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
    if (rc != 0) return rc;
    if (!ok) return HARP_CLIENT_EIO;

    /* grant the device obj credit so pulls can flow immediately */
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "core.credit", true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, CREDIT_GRANT);
    int src = harp_link_send(c->io, HARP_STREAM_CTL, m.buf, m.len);
    harp_cbuf_free(&m);
    return src == 0 ? 0 : HARP_CLIENT_EIO;
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
    harp_ref refs[64];
    size_t n;
    int rc = harp_client_refs(c, refs, 64, &n);
    if (rc != 0) return rc;
    for (size_t i = 0; i < n; i++)
        if (strcmp(refs[i].name, name) == 0) {
            *out = refs[i];
            return 0;
        }
    return HARP_CLIENT_EIO;
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
        const uint8_t *hp;
        size_t hl;
        if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
            key == 0 && harp_cdec_bytes(&b, &hp, &hl) && hl == HARP_HASH_LEN) {
            memcpy(out_hash->b, hp, HARP_HASH_LEN);
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
        for (size_t i = 0; rc == 0 && i < clo.n; i++) {
            if (!missing[i]) continue;
            harp_cbuf_reset(&enc);
            if (harp_store_get(c->store, &clo.h[i], &enc) != 0) {
                rc = HARP_CLIENT_EIO;
                break;
            }
            if (enc.len <= c->peer_credit) c->peer_credit -= enc.len;
            if (harp_link_send(c->io, HARP_STREAM_OBJ, enc.buf, enc.len) != 0)
                rc = HARP_CLIENT_EIO;
        }
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
