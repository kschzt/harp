/* device.h — shared internals of harp-deviced.
 *
 * The daemon was decomposed from one 2,137-line file into modules along
 * its natural thread boundaries:
 *
 *   engine.c   the synth: params, note voice, event queue, ramps, the
 *              render/audio threads (render-thread-owned state lives here)
 *   state.c    engine <-> content-addressed objects: snapshot/load,
 *              param-map hash, live-ref coalescing, refset closure walk
 *   session.c  the protocol: wire helpers, identity, all ctl/obj/evt
 *              handlers, the per-connection session loop
 *   panel.c    the front panel: unix-socket JSON API for the web UI
 *   harp-deviced.c   main(): transports (TCP / FunctionFS), g_dev
 *
 * This header is the contract between them. Global names are unchanged
 * from the monolith (the split was mechanical; behavior is oracle-checked
 * by byte-identical renders). Encapsulating the engine globals behind
 * accessors is future work — see docs/debt.md.
 */
#ifndef HARP_DEVICED_DEVICE_H
#define HARP_DEVICED_DEVICE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "harp/audio.h"
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

/* ---------------- engine data (engine.c owns the definitions) -------- */

/* Cross-thread scalars are C11 atomics with EXPLICIT orderings; relaxed
 * unless a comment states the pairing. (The old "benign word-sized race"
 * volatiles were pragmatically fine on arm64/x86 but formally UB and
 * TSan-opaque — this device is meant to be the conformance reference.) */

typedef struct {
    uint32_t id;
    const char *name;
    uint8_t steps;          /* 0 = continuous; else stepped (§9.3 key 5) */
    const char *const *labels; /* enum labels, count == steps (§9.3 key 9) */
    _Atomic float value; /* written by render (ramps), session (loads),
                            panel (knobs); read everywhere. Last-write-wins
                            is the intended semantic; relaxed — ordering for
                            timestamped changes comes from the event queue */
} dev_param;

static inline float param_get(const dev_param *p) {
    return atomic_load_explicit(&p->value, memory_order_relaxed);
}
static inline void param_put(dev_param *p, float v) {
    atomic_store_explicit(&p->value, v, memory_order_relaxed);
}

/* fixed count so sizeof tricks aren't needed across modules; the arp
 * session grows this to 12 (params 9-12) — one place to change */
#define NPARAMS 12
extern dev_param g_params[NPARAMS];

/* Live performance note state is PRIVATE to engine.c (mono, last-note
 * priority; notes are events, not patch state — they never touch the
 * dirty flag). Other threads reach it only through the panic-grade
 * operations below, which must work even when the event queue cannot. */
void engine_all_notes_off(void);          /* CC 120/123 / panel panic */
void engine_note_off_if(uint32_t note);   /* overflow escalation path */

/* ---- timestamped event queue (§9.2): session thread pushes, render
 * thread pops at exact stream positions. ts is SSI (host-paced) or MSC
 * (free-running); ts == 0 means "now". ---- */
enum {
    DEV_EV_NOTE_ON,
    DEV_EV_NOTE_OFF,
    DEV_EV_PARAM_SET,
    DEV_EV_RAMP,
    DEV_EV_ALL_OFF,
    DEV_EV_TRANSPORT /* §9.7 anchor: (ts, ppq, tempo) defines musical time */
};

typedef struct {
    uint64_t ts;   /* apply at this stream position (0 = asap) */
    uint8_t kind;
    uint32_t a;    /* note number / param id / transport flags */
    float v;       /* velocity / value / ramp target / tempo BPM */
    uint64_t end;  /* ramp end position */
    double ppq;    /* transport: song position at ts (§9.7 key 4) */
} dev_event;

#define DEV_EVQ_CAP 256
/* the queue itself (storage, count, mutex) and the per-param ramp state
 * are private to engine.c; sessions interact through these: */
bool evq_full(void); /* the never-drop-a-note-off escalation check */

extern _Atomic int g_touch_pending;       /* dirty-flag work deferred off the
                                             render thread; set release, read
                                             acquire (pairs with param_put) */
extern _Atomic uint64_t g_evq_drops;      /* ring full — counted, never silent (§14.1) */
extern _Atomic uint64_t g_evt_late;       /* notes/sets applied past ts (§14.2) — keep ZERO */
extern _Atomic uint64_t g_ramp_late;      /* ramps past their END deadline; budgeted */

/* event fence (§8.3.1): the render loop may not pass a fenced pacing
 * frame until the session thread has consumed this many evt messages.
 * fetch_add release in the session loop, load acquire in the render
 * loop (the pairing that makes queued events visible); reset with the
 * stream. */
