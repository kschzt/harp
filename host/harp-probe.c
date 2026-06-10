/* harp-probe — HARP host-side probe CLI (spec draft 0.3).
 *
 * Speaks the framed link over the TCP dev transport to harp-deviced (or, on
 * the Pi, to the daemon behind the USB gadget once that lands). Implements
 * the host side of `harp-core` + `harp-recall`: identity, refs, pull (save),
 * and the canonical archive-before-push restore of §11.4/§12.2.
 *
 *   harp-probe [-d HOST:PORT] [-s STOREDIR] CMD
 *     identify | refs | counters | params
 *     knob ID VALUE          simulate a front-panel edit
 *     save                   pull live/project into the local store ("save project")
 *     restore                push saved state back (archive-before-push, CAS)
 *     demo                   narrated end-to-end recall walkthrough
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/object.h"
#include "harp/store.h"

#define EXPECT_REF "expected/live-project"
#define LIVE_REF "live/project"
#define CREDIT_GRANT (16u << 20)

typedef struct {
    int fd;
    harp_link link;
    harp_cbuf msg;
    uint64_t next_rid;
    uint64_t peer_credit;
    harp_store store;
    bool verbose_ntf;
} probe;

static void die(const char *msg) {
    fprintf(stderr, "harp-probe: %s\n", msg);
    exit(1);
}

/* ---------------- transport ---------------- */

static int dial(const char *hostport) {
    char host[256];
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport) die("device address must be HOST:PORT");
    size_t hl = (size_t)(colon - hostport);
    if (hl >= sizeof host) die("host too long");
    memcpy(host, hostport, hl);
    host[hl] = 0;
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0) die("cannot resolve device host");
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) die("cannot connect to device");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* ---------------- request/response with interleaved traffic ---------------- */

static void handle_ntf(probe *p, const harp_env *e) {
    if (strcmp(e->method, "core.credit") == 0 && e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n, key, v;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_uint(&b, &v))
            p->peer_credit += v;
        return;
    }
    if (strcmp(e->method, "state.changed") == 0 && p->verbose_ntf && e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (!harp_cdec_map(&b, &n)) return;
        char name[HARP_REF_NAME_MAX] = "?";
        uint64_t gen = 0;
        bool dirty = false;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t key;
            if (!harp_cdec_uint(&b, &key)) return;
            if (key == 0) {
                const char *s;
                size_t sl;
                if (!harp_cdec_text(&b, &s, &sl) || sl >= sizeof name) return;
                memcpy(name, s, sl);
                name[sl] = 0;
            } else if (key == 2) {
                if (!harp_cdec_uint(&b, &gen)) return;
            } else if (key == 3) {
                if (!harp_cdec_bool(&b, &dirty)) return;
            } else {
                if (!harp_cdec_skip(&b)) return;
            }
        }
        printf("      ntf state.changed: %s gen=%llu dirty=%s\n", name,
               (unsigned long long)gen, dirty ? "true" : "false");
    }
}

/* Send a request (envelope already in `out`) and wait for its response.
 * Objects arriving on the obj stream are stored; notifications handled.
 * Returns the response envelope; rsp_buf holds the bytes. Exits on error
 * envelope unless tolerate_error. */
static harp_env request(probe *p, harp_cbuf *out, harp_cbuf *rsp_buf, bool tolerate_error) {
    uint64_t rid = p->next_rid; /* caller used this rid in the head */
    if (harp_link_send(p->fd, HARP_STREAM_CTL, out->buf, out->len) != 0)
        die("link send failed");
    for (;;) {
        uint8_t stream;
        int rc = harp_link_recv(p->fd, &p->link, &stream, &p->msg);
        if (rc != 0) die("link receive failed (device gone?)");
        if (stream == HARP_STREAM_OBJ) {
            harp_store_put(&p->store, p->msg.buf, p->msg.len, NULL);
            continue;
        }
        if (stream != HARP_STREAM_CTL) continue;
        harp_env e;
        if (!harp_env_parse(p->msg.buf, p->msg.len, &e)) die("malformed envelope");
        if (e.msgtype == HARP_MSG_NOTIFICATION) {
            handle_ntf(p, &e);
            continue;
        }
        if (e.rid != rid) continue; /* not ours (single in-flight, shouldn't happen) */
        if (e.msgtype == HARP_MSG_ERROR && !tolerate_error) {
            char code[64] = "?";
            if (e.has_body) {
                harp_cdec b;
                harp_cdec_init(&b, e.body, e.body_len);
                uint64_t n, key;
                const char *s;
                size_t sl;
                if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
                    key == 0 && harp_cdec_text(&b, &s, &sl) && sl < sizeof code) {
                    memcpy(code, s, sl);
                    code[sl] = 0;
                }
            }
            fprintf(stderr, "harp-probe: device error '%s' on %s\n", code, e.method);
            exit(1);
        }
        harp_cbuf_reset(rsp_buf);
        harp_cbuf_put(rsp_buf, p->msg.buf, p->msg.len);
        harp_env_parse(rsp_buf->buf, rsp_buf->len, &e);
        return e;
    }
}

