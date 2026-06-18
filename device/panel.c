/* panel.c — the device front panel (split from harp-deviced.c; see device.h).
 *
 * A unix-socket JSON API consumed by the web panel sidecar
 * (web/harp-panel.py): params/refs/counters reads, knob writes (through
 * the same canonical front_panel_set path as the protocol's vendor knob
 * method), snapshot/panic/revert actions. Runs on its own thread; every
 * device mutation takes the same locks as the session thread.
 */
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "device.h"

/* ---------------- panel API (device frontend boundary) ----------------
 *
 * Line-oriented JSON over a Unix socket: the single sanctioned boundary
 * for every frontend — web sidecar, future GPIO encoders, MIDI control
 * surfaces. Frontends reach the engine ONLY through front_panel_set
 * (the device-side mirror of the spec's no-SysEx-side-doors rule):
 * edits here dirty the live ref and echo as §9.4 events, exactly like
 * hands on hardware.
 *
 *   request line             response line (JSON)
 *   params                   {"product","serial","dirty","params":[..]}
 *   knob <id> <value01>      {"ok":true} | {"ok":false,...}
 *   refs                     {"refs":[{"name","hash","gen","dirty"},..]}
 *   counters                 {"frame_errors",...}
 *
 * The web server lives in a sidecar (web/harp-panel.py) — HTTP robustness
 * is a solved problem there; this stays dependency-free C.
 */

static void panel_json_params(device *d, int part, char *body, size_t sz) {
    size_t off = 0;
    pthread_mutex_lock(&d->state_mu);
    bool dirty = d->live_cache_valid ? d->live_cache.dirty : false;
    if (!d->live_cache_valid) {
        harp_ref r;
        if (harp_store_ref_read(&d->store, LIVE_REF, &r) == 0) dirty = r.dirty;
    }
    pthread_mutex_unlock(&d->state_mu);
    if (part < 0 || part >= NPARTS) part = 0;
    /* Values are PER PART (engine.c); `part` selects which (a frontend's part
     * selector). `dirty` is the live ref's, shared across parts. The param NAMES
     * and ids are the same for every part — only the values differ. */
    off += (size_t)snprintf(body + off, sz - off,
                            "{\"product\":\"harp-refdev\",\"serial\":\"%s\",\"part\":%d,"
                            "\"dirty\":%s,\"params\":[",
                            d->serial, part, dirty ? "true" : "false");
    for (size_t i = 0; i < NPARAMS; i++)
        off += (size_t)snprintf(body + off, sz - off,
                                "%s{\"id\":%u,\"name\":\"%s\",\"value\":%.4f}",
                                i ? "," : "", g_params[i].id, g_params[i].name,
                                engine_part_param_get(part, g_params[i].id));
    snprintf(body + off, sz - off, "]}");
}

/* The refs array grows without bound — a desk board accumulates an
 * archive/ snapshot per session. The old fixed 4096-byte stack buffer
 * overflowed once ~48 refs no longer fit: snprintf returns what it WOULD
 * have written, so `off` ran past `sz`, `sz - off` underflowed to a huge
 * size_t, and the next entry wrote past the buffer and smashed the stack
 * (the SIGSEGV on the desk board). So refs build into a growable heap
 * buffer instead, returning the whole list rather than truncating it. */
struct refs_json {
    device *d;
    char *buf;
    size_t cap, off;
    int n;
    bool oom;
};

/* Append printf-style, growing the buffer as needed. Underflow-proof:
 * `off` never advances past a write that didn't fit; on OOM it latches
 * `oom` and stops touching memory. */
static void refs_appendf(struct refs_json *j, const char *fmt, ...) {
    if (j->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int r = vsnprintf(j->buf + j->off, j->cap - j->off, fmt, ap);
        va_end(ap);
        if (r < 0) {
            j->oom = true;
            return;
        }
        if ((size_t)r < j->cap - j->off) { /* fit, with room for the NUL */
            j->off += (size_t)r;
            return;
        }
        size_t need = j->off + (size_t)r + 1;
        size_t ncap = j->cap ? j->cap : 1024;
        while (ncap < need) ncap *= 2;
        char *nb = realloc(j->buf, ncap);
        if (!nb) {
            j->oom = true;
            return;
        }
        j->buf = nb;
        j->cap = ncap;
        /* loop: re-format into the grown buffer */
    }
}

static void panel_refs_cb(const harp_ref *r, void *ud) {
    struct refs_json *j = ud;
    harp_ref shown = *r;
    /* the live ref may be ahead on the coalesced in-memory cache */
    pthread_mutex_lock(&j->d->state_mu);
    if (j->d->live_cache_valid && strcmp(r->name, LIVE_REF) == 0)
        shown = j->d->live_cache;
    pthread_mutex_unlock(&j->d->state_mu);
    char hex[2 * HARP_HASH_LEN + 1] = "";
    if (!shown.unborn) {
        harp_hash_hex(&shown.hash, hex);
        hex[12] = 0;
    }
    refs_appendf(j,
                 "%s{\"name\":\"%s\",\"hash\":\"%s\",\"gen\":%llu,\"dirty\":%s}",
                 j->n++ ? "," : "", shown.name, hex,
                 (unsigned long long)shown.generation, shown.dirty ? "true" : "false");
}

