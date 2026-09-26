/* fx_filter.c — a minimal HARP §8.8 `audio.fx` EFFECT engine: a resonant lowpass.
 *
 * This file is the whole device-specific part of an effect. It replaces the refdev's
 * device/engine.c in a harp-deviced build and implements the engine seam declared in
 * device/device.h; the protocol session, content-addressed state + recall, transports
 * and the §8.3/§8.7 audio loop are harp's own, unmodified. See README.md next to this
 * file for the walkthrough.
 *
 * What makes it an EFFECT (§8.8):
 *   - engine_is_fx() returns 1, so session.c advertises `audio.fx` in the identity
 *     and audio_loop.c demuxes the host's H->D input columns into a->fx_in;
 *   - render_output() reads a->fx_in and writes WET ONLY. The host keeps the dry
 *     signal and does the dry/wet mix (the dry never crosses the wire).
 *
 * What every engine owns, synth or effect:
 *   - the param bank (g_params + a per-part value grid) — the §9.3 param map and
 *     the values recall snapshots and restores;
 *   - the §9.2 timestamped event queue, applied on the render thread AT the sample
 *     each event names: that is what makes DAW automation sample-accurate.
 *
 * An effect has one signal path. The seam is multitimbral (NPARTS value grids, so a
 * recall bundle round-trips unchanged), but only part 0 is audible here.
 */
#include <math.h>
#include <pthread.h>
#include <string.h>

#include "device.h"
#include "evq_mod.h" /* harp_evt_part: event channel -> part (same routing as the refdev) */

/* ---------------- the param bank (§9.3) ----------------
 * NPARAMS is a compile-time constant shared with state.c/session.c, so the example's
 * CMake target builds the device sources with -DNPARAMS=2 (see CMakeLists.txt). The
 * ids, names and defaults ARE the param map: its hash (§9.3) is what recall checks, so
 * changing any of them is an engine-version event (ENGINE_VERSION, §13.4). */
_Static_assert(NPARAMS == 2, "fx-filter defines exactly two params (build with -DNPARAMS=2)");
enum { P_CUTOFF = 1, P_RESO = 2 };
#define DEF_CUTOFF 0.7f /* ~2.5 kHz */
#define DEF_RESO 0.2f

dev_param g_params[NPARAMS] = {
    {P_CUTOFF, "Cutoff", 0, NULL, DEF_CUTOFF},
    {P_RESO, "Resonance", 0, NULL, DEF_RESO},
};

/* Per-part values. Cross-thread (render ramps, session loads, panel knobs), so
 * _Atomic, relaxed, last-write-wins — ordering for timestamped changes comes from
 * the event queue, exactly as in the refdev. Every part starts at the defaults
 * (state.c asserts engine_part_param_get(0, id) == g_params[].def at boot). */
static _Atomic float g_pval[NPARTS][NPARAMS] = {[0 ... NPARTS - 1] = {DEF_CUTOFF, DEF_RESO}};

int param_index(uint32_t id) {
    for (size_t i = 0; i < NPARAMS; i++)
        if (g_params[i].id == id) return (int)i;
    return -1;
}
static float pget(size_t part, size_t idx) {
    return atomic_load_explicit(&g_pval[part][idx], memory_order_relaxed);
}
static void pput(size_t part, size_t idx, float v) {
    atomic_store_explicit(&g_pval[part][idx], v, memory_order_relaxed);
}
float engine_part_param_get(int part, uint32_t id) {
    int idx = param_index(id);
    return (part < 0 || part >= NPARTS || idx < 0) ? 0.0f : pget((size_t)part, (size_t)idx);
}
void engine_part_param_put(int part, uint32_t id, float v) {
    int idx = param_index(id);
    if (part >= 0 && part < NPARTS && idx >= 0) pput((size_t)part, (size_t)idx, v);
}
/* the map is fixed at build time — never re-announced mid-session (§9.3) */
int engine_param_map_dirty_take(void) { return 0; }

/* ---------------- counters + meters the session reads (§14, §9.9) ---------------- */
_Atomic int g_touch_pending; /* stored param state changed -> session re-snapshots + echoes */
_Atomic uint64_t g_evq_drops, g_evt_late, g_ramp_late;
_Atomic uint32_t g_evt_consumed; /* §8.3.1 fence: bumped by the session, reset per stream */
_Atomic uint64_t g_fence_waits, g_fence_timeouts;
_Atomic float g_meter_peak[METER_NSLOTS];
_Atomic float g_meter_rms[METER_NSLOTS];