static void req_head(probe *p, harp_cbuf *out, const char *method, bool has_body) {
    harp_cbuf_reset(out);
    p->next_rid++;
    harp_env_head(out, HARP_MSG_REQUEST, p->next_rid, method, has_body);
}

/* ---------------- protocol ops ---------------- */

typedef struct {
    char serial[64];
    char vendor[64], product[64], fw[32];
    char engine_id[64], engine_ver[32];
    harp_hash param_map_hash;
    uint64_t boot_count;
} identity;

static bool parse_identity(harp_cdec *b, identity *id) {
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
            case 1: { /* vendor / product: {0: uint, 1: name} */
                uint64_t mn, mkey, uid;
                if (!harp_cdec_map(b, &mn)) return false;
                for (uint64_t j = 0; j < mn; j++) {
                    if (!harp_cdec_uint(b, &mkey)) return false;
                    if (mkey == 0) {
                        if (!harp_cdec_uint(b, &uid)) return false;
                    } else if (mkey == 1) {
                        if (!harp_cdec_text(b, &s, &sl)) return false;
                        char *dst = key == 0 ? id->vendor : id->product;
                        if (sl < 64) {
                            memcpy(dst, s, sl);
                            dst[sl] = 0;
                        }
                    } else if (!harp_cdec_skip(b))
                        return false;
                }
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
            case 4: { /* engine */
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
            case 10:
                if (!harp_cdec_uint(b, &id->boot_count)) return false;
                break;
            default:
                if (!harp_cdec_skip(b)) return false;
        }
    }
    return true;
}

static identity do_hello(probe *p) {
    harp_cbuf out, rsp;
    harp_cbuf_init(&out);
    harp_cbuf_init(&rsp);
    req_head(p, &out, "core.hello", true);
    harp_cbor_map(&out, 2);
    harp_cbor_uint(&out, 0);
    harp_cbor_array(&out, 2);
    harp_cbor_uint(&out, 1);
    harp_cbor_uint(&out, 0);
    harp_cbor_uint(&out, 1);
    harp_cbor_text(&out, "harp-probe 0.1 (dev)");
    harp_env e = request(p, &out, &rsp, false);

    identity id;
    memset(&id, 0, sizeof id);
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n;
    bool ok = false;
    if (e.has_body && harp_cdec_map(&b, &n)) {
        for (uint64_t i = 0; i < n && !b.err; i++) {
            uint64_t key;
            if (!harp_cdec_uint(&b, &key)) break;
            if (key == 1)
                ok = parse_identity(&b, &id);
            else
                harp_cdec_skip(&b);
        }
    }
    harp_cbuf_free(&out);
    harp_cbuf_free(&rsp);
    if (!ok) die("bad core.hello response");

    /* grant the device obj credit for pulls */
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "core.credit", true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, CREDIT_GRANT);
    harp_link_send(p->fd, HARP_STREAM_CTL, m.buf, m.len);
    harp_cbuf_free(&m);
    return id;
}

#define MAX_REFS 64

static size_t get_refs(probe *p, harp_ref out[MAX_REFS]) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "state.refs", false);
    harp_env e = request(p, &req, &rsp, false);
    size_t count = 0;
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n, key, alen;
    if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
        key == 0 && harp_cdec_array(&b, &alen)) {
        for (uint64_t i = 0; i < alen && count < MAX_REFS; i++)
            if (harp_ref_decode(&b, &out[count]))
                count++;
            else
                break;
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return count;
}