/* Returns a heap JSON string (caller frees), or NULL on allocation failure. */
static char *panel_json_refs(device *d) {
    struct refs_json j = {d, NULL, 0, 0, 0, false};
    refs_appendf(&j, "{\"refs\":[");
    harp_store_ref_list(&d->store, panel_refs_cb, &j);
    refs_appendf(&j, "]}");
    if (j.oom) {
        free(j.buf);
        return NULL;
    }
    return j.buf;
}

static void panel_json_counters(device *d, char *body, size_t sz) {
    snprintf(body, sz,
             "{\"frame_errors\":%llu,\"session_resets\":%llu,"
             "\"audio_overruns\":%llu,\"snapshots\":%llu,\"evq_drops\":%llu,"
             "\"evt_late\":%llu,\"ramp_late\":%llu,"
             "\"fence_waits\":%llu,\"fence_timeouts\":%llu,\"boot\":%llu,"
             "\"session\":%s,\"streaming\":%s}",
             (unsigned long long)CTR_GET(d->frame_errors),
             (unsigned long long)CTR_GET(d->session_resets),
             (unsigned long long)CTR_GET(d->audio_overruns),
             (unsigned long long)CTR_GET(d->snapshots_taken),
             (unsigned long long)CTR_GET(g_evq_drops),
             (unsigned long long)CTR_GET(g_evt_late),
             (unsigned long long)CTR_GET(g_ramp_late),
             (unsigned long long)CTR_GET(g_fence_waits),
             (unsigned long long)CTR_GET(g_fence_timeouts),
             (unsigned long long)d->boot_count,
             atomic_load_explicit(&d->hello_done, memory_order_acquire) ? "true" : "false",
             atomic_load_explicit(&d->audio.thread_live, memory_order_relaxed) ? "true"
                                                                               : "false");
}

/* Front-panel patch load: point live/project at a stored ref's state (the
 * §11.4 asymmetry applies to front panels too — snapshot-if-dirty first, so
 * reverting never loses anything). Echoes all params so an attached DAW's
 * knobs follow. */
static bool panel_revert(device *d, const char *refname) {
    harp_ref src;
    if (harp_store_ref_read(&d->store, refname, &src) != 0 || src.unborn) return false;
    live_cache_flush(d);
    harp_ref live;
    if (harp_store_ref_read(&d->store, LIVE_REF, &live) != 0) return false;
    if (live.dirty) {
        harp_hash snap;
        if (do_snapshot(d, "pre-revert", &snap, NULL) != 0) return false;
    }
    if (engine_load_snapshot(d, &src.hash) != 0) return false;
    live_cache_flush(d);
    harp_store_ref_read(&d->store, LIVE_REF, &live);
    live.unborn = false;
    live.hash = src.hash;
    live.generation++;
    live.dirty = false;
    if (harp_store_ref_write(&d->store, &live) != 0) return false;
    ntf_state_changed(d, &live);
    /* P3: echo PART 0's restored values (channel 0) so an attached DAW's knobs
     * follow — the panel dimension is part 0 for now (see panel_json_params). */
    for (size_t i = 0; i < NPARAMS; i++)
        evt_echo_param(d, g_params[i].id, engine_part_param_get(0, g_params[i].id), 0);
    return true;
}

/* Phase-2 recall-reconcile mailbox (§11.4 four safe actions, surfaced on the front
 * panel). A shell that finds live/project differing from its saved Recall Bundle
 * POSTs an offer here; a panel frontend (the Electra sidecar) reads it, force-opens
 * its Reconcile screen, and POSTs the user's chosen action back for the shell to
 * execute. Reached from TWO threads now — the panel poll() thread (the verbs below)
 * AND the session/control thread (session.c's x.harp.reconcile.* methods) — so every
 * access goes through g_reconcile_mu via the accessors. */
static struct {
    int pending;     /* an offer awaits a choice */
    char expect[16]; /* short hex: the project's expected live/project hash */
    char live[16];   /* short hex: the device's current live/project hash */
    int dirty;       /* is the live ref dirty (unsaved front-panel edits)? */
    int choice;      /* -1 none; 0 push / 1 pull / 2 read-only / 3 duplicate */
} g_reconcile = {0, "", "", 0, -1};
static pthread_mutex_t g_reconcile_mu = PTHREAD_MUTEX_INITIALIZER;