void engine_meters_reset(void) {
    for (int i = 0; i < METER_NSLOTS; i++) {
        atomic_store_explicit(&g_meter_peak[i], 0.0f, memory_order_relaxed);
        atomic_store_explicit(&g_meter_rms[i], 0.0f, memory_order_relaxed);
    }
}
/* the refdev's opt-in fence instrumentation (HARP_FENCE_INSTRUMENT); not needed here */
void engine_fence_instr_reset(void) {}
void engine_fence_instr_dump(void) {}

/* ---------------- the event queue (§9.2) ----------------
 * Session threads push; the render thread applies each event at its timestamp. */
static dev_event g_evq[DEV_EVQ_CAP];
static size_t g_evq_n; /* under g_evq_mu */
static pthread_mutex_t g_evq_mu = PTHREAD_MUTEX_INITIALIZER;

/* independent events: per-event fill, drops counted (never silent, §14.1) */
void evq_push_run(const dev_event *evs, size_t count) {
    pthread_mutex_lock(&g_evq_mu);
    for (size_t i = 0; i < count; i++) {
        if (g_evq_n < DEV_EVQ_CAP) g_evq[g_evq_n++] = evs[i];
        else CTR_INC(g_evq_drops);
    }
    pthread_mutex_unlock(&g_evq_mu);
}
void evq_push(dev_event ev) { evq_push_run(&ev, 1); }
/* §9.6 transaction commit: all-or-nothing */
bool evq_push_batch(const dev_event *evs, size_t count) {
    pthread_mutex_lock(&g_evq_mu);
    bool ok = g_evq_n + count <= DEV_EVQ_CAP;
    for (size_t i = 0; i < count; i++) {
        if (ok) g_evq[g_evq_n++] = evs[i];
        else CTR_INC(g_evq_drops);
    }
    pthread_mutex_unlock(&g_evq_mu);
    return ok;
}
bool evq_full(void) {
    pthread_mutex_lock(&g_evq_mu);
    bool full = g_evq_n >= DEV_EVQ_CAP;
    pthread_mutex_unlock(&g_evq_mu);
    return full;
}

/* §9.4 automation ramps (one in flight per part+param) and non-destructive
 * modulation offsets. Render-thread-only. */
typedef struct {
    bool active;
    uint64_t start, end;
    float from, to;
} ramp;
static ramp g_ramps[NPARTS][NPARAMS];
static float g_mod[NPARTS][NPARAMS];

/* ---------------- the DSP: stereo TPT state-variable lowpass ----------------
 * Zavalishin's topology-preserving SVF: stable for every cutoff below Nyquist and
 * every resonance, and well-behaved under fast modulation — automation can sweep
 * it at control rate without blowing up. */
typedef struct {
    float ic1[2], ic2[2]; /* integrator states, L/R */
} svf;
static svf g_svf;

/* An optional REAL host-paced pipeline (harp-deviced --pipeline N, audio_state.pipeline): the
 * wet leaves N samples after it was rendered, and the device declares N as §6.4 key 3 — so
 * this example can stand in for a deep-pipeline hardware effect in tests. 0 = none. */
#define PIPE_MAX (1u << 16)
static float g_pipe[2 * PIPE_MAX];
static uint32_t g_pipe_w;

void engine_voices_cold(void) { /* audio.start: clean state */
    memset(&g_svf, 0, sizeof g_svf);
    memset(g_pipe, 0, sizeof g_pipe);
    g_pipe_w = 0;
}
void engine_voices_quiet(void) {}                                  /* no voices to free */
/* Panic paths (CC 120/123, panel): an effect has no notes, so there is nothing to
 * release on the session thread. A queued DEV_EV_ALL_OFF clears the filter state on
 * the render thread (below), which is what silences a self-ringing resonance. */
void engine_all_notes_off(void) {}
void engine_note_off_if(uint32_t note) { (void)note; }

void evq_reset_for_new_stream(void) {
    /* a new stream is a new time domain: queued events and ramps from the old one are stale */
    pthread_mutex_lock(&g_evq_mu);
    g_evq_n = 0;
    pthread_mutex_unlock(&g_evq_mu);
    memset(g_ramps, 0, sizeof g_ramps);
    memset(g_mod, 0, sizeof g_mod);
    atomic_store_explicit(&g_evt_consumed, 0, memory_order_release);
}

/* Apply every event due at or before `pos`. Returns the timestamp of the earliest
 * event still pending (UINT64_MAX if none) so render_output can split the block
 * there and land it on its exact sample. */