static bool find_ref(harp_ref *refs, size_t n, const char *name, harp_ref *out) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(refs[i].name, name) == 0) {
            *out = refs[i];
            return true;
        }
    return false;
}

static void print_ref(const harp_ref *r) {
    char hex[2 * HARP_HASH_LEN + 1] = "(unborn)";
    if (!r->unborn) {
        harp_hash_hex(&r->hash, hex);
        hex[12] = 0; /* short display */
    }
    printf("  %-28s %-12s gen=%-6llu %s\n", r->name, hex, (unsigned long long)r->generation,
           r->dirty ? "DIRTY" : "clean");
}

/* device snapshot of live/project; returns new head */
static harp_hash remote_snapshot(probe *p, const char *msg) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "state.snapshot", true);
    harp_cbor_map(&req, msg ? 2 : 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, LIVE_REF);
    if (msg) {
        harp_cbor_uint(&req, 1);
        harp_cbor_text(&req, msg);
    }
    harp_env e = request(p, &req, &rsp, false);
    harp_hash h;
    memset(&h, 0, sizeof h);
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n, key;
    const uint8_t *hp;
    size_t hl;
    if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
        key == 0 && harp_cdec_bytes(&b, &hp, &hl) && hl == HARP_HASH_LEN)
        memcpy(h.b, hp, HARP_HASH_LEN);
    else
        die("bad state.snapshot response");
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return h;
}

/* child-hash sink for closure walks */
struct hash_sink {
    harp_hash *arr;
    size_t n, cap;
    bool dedupe;
};

static bool sink_collect(const harp_hash *h, void *ud) {
    struct hash_sink *s = ud;
    if (s->dedupe)
        for (size_t i = 0; i < s->n; i++)
            if (harp_hash_eq(&s->arr[i], h)) return true;
    if (s->n < s->cap) s->arr[s->n++] = *h;
    return true;
}

/* Fetch the closure of `root` from the device into the local store.
 * Walks breadth-first: want anything not held, then read children. */
static void fetch_closure(probe *p, const harp_hash *root) {
    harp_hash pending[512];
    size_t npend = 1;
    pending[0] = *root;
    size_t fetched = 0;
    while (npend) {
        /* want everything pending that we don't hold */
        harp_hash want[64];
        size_t nwant = 0;
        for (size_t i = 0; i < npend && nwant < 64; i++)
            if (!harp_store_have(&p->store, &pending[i])) want[nwant++] = pending[i];
        if (nwant) {
            harp_cbuf req, rsp;
            harp_cbuf_init(&req);
            harp_cbuf_init(&rsp);
            req_head(p, &req, "state.want", true);
            harp_cbor_map(&req, 1);
            harp_cbor_uint(&req, 0);
            harp_cbor_array(&req, nwant);
            for (size_t i = 0; i < nwant; i++)
                harp_cbor_bytes(&req, want[i].b, HARP_HASH_LEN);
            request(p, &req, &rsp, false);
            harp_cbuf_free(&req);
            harp_cbuf_free(&rsp);
            /* objects arrive on the obj stream; pump the link until all held */
            int spins = 0;
            while (spins < 1000) {
                bool all = true;
                for (size_t i = 0; i < nwant; i++)
                    if (!harp_store_have(&p->store, &want[i])) all = false;
                if (all) break;
                uint8_t stream;
                if (harp_link_recv(p->fd, &p->link, &stream, &p->msg) != 0)
                    die("link receive failed during fetch");
                if (stream == HARP_STREAM_OBJ)
                    harp_store_put(&p->store, p->msg.buf, p->msg.len, NULL);
                else if (stream == HARP_STREAM_CTL) {
                    harp_env e;
                    if (harp_env_parse(p->msg.buf, p->msg.len, &e) &&
                        e.msgtype == HARP_MSG_NOTIFICATION)
                        handle_ntf(p, &e);
                }
                spins++;
            }
            fetched += nwant;
        }
        /* expand children of everything pending */
        harp_hash next[512];
        struct hash_sink sink = {next, 0, 512, false};
        for (size_t i = 0; i < npend; i++) {
            harp_cbuf enc;
            harp_cbuf_init(&enc);
            if (harp_store_get(&p->store, &pending[i], &enc) == 0) {
                /* parents excluded: bundle closure = current state (see deviced note) */
                harp_obj_foreach_child(enc.buf, enc.len, false, sink_collect, &sink);
            }
            harp_cbuf_free(&enc);
        }
        memcpy(pending, next, sink.n * sizeof *next);
        npend = sink.n;
    }
    if (fetched) printf("      fetched %zu object(s)\n", fetched);
}

