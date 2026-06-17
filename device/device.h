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
/* P2 (voice pool): bumped 1.0.0 -> 1.1.0. §6.4 names voice allocation as
 * render-affecting — the per-part pool means overlapping notes now ring out on
 * separate voices instead of retriggering one, so a render that contains note
 * overlap (the golden melody's release tails) can differ from the pre-pool
 * engine. The bump tells hosts the render contract moved. param-map-hash is
 * deliberately UNCHANGED (the modulatable capability bits are masked out of the
 * hash input, state.c), so stored automation and recall stay byte-identical. */
#define ENGINE_VERSION "1.1.0"
#define FW_VERSION "0.1.0"
#define CREDIT_GRANT (16u << 20)
#define LIVE_REF "live/project"
#define PARAMS_MEDIA "application/x.harp-refdev.params"

/* ---------------- engine data (engine.c owns the definitions) -------- */

/* Cross-thread scalars are C11 atomics with EXPLICIT orderings; relaxed
 * unless a comment states the pairing. (The old "benign word-sized race"
 * volatiles were pragmatically fine on arm64/x86 but formally UB and
 * TSan-opaque — this device is meant to be the conformance reference.) */

/* P3: g_params is now the GLOBAL param DEFINITIONS table — id/name/steps/
 * labels and the factory default. The VALUE is no longer here; it lives
 * PER PART (part::pval, engine.c) so each of the 16 parts is an independent
 * timbre. `def` is a plain (non-atomic) constant — it's read-only after
 * static init, the cross-thread writes happen on the per-part pval[]. */
typedef struct {
    uint32_t id;
    const char *name;
    uint8_t steps;          /* 0 = continuous; else stepped (§9.3 key 5) */
    const char *const *labels; /* enum labels, count == steps (§9.3 key 9) */
    float def;              /* factory default — every part's pval starts here */
} dev_param;

/* fixed count so sizeof tricks aren't needed across modules; the arp
 * session grows this to 12 (params 9-12) — one place to change */
#define NPARAMS 13
extern dev_param g_params[NPARAMS];

/* Per-part param value access (P3). The atomic value lives in part::pval
 * (engine.c owns the type); these helpers are part-aware. param_value_at /
 * param_index let the engine read p->pval[index-of-id] without re-scanning
 * g_params in every accessor. Cross-thread semantics are UNCHANGED from the
 * old dev_param.value: written by render (ramps), session (loads), panel
 * (knobs); read everywhere; last-write-wins, relaxed — ordering for
 * timestamped changes comes from the event queue. */
int param_index(uint32_t id); /* slot in g_params, or -1 (engine.c) */

/* Cross-module per-part value access (engine.c). state.c (snapshot encode/
 * load), session.c (x.harp-refdev.params), and panel.c reach a part's pval[]
 * only through these — the part struct itself stays private to engine.c.
 * part_idx is 0..NPARTS-1; out-of-range reads return 0 / writes no-op. */
#define NPARTS 16
float engine_part_param_get(int part_idx, uint32_t id);
void engine_part_param_put(int part_idx, uint32_t id, float v);

/* ---------------- per-part OUTPUT METERING (§9.9) ----------------
 *
 * Output parameters (§9.9): readonly meters streamed via echo. The render
 * thread FOLDS peak+RMS for every rendered part AND the summed main mix into
 * process-global atomics; a control-thread pump (session.c) reads them and
 * emits 'param' echoes at the meter rate. These atomics are the ONLY thing the
 * metering adds to the render path — it reads the already-written stereo
 * scratch / main mix and writes here; it never feeds back into the render, so
 * the golden render stays byte-identical (see engine.c engine_meter_fold).
 *
 * Layout: index 0..NPARTS-1 = parts 0..15, index NPARTS = the main mix. Peak is
 * max|sample| over the block; RMS is sqrt(mean(square)) over the block. Both are
 * flushed-to-zero on silence / non-finite input (no denormals or NaN escape).
 *
 * COVERAGE CONTRACT (§9.9): a part is metered for a block iff its output is
 * actually produced that block — part 0 always (it carries the drone), parts
 * 1..15 only while active (held/ringing); an unrendered part's meters decay to
 * the silent floor (the pump emits 0 for it). The main-mix meter always covers
 * the summed stereo mix that the host hears on slots 0/1. */