/* Mailbox accessors (locked). Shared by the panel verbs (below) and session.c. */
void reconcile_post_offer(const char *expect, const char *live, int dirty) {
    pthread_mutex_lock(&g_reconcile_mu);
    snprintf(g_reconcile.expect, sizeof g_reconcile.expect, "%s", expect ? expect : "");
    snprintf(g_reconcile.live, sizeof g_reconcile.live, "%s", live ? live : "");
    g_reconcile.dirty = dirty ? 1 : 0;
    g_reconcile.choice = -1;
    g_reconcile.pending = 1;
    pthread_mutex_unlock(&g_reconcile_mu);
}
void reconcile_read(int *pending, char *expect12, char *live12, int *dirty, int *choice) {
    pthread_mutex_lock(&g_reconcile_mu);
    if (pending) *pending = g_reconcile.pending;
    if (expect12) snprintf(expect12, 16, "%s", g_reconcile.expect);
    if (live12) snprintf(live12, 16, "%s", g_reconcile.live);
    if (dirty) *dirty = g_reconcile.dirty;
    if (choice) *choice = g_reconcile.choice;
    pthread_mutex_unlock(&g_reconcile_mu);
}
int reconcile_set_choice(int n) {
    if (n < 0 || n > 3) return 0;
    pthread_mutex_lock(&g_reconcile_mu);
    g_reconcile.choice = n;
    g_reconcile.pending = 0;
    pthread_mutex_unlock(&g_reconcile_mu);
    return 1;
}

/* Handle ONE panel command line; returns the response string (in `body`, or a
 * heap buffer returned via *dyn that the caller frees). Pure per-line dispatch,
 * so the poll() loop below can serve several frontends from this one thread. */
static const char *panel_handle_line(device *d, const char *line, char *body,
                                      size_t bodysz, char **dyn) {
    *dyn = NULL;
    const char *out = body;
    if (strcmp(line, "params") == 0)
        panel_json_params(d, 0, body, bodysz);          /* part 0 (back-compat) */
    else if (strncmp(line, "params ", 7) == 0)
        panel_json_params(d, atoi(line + 7), body, bodysz); /* "params <part>" */
    else if (strcmp(line, "parts") == 0)
        snprintf(body, bodysz, "{\"parts\":%d}", NPARTS);
    else if (strcmp(line, "refs") == 0) {
        *dyn = panel_json_refs(d);
        if (*dyn) out = *dyn;
        else snprintf(body, bodysz, "{\"error\":\"out of memory\"}");
    } else if (strcmp(line, "counters") == 0)
        panel_json_counters(d, body, bodysz);
    else if (strcmp(line, "snapshot") == 0) {
        /* the front-panel save button (§10.4 snapshot-on-demand) */
        harp_hash snap;
        uint64_t gen;
        if (do_snapshot(d, "front panel", &snap, &gen) == 0) {
            char hex[2 * HARP_HASH_LEN + 1];
            harp_hash_hex(&snap, hex);
            hex[12] = 0;
            snprintf(body, bodysz, "{\"ok\":true,\"hash\":\"%s\",\"gen\":%llu}", hex,
                     (unsigned long long)gen);
        } else
            snprintf(body, bodysz, "{\"ok\":false,\"error\":\"storage\"}");
    } else if (strcmp(line, "panic") == 0) {
        engine_all_notes_off(); /* same path the CC 120/123 handler takes */
        snprintf(body, bodysz, "{\"ok\":true}");
    } else if (strncmp(line, "revert ", 7) == 0) {
        if (panel_revert(d, line + 7))
            snprintf(body, bodysz, "{\"ok\":true}");
        else
            snprintf(body, bodysz, "{\"ok\":false,\"error\":\"unknown ref or load failed\"}");
    } else if (strncmp(line, "knob ", 5) == 0) {
        /* "knob <id> <v>" -> part 0 (back-compat); "knob <part> <id> <v>" -> part.
         * Parse as doubles so the float <v> isn't mis-split (counting matched
         * fields disambiguates 2 vs 3 cleanly). */
        double a = -1, b = -1, c = -1;
        int n = sscanf(line + 5, "%lf %lf %lf", &a, &b, &c);
        int ok = 0;
        if (n == 3 && c >= 0 && c <= 1)
            ok = front_panel_set_part(d, (int)a, (uint32_t)b, c);
        else if (n == 2 && b >= 0 && b <= 1)
            ok = front_panel_set(d, (uint32_t)a, b);
        snprintf(body, bodysz, ok ? "{\"ok\":true}"
                 : "{\"ok\":false,\"error\":\"knob [part] <id 1..8> <value 0..1>\"}");
    } else if (strncmp(line, "reconcile-offer ", 16) == 0) {
        /* a shell (or a test) posts a §11.4 conflict: expected vs live hash + dirty */
        char e[16] = "", l[16] = "";
        int dty = 0;
        if (sscanf(line + 16, "%15s %15s %d", e, l, &dty) >= 2) {
            reconcile_post_offer(e, l, dty);
            snprintf(body, bodysz, "{\"ok\":true}");
        } else
            snprintf(body, bodysz,
                     "{\"ok\":false,\"error\":\"reconcile-offer <expect> <live> <dirty>\"}");
    } else if (strcmp(line, "reconcile-get") == 0) {
        int p, dty, ch;
        char ex[16], lv[16];
        reconcile_read(&p, ex, lv, &dty, &ch);
        snprintf(body, bodysz,
                 "{\"pending\":%s,\"expect\":\"%s\",\"live\":\"%s\",\"dirty\":%s,\"choice\":%d}",
                 p ? "true" : "false", ex, lv, dty ? "true" : "false", ch);
    } else if (strncmp(line, "reconcile-choose ", 17) == 0) {
        /* the front panel reports the user's pick; the shell reads it + executes */
        int n = atoi(line + 17);
        if (reconcile_set_choice(n))
            snprintf(body, bodysz, "{\"ok\":true,\"choice\":%d}", n);
        else
            snprintf(body, bodysz, "{\"ok\":false,\"error\":\"reconcile-choose <0..3>\"}");
    } else
        snprintf(body, bodysz, "{\"error\":\"unknown command\"}");
    return out;
}