/* Collect the local closure of `root` (assumed complete locally). */
static size_t collect_closure(probe *p, const harp_hash *root, harp_hash *out, size_t cap) {
    struct hash_sink sink = {out, 0, cap, true};
    sink_collect(root, &sink);
    size_t head = 0;
    while (head < sink.n) {
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        if (harp_store_get(&p->store, &out[head], &enc) == 0)
            harp_obj_foreach_child(enc.buf, enc.len, false, sink_collect, &sink);
        harp_cbuf_free(&enc);
        head++;
    }
    return sink.n;
}

static uint64_t refset(probe *p, const char *name, const harp_hash *expect /* NULL = unborn */,
                       const harp_hash *newh, bool create) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "state.refset", true);
    harp_cbor_map(&req, create ? 4 : 3);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, name);
    harp_cbor_uint(&req, 1);
    if (expect)
        harp_cbor_bytes(&req, expect->b, HARP_HASH_LEN);
    else
        harp_cbor_null(&req);
    harp_cbor_uint(&req, 2);
    harp_cbor_bytes(&req, newh->b, HARP_HASH_LEN);
    if (create) {
        harp_cbor_uint(&req, 3);
        harp_cbor_uint(&req, 1); /* create-if-unborn */
    }
    harp_env e = request(p, &req, &rsp, false);
    uint64_t gen = 0;
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n, key;
    if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
        key == 0)
        harp_cdec_uint(&b, &gen);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return gen;
}

/* ---------------- commands ---------------- */

static void cmd_identify(probe *p) {
    identity id = do_hello(p);
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&id.param_map_hash, hex);
    hex[16] = 0;
    printf("device:    %s %s\n", id.vendor, id.product);
    printf("serial:    %s\n", id.serial);
    printf("firmware:  %s   engine: %s %s\n", id.fw, id.engine_id, id.engine_ver);
    printf("param-map: %s…   boots: %llu\n", hex, (unsigned long long)id.boot_count);
}

static void cmd_refs(probe *p) {
    do_hello(p);
    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    printf("%zu ref(s):\n", n);
    for (size_t i = 0; i < n; i++) print_ref(&refs[i]);
}

