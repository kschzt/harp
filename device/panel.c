/* panel.c — the device front panel (split from harp-deviced.c; see device.h).
 *
 * A unix-socket JSON API consumed by the web panel sidecar
 * (web/harp-panel.py): params/refs/counters reads, knob writes (through
 * the same canonical front_panel_set path as the protocol's vendor knob
 * method), snapshot/panic/revert actions. Runs on its own thread; every
 * device mutation takes the same locks as the session thread.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
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

static void panel_json_params(device *d, char *body, size_t sz) {
    size_t off = 0;
    pthread_mutex_lock(&d->state_mu);
    bool dirty = d->live_cache_valid ? d->live_cache.dirty : false;
    if (!d->live_cache_valid) {
        harp_ref r;
        if (harp_store_ref_read(&d->store, LIVE_REF, &r) == 0) dirty = r.dirty;
    }
    pthread_mutex_unlock(&d->state_mu);
    off += (size_t)snprintf(body + off, sz - off,
                            "{\"product\":\"harp-refdev\",\"serial\":\"%s\","
                            "\"dirty\":%s,\"params\":[",
                            d->serial, dirty ? "true" : "false");
    for (size_t i = 0; i < NPARAMS; i++)
        off += (size_t)snprintf(body + off, sz - off,
                                "%s{\"id\":%u,\"name\":\"%s\",\"value\":%.4f}",
                                i ? "," : "", g_params[i].id, g_params[i].name,
                                g_params[i].value);
    snprintf(body + off, sz - off, "]}");
}

struct refs_json {
    device *d;
    char *body;
    size_t sz, off;
    int n;
};

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
    j->off += (size_t)snprintf(j->body + j->off, j->sz - j->off,
                               "%s{\"name\":\"%s\",\"hash\":\"%s\",\"gen\":%llu,"
                               "\"dirty\":%s}",
                               j->n++ ? "," : "", shown.name, hex,
                               (unsigned long long)shown.generation,
                               shown.dirty ? "true" : "false");
}

static void panel_json_refs(device *d, char *body, size_t sz) {
    struct refs_json j = {d, body, sz, 0, 0};
    j.off = (size_t)snprintf(body, sz, "{\"refs\":[");
    harp_store_ref_list(&d->store, panel_refs_cb, &j);
    snprintf(body + j.off, sz - j.off, "]}");
}

static void panel_json_counters(device *d, char *body, size_t sz) {
    snprintf(body, sz,
             "{\"frame_errors\":%llu,\"session_resets\":%llu,"
             "\"audio_overruns\":%llu,\"snapshots\":%llu,\"evq_drops\":%llu,"
             "\"evt_late\":%llu,\"ramp_late\":%llu,\"boot\":%llu,"
             "\"session\":%s,\"streaming\":%s}",
             (unsigned long long)d->frame_errors, (unsigned long long)d->session_resets,
             (unsigned long long)d->audio_overruns, (unsigned long long)d->snapshots_taken,
             (unsigned long long)g_evq_drops, (unsigned long long)g_evt_late,
             (unsigned long long)g_ramp_late, (unsigned long long)d->boot_count,
             d->hello_done ? "true" : "false", d->audio.thread_live ? "true" : "false");
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
    for (size_t i = 0; i < NPARAMS; i++)
        evt_echo_param(d, g_params[i].id, g_params[i].value);
    return true;
}

/* One panel-API connection at a time (the sidecar holds one persistent
 * connection); line in, JSON line out. */
static void panel_serve_conn(device *d, int fd) {
    char buf[512];
    size_t len = 0;
    char body[4096];
    for (;;) {
        ssize_t r = read(fd, buf + len, sizeof buf - 1 - len);
        if (r <= 0) return;
        len += (size_t)r;
        buf[len] = 0;
        char *nl;
        while ((nl = memchr(buf, '\n', len))) {
            *nl = 0;
            if (strcmp(buf, "params") == 0)
                panel_json_params(d, body, sizeof body);
            else if (strcmp(buf, "refs") == 0)
                panel_json_refs(d, body, sizeof body);
            else if (strcmp(buf, "counters") == 0)
                panel_json_counters(d, body, sizeof body);
            else if (strcmp(buf, "snapshot") == 0) {
                /* the front-panel save button (§10.4 snapshot-on-demand) */
                harp_hash snap;
                uint64_t gen;
                if (do_snapshot(d, "front panel", &snap, &gen) == 0) {
                    char hex[2 * HARP_HASH_LEN + 1];
                    harp_hash_hex(&snap, hex);
                    hex[12] = 0;
                    snprintf(body, sizeof body, "{\"ok\":true,\"hash\":\"%s\",\"gen\":%llu}",
                             hex, (unsigned long long)gen);
                } else
                    snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"storage\"}");
            } else if (strcmp(buf, "panic") == 0) {
                g_note = -1; /* same engine path the CC 120/123 handler takes */
                snprintf(body, sizeof body, "{\"ok\":true}");
            } else if (strncmp(buf, "revert ", 7) == 0) {
                if (panel_revert(d, buf + 7))
                    snprintf(body, sizeof body, "{\"ok\":true}");
                else
                    snprintf(body, sizeof body,
                             "{\"ok\":false,\"error\":\"unknown ref or load failed\"}");
            } else if (strncmp(buf, "knob ", 5) == 0) {
                unsigned id = 0;
                double v = -1;
                if (sscanf(buf + 5, "%u %lf", &id, &v) == 2 && v >= 0 && v <= 1 &&
                    front_panel_set(d, id, v))
                    snprintf(body, sizeof body, "{\"ok\":true}");
                else
                    snprintf(body, sizeof body,
                             "{\"ok\":false,\"error\":\"knob <id 1..8> <value 0..1>\"}");
            } else
                snprintf(body, sizeof body, "{\"error\":\"unknown command\"}");
            size_t blen = strlen(body);
            body[blen] = '\n';
            if (!harp_write_all(fd, body, blen + 1)) return;
            size_t consumed = (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, len - consumed);
            len -= consumed;
        }
        if (len >= sizeof buf - 1) return; /* oversized request line */
    }
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
    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        panel_serve_conn(a->d, cfd);
        close(cfd);
    }
    return NULL;
}