#define METER_NSLOTS (NPARTS + 1) /* 16 parts + 1 main mix */
#define METER_MAIN_IX NPARTS      /* the main-mix entry */

/* Meter-rate hint (§9.3 key 11 / §9.9 "no more than the meter rate"): the
 * descriptor advertises this and the pump paces itself to it. */
#define METER_RATE_HZ 30u

/* Stable, collision-free id range for the readonly meter params (§9.9). The 13
 * device params use ids 1..13; meters live at 0x1000+ so they CANNOT collide.
 * id = METER_ID_BASE + slot*2 + {0 peak, 1 rms}; slot 0..15 = parts, 16 = main. */
#define METER_ID_BASE 0x1000u
#define METER_ID_PEAK(slot) (METER_ID_BASE + (uint32_t)(slot) * 2u + 0u)
#define METER_ID_RMS(slot) (METER_ID_BASE + (uint32_t)(slot) * 2u + 1u)
#define METER_NPARAMS (METER_NSLOTS * 2) /* one peak + one rms per slot */

/* §9.5 per-voice EXPRESSION mod targets (the MPE pitch/pressure axes). These are
 * mod-event ids — NOT §9.3 params (1..13) and NOT meters (0x1000+) — that a shell
 * sends via a §9.4 mod event (etype 6) to drive a voice's pitch bend (semitones)
 * and loudness (gain). The device routes them to the voice's bend_semis / z_gain
 * fields (engine.c); a normalized param id still routes to the mod[] layer. The
 * SHELLS use these same values (kept in sync by hand, like the §9.3 param ids).
 * Reserved at 0x2000+ so they cannot collide with params or meters. */
#define HARP_MOD_PITCH_BEND 0x2001u /* per-voice pitch, SEMITONES (signed) */
#define HARP_MOD_PRESSURE 0x2002u   /* per-voice loudness, gain (signed, ~0..1) */

/* Render thread writes (engine.c), pump reads (session.c). Relaxed: meters
 * order nothing and a one-block-stale read is musically invisible. */
extern _Atomic float g_meter_peak[METER_NSLOTS];
extern _Atomic float g_meter_rms[METER_NSLOTS];

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
    DEV_EV_TRANSPORT, /* §9.7 anchor: (ts, ppq, tempo) defines musical time */
    DEV_EV_MOD        /* §9.4 non-destructive modulation (etype 6): an additive,
                         signed per-(param[,voice]) offset on the base value.
                         APPLIED (P2) on the per-voice mod[] layer — voice!=0
                         addresses the matching voice, voice==0 is part-wide. */
};