/* A connected frontend: its fd + the partial request line being accumulated. */
#define PANEL_MAX_CLIENTS 8
struct panel_client {
    int fd;
    char buf[512];
    size_t len;
};

/* Drain whatever just arrived on client `c`: split into lines, handle each, write
 * its JSON response. false => the connection should close (EOF/error/oversize). */
static bool panel_client_pump(device *d, struct panel_client *c) {
    ssize_t r = read(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len);
    if (r <= 0) return false;
    c->len += (size_t)r;
    c->buf[c->len] = 0;
    char *nl;
    while ((nl = memchr(c->buf, '\n', c->len))) {
        *nl = 0;
        char body[4096];
        char *dyn = NULL;
        const char *resp = panel_handle_line(d, c->buf, body, sizeof body, &dyn);
        bool ok = harp_write_all(c->fd, resp, strlen(resp)) && harp_write_all(c->fd, "\n", 1);
        free(dyn);
        if (!ok) return false;
        size_t consumed = (size_t)(nl + 1 - c->buf);
        memmove(c->buf, nl + 1, c->len - consumed);
        c->len -= consumed;
    }
    return c->len < sizeof c->buf - 1; /* oversized request line => close */
}

void *panel_main(void *arg) {
    struct panel_args *a = arg;
    unlink(a->path);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", a->path);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(sfd, 4) != 0) {
        fprintf(stderr, "harp-deviced: panel api: cannot listen on %s\n", a->path);
        return NULL;
    }
    chmod(a->path, 0666); /* sidecar runs unprivileged */
    fprintf(stderr, "harp-deviced: panel api on %s\n", a->path);

    /* Single-threaded poll() multiplexing: several frontends (the web sidecar AND
     * the Electra MIDI sidecar, say) connect at once, but every command is still
     * handled on THIS one thread — front_panel_set/snapshot/revert keep their
     * single-caller invariant with no new locks. */
    struct panel_client cl[PANEL_MAX_CLIENTS];
    for (int i = 0; i < PANEL_MAX_CLIENTS; i++) cl[i].fd = -1;
    for (;;) {
        struct pollfd pfd[1 + PANEL_MAX_CLIENTS];
        int slot_of[1 + PANEL_MAX_CLIENTS]; /* pfd index -> client slot */
        pfd[0].fd = sfd;
        pfd[0].events = POLLIN;
        int n = 1;
        for (int i = 0; i < PANEL_MAX_CLIENTS; i++) {
            if (cl[i].fd < 0) continue;
            pfd[n].fd = cl[i].fd;
            pfd[n].events = POLLIN;
            slot_of[n] = i;
            n++;
        }
        if (poll(pfd, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* service existing clients first, so a close frees a slot for the accept */
        for (int k = 1; k < n; k++) {
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            struct panel_client *c = &cl[slot_of[k]];
            if (!panel_client_pump(a->d, c)) {
                close(c->fd);
                c->fd = -1;
            }
        }
        if (pfd[0].revents & POLLIN) {
            int cfd = accept(sfd, NULL, NULL);
            if (cfd >= 0) {
                int slot = -1;
                for (int i = 0; i < PANEL_MAX_CLIENTS; i++)
                    if (cl[i].fd < 0) { slot = i; break; }
                if (slot < 0) {
                    close(cfd); /* table full (frontends are few) — refuse */
                } else {
                    cl[slot].fd = cfd;
                    cl[slot].len = 0;
                }
            }
        }
    }
    return NULL;
}