static uint64_t evq_apply_due(uint64_t pos) {
    uint64_t next = UINT64_MAX;
    pthread_mutex_lock(&g_evq_mu);
    size_t w = 0;
    for (size_t r = 0; r < g_evq_n; r++) {
        const dev_event *ev = &g_evq[r];
        if (ev->ts > pos) {
            if (ev->ts < next) next = ev->ts;
            g_evq[w++] = *ev;
            continue;
        }
        /* §9.2/§14.2 lateness: a ramp's deadline is its END (its start is the previous
         * automation point, legitimately past); everything else's is its ts. */
        if (ev->kind == DEV_EV_RAMP) {
            if (ev->end && ev->end < pos) CTR_INC(g_ramp_late);
        } else if (ev->ts && ev->ts < pos)
            CTR_INC(g_evt_late);

        size_t part = harp_evt_part(ev->channel, NPARTS);
        int idx = param_index(ev->a);
        switch (ev->kind) {
            case DEV_EV_PARAM_SET:
                if (ev->voice || idx < 0) break; /* no voices; unknown id ignored */
                pput(part, (size_t)idx, ev->v);
                g_ramps[part][idx].active = false; /* a set supersedes a ramp */
                atomic_store_explicit(&g_touch_pending, 1, memory_order_release);
                break;
            case DEV_EV_RAMP:
                if (ev->voice || idx < 0) break;
                g_ramps[part][idx] = (ramp){true, pos, ev->end, pget(part, (size_t)idx), ev->v};
                atomic_store_explicit(&g_touch_pending, 1, memory_order_release);
                break;
            case DEV_EV_MOD:
                /* §9.4 part-wide modulation: an additive offset on the stored value,
                 * clamped after summing, never written to state (no touch). A
                 * voice-addressed mod has no voice to land on and is ignored (§9.5). */
                if (!ev->voice && idx >= 0) g_mod[part][idx] = ev->v;
                break;
            case DEV_EV_ALL_OFF:
                memset(&g_svf, 0, sizeof g_svf);
                break;
            default: /* notes, transport: nothing to do for this effect */
                break;
        }
    }
    g_evq_n = w;
    pthread_mutex_unlock(&g_evq_mu);
    return next;
}

/* Advance part ramps to `pos`. Ramps move the STORED value (§9.4), so recall and
 * the front-panel echo see the automated value, not just the audio. */
static void ramps_advance(uint64_t pos) {
    for (size_t p = 0; p < NPARTS; p++)
        for (size_t i = 0; i < NPARAMS; i++) {
            ramp *r = &g_ramps[p][i];
            if (!r->active) continue;
            float v = r->to;
            if (pos < r->end && r->end > r->start) {
                if (pos <= r->start) continue;
                v = r->from + (r->to - r->from) * (float)(pos - r->start) / (float)(r->end - r->start);
            } else
                r->active = false;
            pput(p, i, v);
        }
}