static void cmd_counters(probe *p) {
    do_hello(p);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "diag.counters", false);
    harp_env e = request(p, &req, &rsp, false);
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n;
    if (e.has_body && harp_cdec_map(&b, &n)) {
        for (uint64_t i = 0; i < n; i++) {
            const char *s;
            size_t sl;
            if (!harp_cdec_text(&b, &s, &sl)) break;
            printf("  %.*s = ", (int)sl, s);
            if (harp_cdec_peek(&b) == 0) {
                uint64_t v;
                harp_cdec_uint(&b, &v);
                printf("%llu\n", (unsigned long long)v);
            } else {
                int64_t v;
                harp_cdec_int(&b, &v);
                printf("%lld\n", (long long)v);
            }
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static void cmd_params(probe *p) {
    do_hello(p);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.params", false);
    harp_env e = request(p, &req, &rsp, false);
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n, key, alen;
    if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
        key == 0 && harp_cdec_array(&b, &alen)) {
        for (uint64_t i = 0; i < alen; i++) {
            uint64_t three, id;
            const char *s;
            size_t sl;
            double v;
            if (!harp_cdec_array(&b, &three) || three != 3 || !harp_cdec_uint(&b, &id) ||
                !harp_cdec_text(&b, &s, &sl) || !harp_cdec_float(&b, &v))
                break;
            printf("  [%llu] %-16.*s %.3f\n", (unsigned long long)id, (int)sl, s, v);
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static void cmd_knob(probe *p, uint64_t id, double v) {
    do_hello(p);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.knob", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, id);
    harp_cbor_uint(&req, 1);
    harp_cbor_float(&req, v);
    request(p, &req, &rsp, false);
    printf("knob %llu -> %.3f (device live state is now dirty)\n", (unsigned long long)id, v);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

/* save = Pull (§11.4 action 2) */
static void do_save(probe *p) {
    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    harp_ref live;
    if (!find_ref(refs, n, LIVE_REF, &live) || live.unborn)
        die("device has no live/project state");
    harp_hash head = live.hash;
    if (live.dirty) {
        printf("      live state is dirty -> snapshot-on-demand before pull (§10.4)\n");
        head = remote_snapshot(p, "host pull");
    }
    fetch_closure(p, &head);
    harp_ref expected = {0};
    snprintf(expected.name, sizeof expected.name, "%s", EXPECT_REF);
    expected.unborn = false;
    expected.hash = head;
    expected.generation = live.generation;
    expected.dirty = false;
    if (harp_store_ref_write(&p->store, &expected) != 0) die("cannot write local bundle ref");
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&head, hex);
    hex[12] = 0;
    printf("      project saved: live/project @ %s…\n", hex);
}

/* restore = the §12.2 reopen flow; Push with archive-before-push (§11.4 action 1) */
static void do_restore(probe *p) {
    harp_ref expected;
    if (harp_store_ref_read(&p->store, EXPECT_REF, &expected) != 0 || expected.unborn)
        die("no saved project in local store (run `save` first)");

    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    harp_ref live;
    if (!find_ref(refs, n, LIVE_REF, &live)) die("device has no live/project ref");

    if (!live.unborn && !live.dirty && harp_hash_eq(&live.hash, &expected.hash)) {
        printf("      hash match, not dirty -> SYNCED silently (zero dialogs, §12.2)\n");
        return;
    }
    printf("      mismatch%s -> resolving by Push with archive (§11.4)\n",
           live.dirty ? " + dirty live edits" : "");

    /* 1. archive the device's current state — never overwrite without it */
    harp_hash device_head = live.hash;
    if (live.dirty) device_head = remote_snapshot(p, "pre-push archive");
    char archive_name[HARP_REF_NAME_MAX];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(archive_name, sizeof archive_name, "archive/%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             tm.tm_sec);
    refset(p, archive_name, NULL, &device_head, true);
    printf("      archived device state as %s (O(1): a pointer, §11.4)\n", archive_name);

    /* 2. negotiate objects: have/want keeps re-sync proportional to the diff */
    harp_hash closure[512];
    size_t nclo = collect_closure(p, &expected.hash, closure, 512);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "state.have", true);
    harp_cbor_map(&req, 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_array(&req, nclo);
    for (size_t i = 0; i < nclo; i++) harp_cbor_bytes(&req, closure[i].b, HARP_HASH_LEN);
    harp_env e = request(p, &req, &rsp, false);

    bool missing[512] = {false};
    size_t nmissing = 0;
    uint64_t bn, bkey, balen;
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    if (e.has_body && harp_cdec_map(&b, &bn) && bn >= 1 && harp_cdec_uint(&b, &bkey) &&
        bkey == 0 && harp_cdec_array(&b, &balen) && balen == nclo) {
        for (size_t i = 0; i < nclo; i++) {
            bool have;
            if (!harp_cdec_bool(&b, &have)) break;
            if (!have) {
                missing[i] = true;
                nmissing++;
            }
        }
    }

    /* 3. announce, then transfer the missing objects */
    if (nmissing) {
        uint64_t total = 0;
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        for (size_t i = 0; i < nclo; i++)
            if (missing[i]) {
                harp_cbuf_reset(&enc);
                if (harp_store_get(&p->store, &closure[i], &enc) == 0) total += enc.len;
            }
        req_head(p, &req, "state.send", true);
        harp_cbor_map(&req, 2);
        harp_cbor_uint(&req, 0);
        harp_cbor_array(&req, nmissing);
        for (size_t i = 0; i < nclo; i++)
            if (missing[i]) harp_cbor_bytes(&req, closure[i].b, HARP_HASH_LEN);
        harp_cbor_uint(&req, 1);
        harp_cbor_uint(&req, total);
        request(p, &req, &rsp, false);
        for (size_t i = 0; i < nclo; i++) {
            if (!missing[i]) continue;
            harp_cbuf_reset(&enc);
            if (harp_store_get(&p->store, &closure[i], &enc) != 0)
                die("local object vanished");
            if (enc.len <= p->peer_credit) p->peer_credit -= enc.len;
            if (harp_link_send(p->fd, HARP_STREAM_OBJ, enc.buf, enc.len) != 0)
                die("object send failed");
        }
        harp_cbuf_free(&enc);
        printf("      transferred %zu missing object(s), %zu already on device\n", nmissing,
               nclo - nmissing);
    } else {
        printf("      device already holds all %zu object(s)\n", nclo);
    }

    /* 4. CAS the live ref: expect = post-archive head */
    uint64_t gen = refset(p, LIVE_REF, &device_head, &expected.hash, false);
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&expected.hash, hex);
    hex[12] = 0;
    printf("      live/project -> %s… (gen %llu) — recall complete\n", hex,
           (unsigned long long)gen);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static void cmd_demo(probe *p, const char *addr) {
    (void)addr;
    p->verbose_ntf = true;
    printf("HARP recall walkthrough (spec §12.2 / §11.4) — device: harp-deviced\n\n");

    printf("[1] core.hello — identity, engine, capabilities\n");
    identity id = do_hello(p);
    printf("      %s %s, serial %s, fw %s, engine %s %s\n\n", id.vendor, id.product,
           id.serial, id.fw, id.engine_id, id.engine_ver);

    printf("[2] state.refs — what does the device hold?\n");
    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    for (size_t i = 0; i < n; i++) print_ref(&refs[i]);
    printf("\n[3] \"Save the DAW project\" -> Pull (§11.4)\n");
    do_save(p);

    printf("\n[4] The musician turns knobs on the front panel after the save…\n");
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.knob", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 3);
    harp_cbor_uint(&req, 1);
    harp_cbor_float(&req, 0.83);
    request(p, &req, &rsp, false);
    req_head(p, &req, "x.harp-refdev.knob", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 4);
    harp_cbor_uint(&req, 1);
    harp_cbor_float(&req, 0.61);
    request(p, &req, &rsp, false);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    printf("      Filter Cutoff -> 0.83, Filter Reso -> 0.61\n");
    n = get_refs(p, refs);
    harp_ref live;
    if (find_ref(refs, n, LIVE_REF, &live)) print_ref(&live);

    printf("\n[5] \"Reopen the project\" -> mismatch detected -> Push with archive\n");
    do_restore(p);

    printf("\n[6] Verify: live/project matches the saved hash, clean;\n");
    printf("    the musician's tweaks survive in archive/:\n");
    n = get_refs(p, refs);
    for (size_t i = 0; i < n; i++) print_ref(&refs[i]);

    printf("\nEvery overwrite was preceded by a free snapshot. No dialogs on match,\n");
    printf("no silent loss on mismatch — the founding asymmetry of §11.4.\n");
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    const char *addr = "127.0.0.1:47800";
    const char *store_dir = "./host-store";
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            addr = argv[i + 1], i += 2;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            store_dir = argv[i + 1], i += 2;
        else
            break;
    }
    if (i >= argc) {
        fprintf(stderr,
                "usage: harp-probe [-d HOST:PORT] [-s STOREDIR] "
                "identify|refs|counters|params|knob|save|restore|demo\n");
        return 2;
    }
    const char *cmd = argv[i];

    probe p = {0};
    p.fd = dial(addr);
    harp_link_init(&p.link);
    harp_cbuf_init(&p.msg);
    if (harp_store_open(&p.store, store_dir) != 0) die("cannot open local store");

    if (strcmp(cmd, "identify") == 0)
        cmd_identify(&p);
    else if (strcmp(cmd, "refs") == 0)
        cmd_refs(&p);
    else if (strcmp(cmd, "counters") == 0)
        cmd_counters(&p);
    else if (strcmp(cmd, "params") == 0)
        cmd_params(&p);
    else if (strcmp(cmd, "knob") == 0 && i + 2 < argc) {
        cmd_knob(&p, strtoull(argv[i + 1], NULL, 10), strtod(argv[i + 2], NULL));
    } else if (strcmp(cmd, "save") == 0) {
        do_hello(&p);
        do_save(&p);
    } else if (strcmp(cmd, "restore") == 0) {
        do_hello(&p);
        do_restore(&p);
    } else if (strcmp(cmd, "demo") == 0)
        cmd_demo(&p, addr);
    else {
        fprintf(stderr, "harp-probe: unknown command '%s'\n", cmd);
        return 2;
    }
    close(p.fd);
    return 0;
}
