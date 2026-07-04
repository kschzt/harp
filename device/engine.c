/* engine.c — the refdev synth engine (split from harp-deviced.c; see device.h).
 *
 * Owns everything the render thread touches: the parameter bank, the per-part
 * VOICE POOL (P2: NVOICES of real polyphony, deterministically allocated), the
 * timestamped event queue (§9.2), per-param ramps (§9.4) and per-voice
 * modulation (§9.4/§9.5), and the audio thread itself (free-running and
 * host-paced loops, §8).
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h> /* the host-paced pacing channel is a Winsock SOCKET (recv/send) */
#else
#  include <sys/socket.h> /* shutdown()/SHUT_RDWR to wake the parked host-paced recv on stop */
#endif

#include "device.h"
#include "fence_wait.h" /* §8.3.1 pure fence-wait predicate (host-unit-tested) */
#include "harp/plat.h"  /* harp_now_ns: the §8.3.1 real-time fence bound */

#include "arp_select.h"  /* pure §9.7 arp note-selection (host-unit-tested) */
#include "voice_alloc.h" /* pure §9.5 voice-allocation policy (host-unit-tested) */
#include "evq_mod.h"     /* pure §9.4/§9.5 mod routing + addressing (host-unit-tested) */

static const char *const ARP_MODES[] = {"Off", "Up", "Down", "Up-Down", "As Played"};
static const char *const ARP_DIVS[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const ARP_OCTS[] = {"1", "2", "3", "4"};

/* division lengths in PPQ (quarter notes), indexed like ARP_DIVS */
static const double ARP_DIV_PPQ[] = {1.0, 0.5, 1.0 / 3, 0.25, 1.0 / 6, 0.125};

/* P3: g_params is the GLOBAL param DEFINITIONS table (id/name/steps/labels/
 * default). The per-part VALUE lives in part::pval (below); the last field of
 * each row is the factory default that every part's pval[] starts from.
 *
 * The Drone Mix param (id 7) was REMOVED with the drone at engine 2.0.0. It
 * first left a GAP in the id space (1..6, 8..13); at 2.1.0 the ids were
 * RENUMBERED CONTIGUOUS (1..12) — no hole — so the shells can keep their simple
 * id<->index map (a contiguous tag space). param_index() resolves by id, not
 * slot, so the engine reads are unaffected by the renumber as long as the
 * render-path id constants below move with the table. The RENUMBER changes the
 * param-map-hash (a new §9.3 identity — what bumped the engine MINOR to 2.1.0)
 * but NOT the audio: each value still lands on the SAME named param, only its id
 * shifted (Master Level 8->7, the arp 9..12 -> 8..11, Glide 13->12). */
dev_param g_params[NPARAMS] = {
    {1, "Osc Pitch", 0, NULL, 0.5f},    {2, "Osc Shape", 0, NULL, 0.5f},
    {3, "Filter Cutoff", 0, NULL, 0.5f}, {4, "Filter Reso", 0, NULL, 0.5f},
    {5, "Env Attack", 0, NULL, 0.5f},   {6, "Env Release", 0, NULL, 0.5f},
    {7, "Master Level", 0, NULL, 0.5f}, /* was id 8 (the drone's old id 7 is gone) */
    /* the arp (params 8-11, renumbered from 9-12). Mode defaults OFF so pre-arp
     * behavior is preserved. */
    {8, "Arp Mode", 5, ARP_MODES, 0.0f},
    {9, "Arp Division", 6, ARP_DIVS, 0.6f}, /* index 3 = 1/16 */
    {10, "Arp Gate", 0, NULL, 0.5f},
    {11, "Arp Octaves", 4, ARP_OCTS, 0.0f},
    /* 0 = no portamento (legato snaps too — what an arp wants); else
     * glide tau 1..512 ms, exp-mapped. The old behavior was a fixed
     * 12 ms tau that audibly never arrived at fast arp rates. (was id 13) */
    {12, "Glide", 0, NULL, 0.0f},
};

/* Compile-time mirror of g_params[].def, in id order — used to initialise
 * every part's pval[] at static-init time (no runtime init, no main() change,
 * no ordering hazard before the factory snapshot). MUST stay in lockstep with
 * the table above; compute_param_map_hash() asserts it at boot (state.c). */
#define PVAL_DEFAULTS \
    {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f, 0.6f, 0.5f, 0.0f, 0.0f}

/* slot of a param id in g_params, or -1. The per-part value is p->pval[idx]. */
int param_index(uint32_t id) {
    for (size_t i = 0; i < NPARAMS; i++)
        if (g_params[i].id == id) return (int)i;
    return -1;
}

/* ---------------- per-part ramp state (§9.4) ----------------
 *
 * P3: ramps are PER PART now (one set per part, embedded in `part` below) so
 * automation on part k touches only part k's values. Defined here so the
 * 'part' struct can contain ramps[NPARAMS] by value. Render-thread-only after
 * activation. */
typedef struct {
    bool active;
    uint64_t start, end;
    float start_val, target;
} dev_ramp;

/* ---------------- per-part types (P2.0) ----------------
 *
 * The synth voice, the note state, and the arpeggiator are all PER PART;
 * their type defs are relocated here (verbatim, comments intact) so the
 * 'part' struct can contain them by value. P3 also moves the param VALUES and
 * ramps into the part (pval[]/ramps[]) — the event queue stays global, events
 * route to a part only on APPLICATION (evq_apply_due). */

/* A small stereo note synth: blended sine/saw oscillator (R detuned for
 * width) through a Chamberlin state-variable lowpass, envelope-gated per note.
 * Every voice parameter is one of the recallable params — the point is recall
 * you can hear.
 *
 * P2 (voice pool): the NOTE IDENTITY moved INTO the voice (was per-part mono).
 * Each part owns NVOICES of these and allocates one per sounding note for real
 * polyphony. The DSP fields below the divider are VERBATIM from the mono voice
 * — a single active voice rendered with accumulate=false is bit-for-bit the old
 * mono kernel (only the storage of note/note_vel/note_seq moved here). */
typedef struct {
    /* ---- note identity (P2: was part::note/note_vel/note_seq, mono) ---- */
    int note;          /* MIDI note, -1 = released (env decays / idle) */
    float note_vel;    /* velocity / env target while gated */
    uint32_t note_seq; /* retrig edge counter (a fresh note bumps it) */
    uint32_t voice_id; /* §9.5 packed key of the allocating note-on (mod target) */
    uint64_t alloc_seq; /* unique, monotone per part: the deterministic steal age */
    bool active;       /* allocated (sounding or ringing); false = free slot */
    /* §9.4 per-voice modulation offset layer: an additive signed offset on the
     * base param, applied only when a mod event has addressed this voice. While
     * has_mod is false (ALWAYS for the golden) the kernel reads the UNCHANGED
     * base param_value() — identical codegen, identical bytes. */
    float mod[NPARAMS];
    bool has_mod;
    /* §9.5 per-voice EXPRESSION (MPE): a pitch bend in SEMITONES on the note
     * frequency (X axis) and a loudness gain folded into the envelope (Z axis).
     * SEPARATE from mod[] because neither is a normalized §9.3 param — pitch is
     * semitones (unclamped), loudness is a multiplicative gain. Both gated at
     * their use site on != 0, so a voice with no expression renders the LITERAL
     * golden path bit-for-bit, even when has_mod is set by a (cutoff) param mod. */
    float bend_semis;
    float z_gain;
    /* §9.5 per-voice RAMP on the mod[] deviation — the per-voice analogue of the part
     * ramps[]. A single in-flight ramp per voice (the latest per-voice ramp wins; a
     * per-voice SET supersedes it). Gated on vramp.active (false for the golden, so the
     * rendered bytes are unchanged when no per-voice ramp is in flight). */
    dev_ramp vramp;
    int vramp_idx;
    /* ---- DSP state ---- */
    /* control-rate-smoothed parameter values (§9.3: the device interpolates
     * at its declared control rate — instant steps are zipper clicks) */
    float s_pitch, s_shape, s_cutoff, s_reso, s_master;
    bool s_init;
    /* note voice: envelope-gated, portamento via smoothed frequency */
    float n_phase_l, n_phase_r;
    float n_low_l, n_band_l, n_low_r, n_band_r;
    float n_freq;   /* smoothed toward the sounding note */
    float env;      /* envelope level 0..1 */
    uint8_t env_reset; /* sub-blocks of fast decay before a fresh attack */
    uint32_t seen_seq;
} synth_voice;

/* ---------------- the arpeggiator (§9.7 consumer) ----------------
 *
 * All state is render-thread-owned: note events mutate the latch from
 * evq_apply_due (render thread), transport anchors arrive the same way,
 * and the step clock fires between render segments. Musical position at
 * SSI x derives from the anchor: ppq(x) = ppq0 + (x - ssi0) * tempo /
 * (60 * rate) — linear until the next anchor (§9.7), so steps land on
 * division boundaries sample-exactly BY CONSTRUCTION, and a loop wrap
 * or locate is just a new anchor to realign against (T17). */
#define ARP_LATCH_MAX 8

typedef struct {
    /* transport anchor */
    bool playing;
    bool anchor_valid;
    double tempo;       /* BPM */
    double anchor_ppq;  /* song position at anchor_ssi */
    uint64_t anchor_ssi;
    /* latch, in press order */
    uint8_t latch[ARP_LATCH_MAX];
    float vel[ARP_LATCH_MAX];
    int nlatch;
    /* stepping */
    int step;            /* monotone step counter (mode maps it to a note) */
    int sounding;        /* note the arp voice is holding, -1 = none */
    uint64_t gate_off;   /* SSI to release `sounding` (0 = none pending) */
} arp_state;

/* P2 voice pool: per-part polyphony. NVOICES sounding/ringing notes per part,
 * allocated deterministically (index-first, steal-oldest by alloc_seq — see
 * voice_alloc). Every voice is note-only now (the drone is gone). */
#define NVOICES 8

/* One multitimbral part: a POOL of voices, a per-part allocation clock, its
 * arp, AND (P3) its own param values + ramps so each part is an independent
 * timbre. Note identity now lives in each voice (P2); the only cross-thread
 * note field left is `panic_note`, a render-thread-consumed signal from the
 * panic paths (engine_all_notes_off / engine_note_off_if cross threads and
 * cannot touch the render-owned voice pool directly). pval crosses threads too
 * (render ramps, session loads, panel knobs) so it is _Atomic, exactly as the
 * old global dev_param.value was. voices/arp/ramps/alloc_clock are
 * render-thread-only (event application happens on the render thread). */
typedef struct {
    synth_voice voices[NVOICES];
    uint64_t alloc_clock; /* next alloc_seq; monotone, render-thread-only */
    /* panic signal (§ panic paths): another thread stores a value here and the
     * render thread services it at the next segment. -2 = none, -1 = ALL notes
     * off, >=0 = release that note number across the pool. _Atomic because the
     * write crosses threads; the render's exchange is the single consumer. */
    _Atomic int panic_note;
    arp_state arp;
    _Atomic float pval[NPARAMS]; /* per-part param values (P3) */
    dev_ramp ramps[NPARAMS];     /* per-part automation ramps (P3) */
} part;
#define PANIC_NONE (-2)
#define PANIC_ALL (-1)
/* NPARTS now lives in device.h (state.c stages all 16 parts on load). */
/* every part starts idle: all voices note -1 / inactive (the zero-init voice
 * has note 0 which would read as "holding note 0", so note is set to -1
 * explicitly), panic_note PANIC_NONE, AND arp.sounding -1 (else part_active
 * would read the zero-init sounding==0 as "holding note 0" before the first
 * stream reset). pval starts at the factory defaults (PVAL_DEFAULTS ==
 * g_params[].def in id order) so all 16 parts are IDENTICAL to the old globals
 * at boot — part 0 / channel 0 with one note therefore renders byte-identically
 * to P2.2. ramps zero-init (inactive). GNU range designators (engine.c is
 * gnu11). */
static part g_parts[NPARTS] = {[0 ... NPARTS - 1] = {
    .voices = {[0 ... NVOICES - 1] = {.note = -1}},
    .panic_note = PANIC_NONE,
    .arp = {.sounding = -1},
    .pval = PVAL_DEFAULTS}};

/* Per-part param value accessors (P3). Same cross-thread semantics as the old
 * global param_get/param_put: relaxed atomics, last-write-wins. Indexed by the
 * g_params slot (param_index) so the hot paths don't re-scan the table. */
static inline float part_pget(const part *p, size_t idx) {
    return atomic_load_explicit(&p->pval[idx], memory_order_relaxed);
}
static inline void part_pput(part *p, size_t idx, float v) {
    atomic_store_explicit(&p->pval[idx], v, memory_order_relaxed);
}

/* Cross-module per-part access (declared in device.h): state.c / session.c /
 * panel.c reach a part's values by id without touching the private struct. */
float engine_part_param_get(int part_idx, uint32_t id) {
    if (part_idx < 0 || part_idx >= NPARTS) return 0.0f;
    int idx = param_index(id);
    return idx < 0 ? 0.0f : part_pget(&g_parts[part_idx], (size_t)idx);
}
void engine_part_param_put(int part_idx, uint32_t id, float v) {
    if (part_idx < 0 || part_idx >= NPARTS) return;
    int idx = param_index(id);
    if (idx >= 0) part_pput(&g_parts[part_idx], (size_t)idx, v);
}

/* stepped params quantize: normalized [0,1] -> step index. Per part (P3):
 * reads p->pval, not a global value. */
static int param_step_index(const part *p, uint32_t id) {
    int idx = param_index(id);
    if (idx < 0) return 0;
    int n = g_params[idx].steps;
    if (n <= 1) return 0;
    int si = (int)(part_pget(p, (size_t)idx) * n);
    return si >= n ? n - 1 : si;
}

/* per-part value by id, render hot path (was the global param_value). */
static float param_value(const part *p, uint32_t id) {
    int idx = param_index(id);
    return idx < 0 ? 0.0f : part_pget(p, (size_t)idx);
}

/* §9.4 per-voice EFFECTIVE value: the part base plus this voice's mod offset,
 * clamped to [0,1] (the modulation is signed; the SUM is what gets clamped, per
 * §9.4 — the base alone is already in range). Called only when v->has_mod is
 * set; the VP() hot-path macro routes the unmodulated case (always true for the
 * golden) to the LITERAL param_value() above so that codegen — and therefore
 * the rendered bytes — is unchanged when no voice has been modulated. */
static float voice_param(const part *p, const synth_voice *v, uint32_t id) {
    int idx = param_index(id);
    if (idx < 0) return 0.0f;
    float base = part_pget(p, (size_t)idx);
    float r = base + v->mod[idx];
    return r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
}
#define VP(p, v, id) ((v)->has_mod ? voice_param((p), (v), (id)) : param_value((p), (id)))

/* PANIC SIGNALLING (§ panic paths). engine_all_notes_off / engine_note_off_if
 * run on the session/panel threads and must work even when the event queue is
 * jammed. The voice pool is render-thread-owned, so these threads CANNOT poke
 * voices directly without a data race. Instead they store a request into the
 * per-part _Atomic panic_note; the render thread services it (panic_service)
 * once per segment, atomically exchanging it back to PANIC_NONE. A second panic
 * arriving before the render drains the first wins last-write — for ALL_OFF
 * that is correct (a stronger panic), and a single-note off losing to a later
 * one only means the earlier note also gets caught by part_active decay; in the
 * pathological queue-jammed case a follow-up ALL_OFF (the escalation path's
 * partner) clears everything. The store is release / the exchange acquire so
 * the render sees a coherent value. */
void engine_all_notes_off(void) {
    for (size_t i = 0; i < NPARTS; i++)
        atomic_store_explicit(&g_parts[i].panic_note, PANIC_ALL, memory_order_release);
}
void engine_note_off_if(uint32_t note) {
    for (size_t i = 0; i < NPARTS; i++)
        atomic_store_explicit(&g_parts[i].panic_note, (int)note, memory_order_release);
}

/* ---------------- deterministic voice allocation (§9.5) ----------------
 *
 * note-on allocates a voice; note-off releases it (env decays as the old mono
 * voice did); the render frees a released voice once its env decays below the
 * same threshold the old engine used. Allocation is a PURE INTEGER FUNCTION of
 * the ordered event stream — index-first, steal-oldest-by-alloc_seq — so the
 * render stays reproducible (no wall-clock, no float in the choice). All three
 * helpers are render-thread-only (called from evq_apply_due / the arp / the
 * panic service, all on the render thread). */

/* Pick the voice a note-on takes: the first FREE slot in index order, else the
 * active slot with the smallest alloc_seq (the oldest — deterministic since
 * alloc_seq is unique and monotone per part). The policy itself is the pure
 * harp_voice_pick (device/voice_alloc.h) so it is unit-testable off-hardware;
 * this just feeds it the pool state and returns the chosen slot. */
static synth_voice *voice_alloc(part *p) {
    bool active[NVOICES];
    uint64_t seq[NVOICES];
    for (int i = 0; i < NVOICES; i++) {
        active[i] = p->voices[i].active;
        seq[i] = p->voices[i].alloc_seq;
    }
    return &p->voices[harp_voice_pick(active, seq, NVOICES)];
}

/* Start a note on voice v. The DSP state is PRESERVED on a steal (phase/filter
 * continue — no memset, so a steal does not click); only the envelope is
 * re-armed via the seen_seq != note_seq retrig edge the kernel already honors.
 * mod[] resets so a stolen voice does not inherit the previous note's offsets. */
static void voice_note_on(part *p, int note, float vel, uint32_t voice_id) {
    synth_voice *v = voice_alloc(p);
    v->note = note;
    v->note_vel = vel;
    v->voice_id = voice_id;
    v->alloc_seq = p->alloc_clock++;
    v->active = true;
    v->note_seq++; /* retrig edge: the kernel resets the env on seen_seq mismatch */
    v->has_mod = false;
    memset(v->mod, 0, sizeof v->mod);
    v->bend_semis = 0.0f; /* a fresh/stolen voice starts unbent + unpressured */
    v->z_gain = 0.0f;
    v->vramp.active = false; /* ...and with no in-flight §9.5 per-voice ramp */
}

/* Release every active voice currently sounding `note` (set note=-1 so its env
 * decays into release exactly like the old mono voice; the render frees it when
 * env falls below the idle threshold). A note can sound on at most one voice in
 * normal play, but releasing all matches is the safe panic-consistent choice. */
static void voice_note_off(part *p, int note) {
    for (int i = 0; i < NVOICES; i++)
        if (p->voices[i].active && p->voices[i].note == note) p->voices[i].note = -1;
}

/* Zero a part's whole voice pool to the cold-start idle state (T15: the voice
 * starts from zero at audio.start). Each voice's DSP + identity is zeroed, note
 * set to -1 (the zero value 0 would read as note 0), active cleared; the
 * alloc_clock restarts so alloc_seq counting is deterministic per stream. */
static void part_voices_cold(part *p) {
    for (int i = 0; i < NVOICES; i++) {
        p->voices[i] = (synth_voice){0};
        p->voices[i].note = -1;
    }
    p->alloc_clock = 0;
}

static dev_event g_evq[DEV_EVQ_CAP];
static size_t g_evq_n; /* under g_evq_mu */
static pthread_mutex_t g_evq_mu = PTHREAD_MUTEX_INITIALIZER;
_Atomic int g_touch_pending;
_Atomic uint64_t g_evq_drops;
_Atomic uint64_t g_evt_late;
_Atomic uint64_t g_ramp_late;
_Atomic uint32_t g_evt_consumed;
_Atomic uint64_t g_fence_waits;
_Atomic uint64_t g_fence_timeouts;

/* §9.9 OUTPUT METERING atomics. Render thread folds (engine_meter_fold);
 * the session.c pump reads. Relaxed: meters order nothing, and the worst case
 * is a one-block-stale value the eye cannot see. */
_Atomic float g_meter_peak[METER_NSLOTS];
_Atomic float g_meter_rms[METER_NSLOTS];

/* A meter accumulator spans a whole host block, which render_part_slots emits
 * in event-aligned SEGMENTS reusing one scratch buffer. We therefore fold each
 * segment into a stack accumulator (peak max, energy sum, count) and commit
 * once per block, so peak/RMS cover all n frames the host receives — not just
 * the last segment. */
typedef struct {
    float peak;
    double sumsq;
    uint64_t nsamp;
} meter_acc;

/* Fold one rendered stereo SEGMENT (interleaved L,R; `n` frames) into `acc`.
 * This ONLY READS samples already written by engine_render and updates the
 * stack accumulator — it touches no engine_render call, no render math, no slot
 * write, no SSI. Removing every meter call leaves the rendered audio
 * bit-for-bit unchanged, which is the golden-render guarantee.
 *
 * NaN/Inf guard (§9.9): a non-finite sample (it cannot occur in the
 * finite-input synth, but the contract is guarded anyway) is dropped from both
 * peak and energy, so no NaN/Inf can propagate to the stored meter. */
static void meter_acc_fold(meter_acc *acc, const float *interleaved, uint32_t n) {
    uint32_t ns = 2u * n;
    for (uint32_t i = 0; i < ns; i++) {
        float s = interleaved[i];
        if (!isfinite(s)) continue;
        float a = fabsf(s);
        if (a > acc->peak) acc->peak = a;
        acc->sumsq += (double)s * (double)s; /* double sum: a 1024-frame block of
                                                |1.0| sums to 2048, no f32 bias */
    }
    acc->nsamp += ns;
}

/* Commit a block accumulator to meter slot `ix`. Flush-to-zero on silence: a
 * denormal peak/rms (a lone tiny release-tail sample) reads as silence on a
 * meter and would only cost the host denormal stalls, so it stores exact 0. */
static void meter_acc_commit(int ix, const meter_acc *acc) {
    float peak = acc->peak;
    float rms = acc->nsamp ? (float)sqrt(acc->sumsq / (double)acc->nsamp) : 0.0f;
    if (!(peak > 1e-20f)) peak = 0.0f;
    if (!(rms > 1e-20f)) rms = 0.0f;
    atomic_store_explicit(&g_meter_peak[ix], peak, memory_order_relaxed);
    atomic_store_explicit(&g_meter_rms[ix], rms, memory_order_relaxed);
}

/* Reset every meter slot to the silent floor. Called by the pump at stream
 * start (a new stream is a new meter timeline; stale peaks must not leak). */
void engine_meters_reset(void) {
    for (int i = 0; i < METER_NSLOTS; i++) {
        atomic_store_explicit(&g_meter_peak[i], 0.0f, memory_order_relaxed);
        atomic_store_explicit(&g_meter_rms[i], 0.0f, memory_order_relaxed);
    }
}

void evq_push(dev_event ev) {
    pthread_mutex_lock(&g_evq_mu);
    if (g_evq_n < DEV_EVQ_CAP)
        g_evq[g_evq_n++] = ev;
    else
        CTR_INC(g_evq_drops);
    pthread_mutex_unlock(&g_evq_mu);
}

/* §9.6: enqueue a whole transaction batch ALL-OR-NOTHING under ONE lock. The render thread
 * drains same-ts events together (evq_apply_due), so committing the batch at one ts applies
 * it atomically — but ONLY if the whole batch lands; a per-event push could drop part of it
 * at DEV_EVQ_CAP and apply a partial kit-load. Returns false (batch rejected, counted) when
 * the queue can't hold all of it. */
bool evq_push_batch(const dev_event *evs, size_t count) {
    pthread_mutex_lock(&g_evq_mu);
    bool ok = (g_evq_n + count <= DEV_EVQ_CAP);
    if (ok)
        for (size_t i = 0; i < count; i++) g_evq[g_evq_n++] = evs[i];
    else
        for (size_t i = 0; i < count; i++) CTR_INC(g_evq_drops);
    pthread_mutex_unlock(&g_evq_mu);
    return ok;
}

/* Push a RUN of independent events under ONE lock — BYTE-IDENTICAL to `count` sequential
 * evq_push() calls: same order, same PER-EVENT partial-fill-then-drop at DEV_EVQ_CAP, same
 * g_evq_drops accounting. This is the consume-side flood path (session.c stages a run of
 * already-arrived untagged param/ramp/mod events and commits them here); it amortises the
 * mutex acquire/release — contended with the render thread's evq_apply_due — over the whole
 * run instead of paying it per event. It is NOT the §9.6 atomic batch: evq_push_batch stays
 * all-or-nothing for a transaction's same-instant commit, but an independent-event flood has
 * no all-or-nothing contract, so matching per-event push exactly IS the bit-exact requirement
 * (a partial fill under queue pressure must land the same events a per-event push would). */
void evq_push_run(const dev_event *evs, size_t count) {
    pthread_mutex_lock(&g_evq_mu);
    for (size_t i = 0; i < count; i++) {
        if (g_evq_n < DEV_EVQ_CAP)
            g_evq[g_evq_n++] = evs[i];
        else
            CTR_INC(g_evq_drops);
    }
    pthread_mutex_unlock(&g_evq_mu);
}

bool evq_full(void) {
    pthread_mutex_lock(&g_evq_mu);
    bool full = g_evq_n >= DEV_EVQ_CAP;
    pthread_mutex_unlock(&g_evq_mu);
    return full;
}

/* (dev_ramp is defined with the per-part types up top — P3: ramps[NPARAMS]
 * now live inside each part, not a global g_ramps[].) */

/* Stream state reset (§7.1 in miniature): a new stream is a new time
 * domain — queued events and active ramps from the previous session's SSI
 * timeline are stale BY DEFINITION and must not leak into this one.
 * (Learned from a jammed queue of never-due zombie events silently
 * dropping every new event.) */
static void arp_stream_reset(part *p);

void evq_reset_for_new_stream(void) {
    pthread_mutex_lock(&g_evq_mu);
    g_evq_n = 0;
    pthread_mutex_unlock(&g_evq_mu);
    /* notes are performance state OF A STREAM: a note held across a stream
     * stop/restart is a stuck note (its note-off died with the old stream).
     * Per part now: each part's note + arp + RAMPS are stale BY DEFINITION
     * (P3: ramps are per part — clear every part's). pval is patch state and
     * is NOT reset: it survives the stream restart, like the old globals. */
    for (size_t i = 0; i < NPARTS; i++) {
        part *p = &g_parts[i];
        /* every voice idles (note -1, freed); a note held across a restart is
         * a stuck note. The DSP state is zeroed by the audio thread at start. */
        for (int vi = 0; vi < NVOICES; vi++) {
            p->voices[vi].note = -1;
            p->voices[vi].active = false;
        }
        atomic_store_explicit(&p->panic_note, PANIC_NONE, memory_order_relaxed);
        memset(p->ramps, 0, sizeof p->ramps);
        arp_stream_reset(p); /* defined with the arp block below */
    }
    /* fence sequence space restarts with the stream (host resets its
     * queued-event counter at session start; both sides count from 0) */
    atomic_store_explicit(&g_evt_consumed, 0, memory_order_release);
}
/* ---------------- the sound engine ---------------- */
/* (synth_voice is defined up top so 'part' can contain it — see P2.0;
 * param_value() is the per-part value accessor, also defined up top.) */

#define SMOOTH_SUBBLOCK 32 /* samples; 1.5 kHz at 48 k — the declared control rate honored */

/* Advance a part's active ramps to stream position `pos`: the base value moves
 * along the line (visible to echo/refs — ramps change the stored value, §9.4).
 * P3: per part — operates on p->ramps into p->pval. */
static void ramps_advance(part *p, uint64_t pos) {
    for (size_t i = 0; i < NPARAMS; i++) {
        dev_ramp *r = &p->ramps[i];
        if (!r->active) continue;
        if (pos >= r->end || r->end <= r->start) {
            part_pput(p, i, r->target);
            r->active = false;
        } else if (pos > r->start) {
            float t = (float)(pos - r->start) / (float)(r->end - r->start);
            part_pput(p, i, r->start_val + (r->target - r->start_val) * t);
        }
    }
    /* §9.5 per-voice ramps: tick each voice's in-flight ramp into its mod[] deviation —
     * the per-voice analogue of the part ramps above. Inactive (vramp.active == false)
     * for the golden, so the rendered bytes are unchanged when no per-voice ramp exists. */
    for (int vi = 0; vi < NVOICES; vi++) {
        synth_voice *v = &p->voices[vi];
        dev_ramp *r = &v->vramp;
        if (!r->active) continue;
        int j = v->vramp_idx;
        if (pos >= r->end || r->end <= r->start) {
            v->mod[j] = r->target;
            r->active = false;
        } else if (pos > r->start) {
            float t = (float)(pos - r->start) / (float)(r->end - r->start);
            v->mod[j] = r->start_val + (r->target - r->start_val) * t;
        }
    }
}

/* Every part is note-only now the drone is gone (engine 2.0.0): one note voice's
 * oscillator pair + state-variable filter, gated by the envelope.
 *
 * accumulate: false WRITES the stereo scratch with a direct '=' (the first voice
 * into a buffer, signed zero preserved); true sums onto it with '+='. The per-part
 * output branch writes each isolated part with a fresh '='; the main mix calls
 * part0(accumulate=false) as the first writer, then active parts(accumulate=true).
 * part0 is the only accumulate=false caller in the main-mix path. */
static void engine_render(part *p, synth_voice *v, float *interleaved, uint32_t n,
                          float rate, uint64_t pos, bool accumulate) {
    /* P2: the note identity (note/note_vel/note_seq) reads from THIS VOICE now,
     * not the part. param reads go through VP(p,v,id): when no mod has addressed
     * the voice (has_mod==false, ALWAYS for the golden) VP collapses to the
     * literal param_value(p,id) below — identical codegen, identical bytes — so
     * a single isolated voice rendered with accumulate=false is bit-for-bit the
     * pre-pool mono kernel. The DSP body is otherwise UNTOUCHED. */
    if (!v->s_init) { /* first block: land on targets, no glide-in from zero */
        v->s_pitch = VP(p, v, 1);
        v->s_shape = VP(p, v, 2);
        v->s_cutoff = VP(p, v, 3);
        v->s_reso = VP(p, v, 4);
        v->s_master = VP(p, v, 7); /* Master Level: renumbered 8->7 at 2.1.0 */
        v->n_freq = 220.0f;
        v->s_init = true;
    }
    float detune = 1.004f;
    /* one-pole toward targets per sub-block; tau ~= 12 ms */
    float alpha = 1.0f - expf(-(float)SMOOTH_SUBBLOCK / (0.012f * rate));

    uint32_t i = 0;
    while (i < n) {
        uint32_t end = i + SMOOTH_SUBBLOCK;
        if (end > n) end = n;

        ramps_advance(p, pos + i);
        v->s_pitch += alpha * (VP(p, v, 1) - v->s_pitch);
        v->s_shape += alpha * (VP(p, v, 2) - v->s_shape);
        v->s_cutoff += alpha * (VP(p, v, 3) - v->s_cutoff);
        v->s_reso += alpha * (VP(p, v, 4) - v->s_reso);
        v->s_master += alpha * (VP(p, v, 7) - v->s_master); /* Master Level: 8->7 */

        /* note voice control: gate + envelope. Pitch SNAPS on a fresh
         * attack and glides only when legato (env still high): a fixed-tau
         * glide on every note makes perceived onsets interval-dependent —
         * measured r=0.86 between grid deviation and leap size, ±23 ms of
         * musical slop on sequenced 16ths. Real monosynths snap too. */
        int note = v->note;
        bool retrig = v->seen_seq != v->note_seq;
        if (retrig) {
            v->seen_seq = v->note_seq;
            /* Envelope RESETS per note: without this, articulation depends
             * on how far the previous release decayed (measured transient
             * depth 0.06..0.99 across one take — heard as timing slop).
             * Two fast-decay sub-blocks (~1.3 ms) to near-zero, click-free,
             * then a uniform attack. */
            if (v->env > 0.07f) v->env_reset = 2;
        }
        bool gate = note >= 0;
        if (gate) {
            /* §9.5 per-voice pitch bend (MPE X): a semitone offset on the note.
             * Gated on bend_semis != 0 so an unbent voice — including a voice
             * carrying only a cutoff mod (has_mod set) — keeps the LITERAL
             * original expression, byte-identical to the golden render. */
            float target = v->bend_semis != 0.0f
                ? 440.0f * exp2f(((float)note + v->bend_semis - 69.0f) / 12.0f)
                : 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
            float glide = VP(p, v, 12); /* Glide: renumbered 13->12 at 2.1.0 */
            if (glide <= 0.001f || (retrig && v->env < 0.2f)) {
                v->n_freq = target; /* no portamento / fresh attack: arrive
                                       on time (off notes are worse than
                                       missing glides) */
            } else {
                /* legato glide, player-set tau: 1..512 ms exp-mapped */
                float tau = 0.001f * exp2f(glide * 9.0f);
                float ga = 1.0f - expf(-(float)SMOOTH_SUBBLOCK / (tau * rate));
                v->n_freq += ga * (target - v->n_freq);
            }
        }
        /* attack 1 ms..1 s, release 5 ms..3 s (exp-mapped knobs) */
        float atk_tau = 0.001f * exp2f(VP(p, v, 5) * 10.0f);
        float rel_tau = 0.005f * exp2f(VP(p, v, 6) * 9.2f);
        if (v->env_reset) {
            v->env *= 0.25f; /* fast decay sub-block */
            v->env_reset--;
        } else {
            float env_target = gate ? v->note_vel : 0.0f;
            /* §9.5 per-voice loudness (MPE Z / pressure): scale the gated target
             * by (1 + z_gain). Gated on z_gain != 0 so an unpressured voice keeps
             * v->note_vel exactly (byte-identical golden). */
            if (gate && v->z_gain != 0.0f) env_target = v->note_vel * (1.0f + v->z_gain);
            float env_alpha =
                1.0f - expf(-(float)SMOOTH_SUBBLOCK / ((gate ? atk_tau : rel_tau) * rate));
            v->env += env_alpha * (env_target - v->env);
        }

        float fc = 60.0f * exp2f(v->s_cutoff * 6.0f);
        float f = 2.0f * sinf((float)M_PI * fc / rate);
        float q = 1.0f - 0.9f * v->s_reso;
        float shape = v->s_shape, master = v->s_master;
        float env = v->env;
        float nfreq = v->n_freq;

        for (; i < end; i++) {
            /* note oscillator pair (own filter state) — every part is note-only
             * now the continuous drone is gone (engine 2.0.0). */
            v->n_phase_l += nfreq / rate;
            if (v->n_phase_l >= 1.0f) v->n_phase_l -= 1.0f;
            v->n_phase_r += nfreq * detune / rate;
            if (v->n_phase_r >= 1.0f) v->n_phase_r -= 1.0f;
            float nosc_l = sinf(2.0f * (float)M_PI * v->n_phase_l);
            nosc_l += ((2.0f * v->n_phase_l - 1.0f) - nosc_l) * shape;
            float nosc_r = sinf(2.0f * (float)M_PI * v->n_phase_r);
            nosc_r += ((2.0f * v->n_phase_r - 1.0f) - nosc_r) * shape;

            v->n_low_l += f * v->n_band_l;
            float nhigh_l = nosc_l - v->n_low_l - q * v->n_band_l;
            v->n_band_l += f * nhigh_l;
            v->n_low_r += f * v->n_band_r;
            float nhigh_r = nosc_r - v->n_low_r - q * v->n_band_r;
            v->n_band_r += f * nhigh_r;

            /* The PRIMARY voice into a scratch buffer WRITES every sample with a
             * direct '=' (accumulate=false), signed zero included (a render can
             * legitimately produce -0.0f; a memset+accumulate would flip them to
             * +0.0f and break the byte hash). Voices that mix ON TOP ACCUMULATE
             * with '+=' (accumulate=true). This is the former note-only branch —
             * every part renders the same note expression now the drone is gone. */
            float out_l = v->n_low_l * env;
            float out_r = v->n_low_r * env;
            if (!accumulate) {
                interleaved[2 * i] = out_l * master * 0.5f;
                interleaved[2 * i + 1] = out_r * master * 0.5f;
            } else {
                interleaved[2 * i] += out_l * master * 0.5f;
                interleaved[2 * i + 1] += out_r * master * 0.5f;
            }
        }
    }
}

/* idle threshold: a released voice (note==-1) whose env has decayed below this
 * is freed (active=false). THE SAME constant part_active uses to decide a part
 * still rings, so a voice keeps rendering its release tail until it crosses the
 * threshold, then goes silent and is reclaimed — exactly the mono engine's tail
 * lifetime, now per voice. */
#define VOICE_IDLE_THRESH 1e-4f

/* THE single render-summation helper (§ design 3). Renders a part's whole voice
 * pool into `out` (interleaved stereo, n frames) at stream position `pos`,
 * summing voices IN INDEX ORDER. Replaces the per-part engine_render call in
 * BOTH render_with_events (main mix) and render_part_slots (per-part slot) so
 * the two paths CANNOT drift — they share this exact summation.
 *
 * No drone now: a voice renders only while allocated/ringing. An idle part writes
 * nothing here, so render_part_voices memsets its buffer to silence for the first
 * writer (accumulate=false) — see below — instead of relying on a continuous
 * voice 0 to initialise it as the pre-drone-removal engine did. A free note-only
 * voice is silence; skipping it is the same as adding zero, AND it preserves the
 * signed-zero write of the first writer.
 *
 * `accumulate` is the PART-LEVEL flag (false = this part is the first writer of
 * `out`, so its first rendered voice WRITES with '='; subsequent voices and all
 * voices of an accumulate=true part sum with '+='). For ONE active voice and
 * accumulate=false this is a single engine_render(=) call — byte-identical to
 * the mono engine.
 *
 * Voices released long enough to fall under VOICE_IDLE_THRESH are freed here
 * (after rendering, so the final tail sample is still emitted). */
static void render_part_voices(part *p, float *out, uint32_t n, float rate,
                               uint64_t pos, bool accumulate) {
    bool wrote = false; /* has any voice written `out` yet this call? */
    for (int vi = 0; vi < NVOICES; vi++) {
        synth_voice *v = &p->voices[vi];
        /* every part is note-only now: a voice renders only while allocated/ringing */
        if (!v->active) continue;
        /* first writer uses the part-level accumulate; later voices always '+=' */
        engine_render(p, v, out, n, rate, pos, wrote ? true : accumulate);
        wrote = true;
        /* reclaim a released voice once its tail has decayed, freeing its slot for
         * re-allocation. active=false leaves the DSP state in place; a future
         * allocation/steal preserves phase (no click). */
        if (v->active && v->note < 0 && v->env <= VOICE_IDLE_THRESH)
            v->active = false;
    }
    /* accumulate=false promises the buffer is fully written (it is the first writer).
     * A part with no active voice wrote nothing — define the buffer as silence so the
     * consumer never reads stale samples. Part 0 is no longer special (no drone), so
     * an idle main mix legitimately hits this and renders silence. accumulate=true
     * sums onto an already-written buffer, so it never needs this. */
    if (!wrote && !accumulate) memset(out, 0, (size_t)2 * n * sizeof(float));
}

/* Service a pending panic for one part (render thread, once per segment). The
 * panic threads cannot touch the render-owned pool, so they signalled via the
 * _Atomic panic_note; we exchange it back to PANIC_NONE and act: PANIC_ALL
 * releases AND frees every voice (a hard all-notes-off silences ringing tails
 * too, like the old note_put(-1) on the single voice did), a note number
 * releases matching voices into their normal decay. */
static void panic_service(part *p) {
    int req = atomic_exchange_explicit(&p->panic_note, PANIC_NONE, memory_order_acquire);
    if (req == PANIC_NONE) return;
    if (req == PANIC_ALL) {
        for (int vi = 0; vi < NVOICES; vi++) {
            p->voices[vi].note = -1;
            p->voices[vi].active = false;
        }
    } else {
        voice_note_off(p, req);
    }
}

/* Free-running stream thread (§8.3 mode 0): the device clock owns the
 * stream; the MSC counts rendered samples; frames carry (epoch, msc). */

static void render_with_events(float *interleaved, uint32_t n,
                               float rate, uint64_t pos);
/* §P2.2 output assembly: render the host-requested output slots (a->out_slots
 * / n_out_slots) into `out`, interleaved nsamples × n_out_slots in request
 * order. Returns the channel count written (== a->n_out_slots), to stamp the
 * frame header `slots`. The default stereo main mix ({0,1}) routes straight
 * through the unchanged render_with_events; see the definition for the
 * additive per-part branch. */


/* Apply queue events due at `pos`; return the next event timestamp inside
 * (pos, limit) for render segmentation, or 0 if none. Render thread only. */
/* ---------------- the arpeggiator (§9.7 consumer) ----------------
 *
 * Per part now: every function takes 'part *p' and operates on p->arp. P2: the
 * arp is monophonic, so it drives VOICE 0 of the part directly (arp_voice_on /
 * arp_voice_off) — the same slot the pre-pool mono engine used — instead of the
 * polyphonic allocator, keeping "one note at a time" byte-identical. The arp
 * type + ARP_LATCH_MAX are defined up top so 'part' can contain the state by
 * value; the step/latch/anchor/gate MATH is unchanged. P3: arp_active() reads
 * THIS PART's Arp Mode param (the arp params 8..11 are per part now; renumbered
 * contiguous from 9..12 at 2.1.0 with the drone's old id 7 reclaimed). */

static bool arp_active(const part *p) { return param_step_index(p, 8) != 0; } /* Arp Mode: 9->8 */

static void arp_reset(part *p);
static void arp_voice_off(part *p);

/* a new stream is a new time domain: anchor and groove are stale */
static void arp_stream_reset(part *p) {
    arp_reset(p);
    p->arp.anchor_valid = false;
    p->arp.playing = false;
    p->arp.tempo = 0;
}

/* The arp is MONOPHONIC by construction (§9.7: one sounding note, retriggered
 * each step). It drives VOICE 0 of its part directly — the same slot the
 * pre-pool mono engine used — rather than the polyphonic allocator, so a step
 * RETRIGGERS the one voice (env reset, phase continuous) instead of ringing a
 * fresh voice out. That keeps "one note at a time" byte-identical to the mono
 * groove render: the arp never produces tail-overlap polyphony. (Voice 0 is just
 * the first voice slot now — no drone rides on it.) */
static void arp_voice_off(part *p) {
    synth_voice *v0 = &p->voices[0];
    if (p->arp.sounding >= 0 && v0->note == p->arp.sounding) v0->note = -1;
    p->arp.sounding = -1;
    p->arp.gate_off = 0;
}

/* Retrigger voice 0 onto the arp's stepped note — the mono note_put + note_seq
 * bump the pre-pool engine did, now on the voice. active=true so a note-only
 * part (1..15) renders it; alloc_seq is left untouched (the arp owns voice 0,
 * never steals). */
static void arp_voice_on(part *p, int note, float vel) {
    synth_voice *v0 = &p->voices[0];
    v0->note_vel = vel;
    v0->note = note;
    v0->note_seq++;
    v0->active = true;
}

static void arp_latch_add(part *p, uint8_t note, float vel) {
    for (int i = 0; i < p->arp.nlatch; i++)
        if (p->arp.latch[i] == note) {
            p->arp.vel[i] = vel;
            return;
        }
    if (p->arp.nlatch < ARP_LATCH_MAX) {
        p->arp.latch[p->arp.nlatch] = note;
        p->arp.vel[p->arp.nlatch] = vel;
        p->arp.nlatch++;
    }
}

static void arp_latch_remove(part *p, uint8_t note) {
    for (int i = 0; i < p->arp.nlatch; i++)
        if (p->arp.latch[i] == note) {
            memmove(&p->arp.latch[i], &p->arp.latch[i + 1],
                    (size_t)(p->arp.nlatch - i - 1));
            memmove(&p->arp.vel[i], &p->arp.vel[i + 1],
                    (size_t)(p->arp.nlatch - i - 1) * sizeof(float));
            p->arp.nlatch--;
            return;
        }
}

static void arp_reset(part *p) {
    p->arp.nlatch = 0;
    p->arp.step = 0;
    arp_voice_off(p);
}

/* musical position <-> stream position under the current anchor */
static double arp_ppq_at(part *p, uint64_t ssi, double rate) {
    return p->arp.anchor_ppq +
           ((double)ssi - (double)p->arp.anchor_ssi) * p->arp.tempo / (60.0 * rate);
}
static uint64_t arp_ssi_at(part *p, double ppq, double rate) {
    double ds = (ppq - p->arp.anchor_ppq) * 60.0 * rate / p->arp.tempo;
    double ssi = (double)p->arp.anchor_ssi + ds;
    return ssi <= 0 ? 0 : (uint64_t)(ssi + 0.5);
}

/* First step boundary strictly after `after`, as an SSI. THE single
 * source of truth: the firing check asks "is pos the boundary seen from
 * pos-1?", so fire and deadline agree by construction — no epsilon games
 * across the ppq<->ssi rounding. */
static uint64_t arp_next_step_ssi(part *p, uint64_t after, double rate) {
    double div = ARP_DIV_PPQ[param_step_index(p, 9)]; /* Arp Division: 10->9 */
    double k = floor(arp_ppq_at(p, after, rate) / div + 1e-9) + 1.0;
    uint64_t bssi = arp_ssi_at(p, k * div, rate);
    while (bssi <= after) bssi = arp_ssi_at(p, (k += 1.0) * div, rate);
    return bssi;
}

/* next arp deadline strictly after `pos`: a step boundary or a pending
 * gate-off, whichever first. 0 = nothing scheduled. */
static uint64_t arp_next_deadline(part *p, uint64_t pos, double rate) {
    if (!arp_active(p) || !p->arp.playing || !p->arp.anchor_valid ||
        p->arp.tempo <= 0)
        return 0;
    uint64_t next = 0;
    if (p->arp.nlatch > 0) next = arp_next_step_ssi(p, pos, rate);
    if (p->arp.gate_off && p->arp.gate_off > pos &&
        (next == 0 || p->arp.gate_off < next))
        next = p->arp.gate_off;
    return next;
}

/* fire whatever is due exactly AT `pos` (called between render segments) */
static void arp_fire_due(part *p, uint64_t pos, double rate) {
    if (!arp_active(p)) return;
    if (p->arp.gate_off && pos >= p->arp.gate_off) arp_voice_off(p);
    if (!p->arp.playing || !p->arp.anchor_valid || p->arp.tempo <= 0 ||
        p->arp.nlatch == 0 || pos == 0)
        return;
    if (arp_next_step_ssi(p, pos - 1, rate) != pos) return; /* not a boundary */

    /* which latched note does this step sound? The mode/octave/sort/clamp logic
     * is the pure harp_arp_select (device/arp_select.h) so all four modes are
     * unit-testable off-hardware; `order` is its sort scratch. */
    int order[ARP_LATCH_MAX];
    harp_arp_pick pick = harp_arp_select(param_step_index(p, 8), p->arp.step, /* Arp Mode: 9->8 */
                                         p->arp.latch, p->arp.nlatch,
                                         param_step_index(p, 11), order); /* Arp Octaves: 12->11 */
    int note = pick.note;

    arp_voice_on(p, note, p->arp.vel[pick.sel]); /* mono retrigger on voice 0 */
    p->arp.sounding = note;
    /* gate: release after gate-fraction of the step length */
    double div = ARP_DIV_PPQ[param_step_index(p, 9)]; /* Arp Division: 10->9 */
    double step_samples = div * 60.0 * rate / p->arp.tempo;
    double gate = param_value(p, 10); /* Arp Gate: 11->10 */
    gate = gate < 0.05 ? 0.05 : gate > 0.98 ? 0.98 : gate;
    p->arp.gate_off = pos + (uint64_t)(step_samples * gate + 0.5);
    if (p->arp.gate_off <= pos) p->arp.gate_off = pos + 1;
    p->arp.step++;
}

static uint64_t evq_apply_due(uint64_t pos, uint64_t limit) {
    uint64_t next = 0;
    pthread_mutex_lock(&g_evq_mu);
    size_t w = 0;
    for (size_t i = 0; i < g_evq_n; i++) {
        dev_event *ev = &g_evq[i];
        if (ev->ts <= pos) {
            /* late = past the musical deadline (§9.2/§14.2): for ramps the
             * deadline is END (the start is the previous automation point,
             * legitimately in the recent past); for everything else, ts. */
            if (ev->kind == DEV_EV_RAMP) {
                if (ev->end && ev->end < pos) CTR_INC(g_ramp_late);
            } else if (ev->ts && ev->ts < pos)
                CTR_INC(g_evt_late);
            /* route to the event's part (real routing now, §P2.1). P3: the
             * note + arp cases mutate p, AND PARAM_SET/RAMP now apply to the
             * ROUTED part's pval/ramps (per-part params on the wire — channel
             * 0 -> part 0, so the golden's --set ch0 still hits part 0). ALL_OFF
             * / TRANSPORT act across ALL parts (see below). */
            part *p = &g_parts[harp_evt_part(ev->channel, NPARTS)]; /* §P2.1 routing (pure) */
            switch (ev->kind) {
                case DEV_EV_NOTE_ON:
                    /* arp engaged: notes feed the latch; the step clock
                     * owns the voice. Arp off (or transport stopped): the
                     * polyphonic allocator hands the note a voice (P2) — a
                     * voice per overlapping note, vs the old single mono voice.
                     * voice_id carries the §9.5 packed key for later mod. */
                    arp_latch_add(p, (uint8_t)ev->a, ev->v);
                    if (!arp_active(p) || !p->arp.playing || !p->arp.anchor_valid) {
                        /* §9.5 packed voice key for later per-voice mod targeting:
                         * (group<<12)|(channel<<8)|note. MIDI-1.0 note-ons carry no
                         * voice (ev->voice==0), so derive the canonical key from the
                         * part + note; honor an explicit voice if a future MIDI-2.0
                         * note-on ever supplies one. */
                        uint32_t vid = ev->voice ? ev->voice
                                       : (((uint32_t)ev->channel << 8) | ((uint32_t)ev->a & 0x7f));
                        voice_note_on(p, (int)ev->a, ev->v, vid);
                    }
                    break;
                case DEV_EV_NOTE_OFF:
                    arp_latch_remove(p, (uint8_t)ev->a);
                    if (!arp_active(p) || !p->arp.playing || !p->arp.anchor_valid) {
                        voice_note_off(p, (int)ev->a); /* release matching voices */
                    } else if (p->arp.nlatch == 0) {
                        arp_voice_off(p); /* all keys released = latch clears */
                    }
                    break;
                case DEV_EV_ALL_OFF:
                    /* global panic: reset arp + release/free every voice on
                     * EVERY part (a hard panic silences ringing tails too) */
                    for (size_t pi = 0; pi < NPARTS; pi++) {
                        arp_reset(&g_parts[pi]);
                        for (int vi = 0; vi < NVOICES; vi++) {
                            g_parts[pi].voices[vi].note = -1;
                            g_parts[pi].voices[vi].active = false;
                        }
                    }
                    break;
                case DEV_EV_TRANSPORT: {
                    /* transport has no channel: BROADCAST the one timeline to
                     * every part's arp (identical anchor/playing/tempo math
                     * per part, including the was/stop/start handling) */
                    for (size_t pi = 0; pi < NPARTS; pi++) {
                        part *tp = &g_parts[pi];
                        bool was = tp->arp.playing;
                        tp->arp.playing = (ev->a & 1) != 0;
                        if (ev->a & (1u << 3)) tp->arp.tempo = ev->v;
                        if (ev->a & (1u << 5)) {
                            tp->arp.anchor_ppq = ev->ppq;
                            tp->arp.anchor_ssi = ev->ts ? ev->ts : pos;
                            tp->arp.anchor_valid = true;
                        }
                        if (!tp->arp.playing && was) arp_voice_off(tp); /* stop: silence */
                        if (tp->arp.playing && !was) tp->arp.step = 0;  /* start on step 1 */
                    }
                    break;
                }
                case DEV_EV_PARAM_SET: {
                    int j = param_index(ev->a);
                    if (ev->voice) {
                        /* §9.5 per-voice SET: affect ONLY the addressed sounding voice —
                         * set its mod[] deviation so voice_param renders at ev->v, NOT the
                         * part base (which would hit every voice). Transient like §9.4 mod,
                         * so it does NOT touch stored state (no g_touch_pending). */
                        if (j >= 0) {
                            float base = part_pget(p, (size_t)j);
                            for (int vi = 0; vi < NVOICES; vi++) {
                                synth_voice *mv = &p->voices[vi];
                                if (!harp_mod_targets_voice(ev->voice, mv->active, mv->voice_id)) continue;
                                mv->mod[j] = ev->v - base;
                                mv->vramp.active = false; /* set supersedes ramp (§9.4) */
                                mv->has_mod = true;
                            }
                        }
                        break;
                    }
                    /* P3: voice==0 -> the ROUTED part's pval/ramps (was global) */
                    if (j >= 0) {
                        part_pput(p, (size_t)j, ev->v);
                        p->ramps[j].active = false; /* set supersedes ramp (§9.4) */
                    }
                    atomic_store_explicit(&g_touch_pending, 1,
                                          memory_order_release);
                    break;
                }
                case DEV_EV_RAMP: {
                    int j = param_index(ev->a);
                    if (ev->voice) {
                        /* §9.5 per-voice RAMP: ramp ONLY the addressed sounding voice's
                         * mod[] deviation toward ev->v over [pos, ev->end] (ticked in
                         * ramps_advance), NOT the part base. Transient like the per-voice
                         * SET above — no g_touch_pending. */
                        if (j >= 0) {
                            float base = part_pget(p, (size_t)j);
                            for (int vi = 0; vi < NVOICES; vi++) {
                                synth_voice *mv = &p->voices[vi];
                                if (!harp_mod_targets_voice(ev->voice, mv->active, mv->voice_id)) continue;
                                mv->vramp.active = true;
                                mv->vramp.start = pos;
                                mv->vramp.end = ev->end;
                                mv->vramp.start_val = mv->mod[j]; /* from the voice's current deviation */
                                mv->vramp.target = ev->v - base;  /* to the deviation that renders at ev->v */
                                mv->vramp_idx = j;
                                mv->has_mod = true;
                            }
                        }
                        break;
                    }
                    /* P3: voice==0 -> ramp the ROUTED part's value (was global) */
                    if (j >= 0) {
                        p->ramps[j].active = true;
                        p->ramps[j].start = pos;
                        p->ramps[j].end = ev->end;
                        p->ramps[j].start_val = part_pget(p, (size_t)j);
                        p->ramps[j].target = ev->v;
                    }
                    atomic_store_explicit(&g_touch_pending, 1,
                                          memory_order_release);
                    break;
                }
                case DEV_EV_MOD: {
                    /* §9.4 non-destructive modulation (P2): an additive, signed
                     * per-(param[,voice]) offset on the part BASE value. It does
                     * NOT touch pval/ramps (the base) or the dirty flag — it
                     * lives on the per-voice mod[] layer, clamped only after
                     * summation in voice_param(). Addressing (§9.5):
                     *   voice != 0 -> the ONE active voice whose voice_id matches
                     *                 (a mod to a gone voice is ignored, §9.5);
                     *   voice == 0 -> "whole-part" (no voice key): a part-wide
                     *                 offset applied to EVERY active voice, the
                     *                 reading consistent with the channel-level
                     *                 §9.4 base — every sounding voice modulates.
                     * §9.5 per-voice EXPRESSION (MPE) targets route to dedicated
                     * voice fields instead of mod[]: HARP_MOD_PITCH_BEND ->
                     * bend_semis (semitones, unclamped), HARP_MOD_PRESSURE ->
                     * z_gain (loudness). A normalized param id -> the mod[] layer.
                     * Unknown id: ignored. */
                    int idx = param_index(ev->a); /* >=0 = real param; <0 = expr/unknown */
                    /* §9.5 target + addressing via the pure helpers (device/evq_mod.h,
                     * host-unit-tested). Byte-identical to the prior inline checks. */
                    harp_mod_target_kind tk = harp_mod_target(ev->a, idx,
                                                              HARP_MOD_PITCH_BEND, HARP_MOD_PRESSURE);
                    if (tk == HARP_MODT_IGNORE) break; /* unknown id: ignore */
                    for (int vi = 0; vi < NVOICES; vi++) {
                        synth_voice *mv = &p->voices[vi];
                        if (!harp_mod_targets_voice(ev->voice, mv->active, mv->voice_id)) continue;
                        if (tk == HARP_MODT_BEND) mv->bend_semis = (float)ev->v;   /* X: semitones */
                        else if (tk == HARP_MODT_PRESSURE) mv->z_gain = (float)ev->v; /* Z: loudness gain */
                        else { mv->mod[idx] = ev->v; mv->has_mod = true; }         /* §9.4 param mod */
                    }
                    break;
                }
            }
            continue; /* consumed */
        }
        if (ev->ts < limit && (next == 0 || ev->ts < next)) next = ev->ts;
        g_evq[w++] = *ev;
    }
    g_evq_n = w;
    pthread_mutex_unlock(&g_evq_mu);
    return next;
}

/* A part is "active" (worth rendering) if it sounds now or still rings:
 * a non-empty latch, an arp voice, ANY allocated voice, a still-decaying
 * release tail on any voice, or a pending panic the render must service. The
 * env>1e-4f term keeps release tails alive — env only decays WHILE the part
 * renders, so an active part keeps rendering until every voice falls under the
 * threshold (and is freed), then goes silent and terminates. For ch0-only
 * input, parts 1..15 have no active voice, empty latch, sounding=-1, env=0 ->
 * inactive -> never rendered. */
static bool part_active(const part *p) {
    if (p->arp.nlatch > 0 || p->arp.sounding >= 0) return true;
    if (atomic_load_explicit(&p->panic_note, memory_order_relaxed) != PANIC_NONE)
        return true;
    for (int vi = 0; vi < NVOICES; vi++)
        if (p->voices[vi].active || p->voices[vi].env > 1e-4f) return true;
    return false;
}

/* Render n samples starting at stream position `pos`, splitting at event
 * timestamps so application is sample-accurate (§9.2). Multi-part now (§P2.1):
 * arp steps fire for every part and the segment deadline is the min over all
 * parts. part 0 renders FIRST as the segment's first writer (its notes, or
 * silence when idle now the drone is gone) and parts 1..15 accumulate when
 * active. For ch0-only input this is part0's note render, byte for byte. */
static void render_with_events(float *interleaved, uint32_t n,
                               float rate, uint64_t pos) {
    uint32_t done = 0;
    while (done < n) {
        uint64_t next = evq_apply_due(pos + done, pos + n);
        for (size_t pi = 0; pi < NPARTS; pi++) {
            panic_service(&g_parts[pi]);                  /* drain panic signals */
            arp_fire_due(&g_parts[pi], pos + done, rate); /* steps fire AT seg starts */
        }
        for (size_t pi = 0; pi < NPARTS; pi++) {
            uint64_t anext = arp_next_deadline(&g_parts[pi], pos + done, rate);
            if (anext && (next == 0 || anext < next)) next = anext;
        }
        uint32_t seg = next ? (uint32_t)(next - (pos + done)) : n - done;
        if (seg == 0) seg = 1; /* paranoia: guarantee forward progress */
        if (seg > n - done) seg = n - done;
        /* part 0 renders FIRST (accumulate=false) so it initialises the segment —
         * with its own notes when active, else render_part_voices memsets silence.
         * parts 1..15 then sum (+=) when active. Every part is note-only now the
         * drone is gone; part 0 is the first writer purely by index, not because it
         * carries a continuous voice. (render_part_voices is shared with
         * render_part_slots so the two summations cannot drift.) */
        render_part_voices(&g_parts[0], interleaved + 2 * done, seg, rate, pos + done,
                           false); /* part 0: WRITE (notes, or silence when idle) */
        for (size_t pi = 1; pi < NPARTS; pi++)
            if (part_active(&g_parts[pi]))
                render_part_voices(&g_parts[pi], interleaved + 2 * done, seg, rate,
                                   pos + done, true); /* parts 1..15: accumulate when active */
        done += seg;
    }
}

/* §P2.2 per-part output. Active-slots-out may ask for the main mix (slots
 * 0/1), any part's isolated stereo output (slots 2+2k / 3+2k), or a mix of
 * both. We must NOT advance any voice more than once per segment, so the
 * segment loop here mirrors render_with_events EXACTLY for the shared
 * event/arp application + min-deadline, then renders each NEEDED part ONCE
 * into a stereo scratch and distributes it: into the main-mix accumulator
 * (part0 WRITE then active parts ACCUMULATE, in part order — reproducing
 * render_with_events's part0-write+others-+=) and/or into that part's own
 * output slot pair. Per-part columns are written once with '='; the main-mix
 * columns are copied from the accumulator. Only ONE part scratch + ONE stereo
 * mix accumulator are live at a time (bounded stack), not 16 scratches. */
static void render_part_slots(audio_state *a, float *out, uint32_t n, float rate,
                              uint64_t pos) {
    uint8_t ns = a->n_out_slots;
    /* which output columns want slot 0 / slot 1 (main mix), and which want a
     * given part's L/R — precomputed so the inner packing is a copy */
    bool need_main = false;
    bool need_part[NPARTS] = {false};
    for (uint8_t s = 0; s < ns; s++) {
        uint8_t slot = a->out_slots[s];
        if (slot < 2)
            need_main = true;
        else
            need_part[(slot - 2) / 2] = true; /* slot 2+2k/3+2k -> part k */
    }

    float pscr[2 * AUDIO_MAX_NSAMPLES]; /* one part's stereo render */
    float mix[2 * AUDIO_MAX_NSAMPLES];  /* the reconstructed main mix */

    /* §9.9 meter accumulators for THIS block. Per-part + main; `metered[pi]`
     * records whether part pi produced output this block (the coverage
     * contract). Folding reads pscr/mix AFTER they are written and never feeds
     * back into the render, so the audio bytes are untouched. */
    meter_acc pacc[NPARTS] = {{0}};
    bool metered[NPARTS] = {false};
    meter_acc macc = {0};
    bool main_metered = false;

    uint32_t done = 0;
    while (done < n) {
        /* SHARED event/arp application — IDENTICAL to render_with_events, run
         * EXACTLY ONCE per segment so each voice advances at most once */
        uint64_t next = evq_apply_due(pos + done, pos + n);
        for (size_t pi = 0; pi < NPARTS; pi++) {
            panic_service(&g_parts[pi]);                  /* drain panic signals */
            arp_fire_due(&g_parts[pi], pos + done, rate); /* steps fire AT seg starts */
        }
        for (size_t pi = 0; pi < NPARTS; pi++) {
            uint64_t anext = arp_next_deadline(&g_parts[pi], pos + done, rate);
            if (anext && (next == 0 || anext < next)) next = anext;
        }
        uint32_t seg = next ? (uint32_t)(next - (pos + done)) : n - done;
        if (seg == 0) seg = 1; /* paranoia: guarantee forward progress */
        if (seg > n - done) seg = n - done;

        bool mix_written = false; /* part0 is the first writer of `mix` */
        /* render each needed part ONCE, in part order, distributing its
         * scratch to the main-mix accumulator and/or its own slot pair */
        for (size_t pi = 0; pi < NPARTS; pi++) {
            bool in_mix = need_main && (pi == 0 || part_active(&g_parts[pi]));
            if (!in_mix && !need_part[pi]) continue; /* this part isn't wanted */
            /* Every part is note-only now (no drone). WRITE the scratch
             * (accumulate=false) — signed-zero-preserving — via the SHARED
             * voice-pool summation helper (same as the main mix, so the two paths
             * can't drift). An idle requested part renders to silence
             * (render_part_voices zero-fills its unwritten scratch). */
            render_part_voices(&g_parts[pi], pscr, seg, rate, pos + done, false);
            /* §9.9: fold this part's just-written stereo into its block meter.
             * A pure read of pscr — does not alter pscr, mix, or any slot. */
            meter_acc_fold(&pacc[pi], pscr, seg);
            metered[pi] = true;
            if (in_mix) {
                /* part0 WRITES mix, later active parts ACCUMULATE — same order
                 * and same float ops as render_with_events's main mix */
                for (uint32_t i = 0; i < 2 * seg; i++) {
                    if (!mix_written)
                        mix[i] = pscr[i];
                    else
                        mix[i] += pscr[i];
                }
                mix_written = true;
            }
            if (need_part[pi]) {
                /* pack this part's isolated stereo into every output column
                 * that requested its L/R slot */
                for (uint8_t s = 0; s < ns; s++) {
                    uint8_t slot = a->out_slots[s];
                    if (slot >= 2 && (size_t)((slot - 2) / 2) == pi) {
                        bool right = (slot & 1);
                        for (uint32_t i = 0; i < seg; i++)
                            out[(size_t)(done + i) * ns + s] = pscr[2 * i + right];
                    }
                }
            }
        }
        /* if main was requested but nothing fed `mix` (can't happen: part0 is
         * always in_mix when need_main), guard against reading uninitialised */
        if (need_main && !mix_written) memset(mix, 0, (size_t)2 * seg * sizeof(float));
        /* §9.9: fold the just-assembled main-mix segment into its block meter.
         * Read-only on `mix`; runs only when the mix was actually produced. */
        if (need_main) {
            meter_acc_fold(&macc, mix, seg);
            main_metered = true;
        }
        /* pack the main-mix columns (slots 0/1) from the accumulator */
        if (need_main)
            for (uint8_t s = 0; s < ns; s++) {
                uint8_t slot = a->out_slots[s];
                if (slot < 2)
                    for (uint32_t i = 0; i < seg; i++)
                        out[(size_t)(done + i) * ns + s] = mix[2 * i + slot];
            }
        done += seg;
    }

    /* §9.9 commit the block's meters (coverage contract): a part that produced
     * output this block gets its peak/RMS; a part that did NOT render (inactive,
     * or not requested) decays to the silent floor so a stale peak never sticks.
     * The main-mix slot updates only when the mix was assembled (need_main);
     * otherwise it keeps its prior value (the host didn't ask for the mix). */
    for (size_t pi = 0; pi < NPARTS; pi++) {
        if (metered[pi])
            meter_acc_commit((int)pi, &pacc[pi]);
        else {
            atomic_store_explicit(&g_meter_peak[pi], 0.0f, memory_order_relaxed);
            atomic_store_explicit(&g_meter_rms[pi], 0.0f, memory_order_relaxed);
        }
    }
    if (main_metered) meter_acc_commit(METER_MAIN_IX, &macc);
}

/* The render seam (device.h): the audio render+emit loop lives in audio_loop.c
 * (harpdevice) and reaches the synth ONLY through these two — reset every part's
 * voices, and render n samples at stream position pos. A downstream daemon supplies its own pair; the refdev's are below, unchanged. */
void engine_voices_cold(void) { for (size_t i = 0; i < NPARTS; i++) part_voices_cold(&g_parts[i]); }

/* §9.3: the refdev's advertised param map is fixed — it never re-announces. */
int engine_param_map_dirty_take(void) { return 0; }

/* stream-stop quieting (called by audio_stop after the render thread is joined):
 * free every voice and clear any pending panic so nothing leaks into the next
 * stream. NOT a cold reset (no zero/alloc_clock) — keep byte-identical. */
void engine_voices_quiet(void) {
    for (size_t i = 0; i < NPARTS; i++) {
        for (int vi = 0; vi < NVOICES; vi++) {
            g_parts[i].voices[vi].note = -1;
            g_parts[i].voices[vi].active = false;
        }
        atomic_store_explicit(&g_parts[i].panic_note, PANIC_NONE, memory_order_relaxed);
    }
}

/* §8.8 engine role. The reference engine is a SYNTH (note → audio); it has no audio input,
 * so it returns 0 and `a->fx_in` is never demuxed — the golden render path is byte-identical.
 * An EFFECT engine (e.g. harp-fx's reverb, which replaces this translation unit like
 * jetson-synth does) overrides this to 1; the device then advertises `audio.fx` (§6.2) and
 * audio_loop.c's host_paced_loop demuxes the H→D input into `a->fx_in` for render_output. */
int engine_is_fx(void) { return 0; }

uint16_t render_output(audio_state *a, float *out, uint32_t n, float rate,
                       uint64_t pos) {
    /* TEST/MEASUREMENT tone (harp-deviced --tone HZ): a pure stereo sine,
     * phase-continuous across blocks (pos is the absolute sample index), at
     * -6 dBFS so it can't clip. A clean SINAD reference for the free-running
     * RTP path, where the rich drone reads conservatively. Gated on tone_hz>0,
     * which is 0 on every production/golden run — the path below is untouched. */
    if (a->tone_hz > 0.0) {
        double w = 2.0 * M_PI * a->tone_hz / (double)rate;
        for (uint32_t i = 0; i < n; i++) {
            float s = (float)sin(w * (double)(pos + i)) * 0.5f;
            out[2 * i] = s;
            out[2 * i + 1] = s;
        }
        return 2; /* stereo */
    }
    /* DEFAULT / main-mix fast path: the host wants exactly the stereo main mix
     * in slot order {0,1}. Route straight through the UNCHANGED P2.1
     * render_with_events into the 2-channel `out` buffer — byte-identical
     * output, golden hashes hold. The per-part branch is purely additive. */
    if (a->n_out_slots == 2 && a->out_slots[0] == 0 && a->out_slots[1] == 1) {
        render_with_events(out, n, rate, pos);
        /* §9.9 main-mix meter on the golden path: fold the ALREADY-WRITTEN
         * stereo output. This is a read of `out` AFTER render_with_events
         * returned — it cannot change a single rendered byte, so the golden /
         * groove hashes are untouched. Per-part meters are unavailable here (no
         * per-part scratch exists on this fast path — by construction, to keep
         * it byte-identical); the pump emits the silent floor for parts in this
         * mode. The main mix is what the host hears on slots 0/1, so that is the
         * meter that matters in the default streaming case. */
        meter_acc macc = {0};
        meter_acc_fold(&macc, out, n);
        meter_acc_commit(METER_MAIN_IX, &macc);
        return 2;
    }
    /* PER-PART case: any requested slot >= 2, or a non-{0,1} arrangement */
    render_part_slots(a, out, n, rate, pos);
    return a->n_out_slots;
}