/* value the DSP hears: stored value + modulation, clamped to the normalized range */
static float effective(size_t idx) {
    float v = pget(0, idx) + g_mod[0][idx];
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

/* Filter `n` frames of input starting at frame `off` into the stereo scratch.
 * Coefficients are computed once per segment (<= CTRL_BLOCK frames) — the declared
 * control rate — from the parameter values at the segment start. */
static void svf_run(const float *inL, const float *inR, uint32_t avail, float *wet,
                    uint32_t off, uint32_t n, float rate) {
    /* Cutoff: 20 Hz .. 20 kHz, exponential; kept under 0.45*rate so tan() stays sane
     * at low sample rates. Resonance: Q 0.5 .. 20 (k = 1/Q, 2 .. 0.05). */
    float fc = 20.0f * powf(1000.0f, effective(0));
    if (fc > 0.45f * rate) fc = 0.45f * rate;
    float g = tanf((float)M_PI * fc / rate);
    float k = 2.0f - 1.95f * effective(1);
    float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    for (uint32_t s = off; s < off + n; s++) {
        float x[2] = {s < avail ? inL[s] : 0.0f, s < avail ? inR[s] : 0.0f};
        for (unsigned c = 0; c < 2; c++) {
            float v3 = x[c] - g_svf.ic2[c];
            float v1 = a1 * g_svf.ic1[c] + a2 * v3;
            float v2 = g_svf.ic2[c] + a2 * g_svf.ic1[c] + a3 * v3;
            g_svf.ic1[c] = 2.0f * v1 - g_svf.ic1[c];
            g_svf.ic2[c] = 2.0f * v2 - g_svf.ic2[c];
            wet[2 * s + c] = v2; /* lowpass output */
        }
    }
    /* Denormals: when the track goes silent the integrators decay toward zero through the
     * subnormal range, where x86 (without FTZ/DAZ) runs 10-100x slower — a live insert's
     * device would stall exactly when it idles. Flush a state that small to exact zero. */
    for (unsigned c = 0; c < 2; c++) {
        if (fabsf(g_svf.ic1[c]) < 1e-20f) g_svf.ic1[c] = 0.0f;
        if (fabsf(g_svf.ic2[c]) < 1e-20f) g_svf.ic2[c] = 0.0f;
    }
}

int engine_is_fx(void) { return 1; }

#define CTRL_BLOCK 32 /* control rate: 1.5 kHz at 48 kHz, as the refdev */

uint16_t render_output(audio_state *a, float *out, uint32_t n, float rate, uint64_t pos) {
    /* INPUT: the host's columns, in audio.start in-slot order. Slot 0 is L, slot 1
     * is R; a mono host (one column) feeds both. No input declared (a synth-style
     * host, or the free-running path between blocks) -> the filter rings out on
     * silence. */
    static const float zeros[AUDIO_MAX_NSAMPLES];
    const float *inL = zeros, *inR = zeros;
    uint32_t avail = 0;
    if (a->fx_in && a->n_in_slots > 0) {
        int cl = 0, cr = -1;
        for (int c = 0; c < a->n_in_slots; c++) {
            if (a->in_slots[c] == 0) cl = c;
            if (a->in_slots[c] == 1) cr = c;
        }
        inL = a->fx_in + (size_t)cl * AUDIO_MAX_NSAMPLES;
        inR = cr >= 0 ? a->fx_in + (size_t)cr * AUDIO_MAX_NSAMPLES : inL;
        avail = a->fx_in_n;
    }

    /* WET, split at event timestamps (sample-accurate) and at the control rate */
    float wet[2 * AUDIO_MAX_NSAMPLES];
    for (uint32_t done = 0; done < n;) {
        uint64_t next = evq_apply_due(pos + done);
        ramps_advance(pos + done);
        uint32_t seg = n - done;
        if (seg > CTRL_BLOCK) seg = CTRL_BLOCK;
        if (next - pos - done < seg) seg = (uint32_t)(next - pos - done); /* next > pos+done */
        svf_run(inL, inR, avail, wet, done, seg, rate);
        done += seg;
    }

    if (a->pipeline) { /* the declared pipeline, for real: the wet trails by exactly N more */
        uint32_t d = a->pipeline < PIPE_MAX ? a->pipeline : PIPE_MAX - 1;
        for (uint32_t s = 0; s < n; s++) {
            uint32_t w = g_pipe_w, r = (g_pipe_w - d) & (PIPE_MAX - 1);
            g_pipe[2 * w] = wet[2 * s];
            g_pipe[2 * w + 1] = wet[2 * s + 1];
            wet[2 * s] = g_pipe[2 * r];
            wet[2 * s + 1] = g_pipe[2 * r + 1];
            g_pipe_w = (g_pipe_w + 1) & (PIPE_MAX - 1);
        }
    }

    /* OUTPUT: pack the requested slots. 0/1 = main L/R and 2/3 = part 0 L/R (the
     * same wet signal); any other part's slots are silent. */
    uint8_t ns = a->n_out_slots ? a->n_out_slots : 2;
    for (uint8_t c = 0; c < ns; c++) {
        uint8_t slot = a->n_out_slots ? a->out_slots[c] : c;
        bool live = slot < 4;
        unsigned ch = slot & 1u;
        for (uint32_t s = 0; s < n; s++) out[(size_t)s * ns + c] = live ? wet[2 * s + ch] : 0.0f;
    }

    /* §9.9 meters: main mix and part 0 both carry the wet */
    float peak = 0.0f;
    double sumsq = 0.0;
    for (uint32_t i = 0; i < 2 * n; i++) {
        float v = wet[i];
        if (!isfinite(v)) continue;
        if (fabsf(v) > peak) peak = fabsf(v);
        sumsq += (double)v * v;
    }
    float rms = n ? (float)sqrt(sumsq / (2.0 * n)) : 0.0f;
    const int ix[2] = {METER_MAIN_IX, 0};
    for (int m = 0; m < 2; m++) {
        atomic_store_explicit(&g_meter_peak[ix[m]], peak > 1e-20f ? peak : 0.0f, memory_order_relaxed);
        atomic_store_explicit(&g_meter_rms[ix[m]], rms > 1e-20f ? rms : 0.0f, memory_order_relaxed);
    }
    return ns;
}