extern _Atomic uint32_t g_evt_consumed;
extern _Atomic uint64_t g_fence_waits;    /* fences that actually waited */
extern _Atomic uint64_t g_fence_timeouts; /* waits that hit the bound (events
                                             still in flight after 5 ms — the
                                             range may render with a late
                                             event; evt_late will say) */

/* counter convenience: relaxed increments/reads (counters order nothing) */
#define CTR_INC(c) atomic_fetch_add_explicit(&(c), 1, memory_order_relaxed)
#define CTR_GET(c) atomic_load_explicit(&(c), memory_order_relaxed)

/* ---------------- audio plane + device ---------------- */

/* per-frame sample bound: sizes the render loops' stack buffers (engine.c)
 * and caps what audio.start accepts (session.c) — must agree */
#define AUDIO_MAX_NSAMPLES 1024

/* audio plane state (§8): one D→H stream.
 * mode 0: free-running, paced by the device clock, MSC timestamps.
 * mode 1: host-paced — no timers; renders exactly the SSI ranges the host
 *         supplies in pacing frames on the audio OUT endpoint. */
typedef struct {
    pthread_t thread;
    _Atomic bool running;    /* session stores false to stop; render polls */
    _Atomic bool thread_live; /* session writes; panel reads for display */
    int fd;     /* audio IN endpoint: device -> host */
    int out_fd; /* audio OUT endpoint: host -> device (pacing, mode 1) */
    uint32_t mode, rate, nsamples, epoch;
    uint64_t reanchors;
} audio_state;

typedef struct {
    harp_store store;
    char serial[64];
    harp_hash param_map_hash;
    uint64_t boot_count;

    harp_io *io; /* NULL when no session transport is attached. Written by
                    the session loop and read by panel-thread echo paths:
                    ALL access (set, check, send) happens under send_mu —
                    checking outside it is a use-after-teardown. */
    harp_link link;
    _Atomic bool hello_done; /* session writes; panel reads (echo gate) */
    bool closing;            /* session thread only */
    uint64_t peer_credit;   /* bytes we may still send on obj */
    uint64_t granted;       /* unconsumed credit we granted the peer */

    audio_state audio;

    /* live-ref write coalescing: only the clean->dirty transition must hit
     * storage synchronously (it's the loss-safety edge); generation bumps
     * during continuous editing stay in memory (fsync per knob tick starves
     * the audio path on SD cards). Flushed before any reader (§10.3:
     * notification MAY be coalesced; terminal state must be visible). */
    harp_ref live_cache;
    bool live_cache_valid;
    uint64_t last_live_ntf_ms;

    /* knob sources are multi-threaded now (session loop, HTTP panel):
     * send_mu serializes link writes; state_mu guards the live-ref cache */
    pthread_mutex_t send_mu, state_mu;

    /* counters (§14.2): frame_errors has render + session writers;
     * snapshots_taken has session + panel writers; all read cross-thread */
    _Atomic uint64_t frame_errors, session_resets, snapshots_taken, audio_overruns;
} device;

extern device g_dev;

static inline uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/* ---------------- engine.c ---------------- */
void evq_push(dev_event ev);
void evq_reset_for_new_stream(void);
void *audio_thread(void *arg);
void audio_stop(device *d);

/* ---------------- state.c ---------------- */
int engine_snapshot_objects(device *d, const harp_hash *parent, const char *msg,
                            harp_hash *out);
int engine_load_snapshot(device *d, const harp_hash *snap_h);
void encode_param_array(harp_cbuf *b);
void compute_param_map_hash(device *d);
void live_ref_touch(device *d, bool dirty);
void live_cache_flush(device *d);
bool front_panel_set(device *d, uint32_t id, double v);
int do_snapshot(device *d, const char *msg, harp_hash *out, uint64_t *out_gen);

/* Closure check for refset (§11.3): root snapshot -> tree -> children
 * present. Parent ancestry deliberately not required (unbounded history
 * in every bundle otherwise — see harp-deviced.c header note). */
struct closure_ctx {
    device *d;
    bool complete;
    int depth;
};
void closure_walk(struct closure_ctx *ctx, const harp_hash *h);

/* ---------------- session.c ---------------- */
int send_ctl(device *d, const harp_cbuf *msg);
void evt_echo_param(device *d, uint32_t id, float v);
void ntf_state_changed(device *d, const harp_ref *r);
void grant_credit(device *d);
void send_error(device *d, uint64_t rid, const char *method, const char *code,
                const char *detail);
void harp_deviced_run_session(device *d, harp_io *io);

/* ---------------- panel.c ---------------- */
struct panel_args {
    device *d;
    const char *path;
};
void *panel_main(void *arg); /* arg: struct panel_args*, must outlive the thread */

#endif /* HARP_DEVICED_DEVICE_H */