typedef struct {
    uint64_t ts;   /* apply at this stream position (0 = asap) */
    uint8_t kind;
    uint32_t a;    /* note number / param id / transport flags */
    float v;       /* velocity / value / ramp target / tempo BPM */
    uint64_t end;  /* ramp end position */
    double ppq;    /* transport: song position at ts (§9.7 key 4) */
    uint8_t channel; /* multitimbral part = UMP channel (notes) / event body
                        key 5 (param/ramp/mod); 0..15, default 0 (§9.4, §15.2). */
    uint32_t voice;  /* §9.5 per-voice address (event body key 3): packed
                        (group<<12)|(channel<<8)|note, 0 = whole-part (no voice).
                        Appended last so positional initializers default it to 0
                        (whole-part) — single-part / non-per-voice wire unchanged. */
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
    /* requested OUTPUT slots (§6.3 active-slots-out, audio.start key 4): the
     * channel map exposes 34 slots (2 main-mix + 16 stereo per-part pairs,
     * §P2.2). out_slots[0..n_out_slots-1] holds the host-requested slot
     * indices in request order; the render thread emits exactly those channels
     * interleaved in that order. Default (key absent / empty) = {0,1}, the
     * stereo main mix — the P2.1 byte-identical render path. Sized for the full
     * map (34) so any subset/order fits; values are clamped 0..33 on parse. */
    uint8_t out_slots[34];
    uint8_t n_out_slots;

    /* §9.9 meter echo pump: a control-thread emitter that streams readonly
     * meter params via evt_echo_param at METER_RATE_HZ, ONLY while streaming.
     * Owned/managed by session.c (meter_pump_start/stop); never the render
     * thread. `meter_running` is the stop flag (session writes, pump polls);
     * `meter_live` guards join (set true after a successful create). */
    pthread_t meter_thread;
    _Atomic bool meter_running;
    _Atomic bool meter_live;
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
void engine_meters_reset(void); /* §9.9: clear meters to the silent floor */

/* ---------------- state.c ---------------- */

/* RefDev params-blob CODEC (P3 closer): the on-wire multitimbral params blob
 * (§10) factored out as a PURE, self-contained pair — no store I/O, no g_parts
 * touch — so it is unit-testable and fuzzable in isolation. The engine wraps
 * encode with the PARAMS_MEDIA blob header and feeds parse from a part snapshot;
 * these functions only move bytes <-> a float[NPARTS][NPARAMS] grid.
 *
 * encode emits the inner CBOR map { partIdx(0..15 asc) => { paramId(g_params
 * order asc) => f32 value } } for ALL 16 parts — byte-identical to the prior
 * inline encoder, so recall hashes and snapshots are unchanged.
 *
 * parse fills v[]/present[] for the params it finds and returns false on ANY
 * structurally malformed input (bad CBOR / truncation). It accepts both the
 * NEW per-part outer map and the LEGACY flat { id => value } map (-> part 0),
 * discriminated by the type of the first outer value (MAP vs FLOAT). An
 * out-of-range partIdx or unknown paramId is SKIPPED (not fatal on its own);
 * it never writes outside the v[0..NPARTS-1][0..NPARAMS-1] grid and never loops
 * unbounded (every container count is the decoder's bounds-checked map count). */
void refdev_encode_params_blob(const float v[NPARTS][NPARAMS], harp_cbuf *payload);
bool refdev_parse_params_blob(const uint8_t *payload, size_t len,
                              float v[NPARTS][NPARAMS], bool present[NPARTS][NPARAMS]);

int engine_snapshot_objects(device *d, const harp_hash *parent, const char *msg,
                            harp_hash *out);
int engine_load_snapshot(device *d, const harp_hash *snap_h);
/* The FULL descriptor array advertised on evt.params / identity: the 13
 * automatable device params FOLLOWED BY the readonly meter params (§9.9). */
void encode_param_array(harp_cbuf *b);
/* The AUTOMATABLE subset only — the 13 device params, no meters. This is the
 * SOLE input to param-map-hash (§9.3): the hash protects stored automation
 * lanes, and readonly outputs (§9.9) can never be automation targets, so they
 * MUST NOT perturb it. Keeping the hash over this subset is what makes the
 * golden render + per-part recall + identity byte-identical to pre-meter. */
void encode_param_array_automatable(harp_cbuf *b);
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
/* P3: echo carries the part (channel). channel 0 omits the §9.4 channel key
 * (key 5) so part-0 echoes are byte-identical to P2.2; non-zero emits it. */
void evt_echo_param(device *d, uint32_t id, float v, uint8_t channel);
/* §9.9 meter echo pump lifecycle (session.c): start at audio.start, stop at
 * audio.stop / session teardown. start clears the meter atomics so a new
 * stream begins from the silent floor. */
void meter_pump_start(device *d);
void meter_pump_stop(device *d);
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
