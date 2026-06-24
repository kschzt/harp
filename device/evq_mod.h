/* evq_mod.h — §9.4/§9.5 modulation ROUTING + ADDRESSING as pure functions.
 *
 * A DEV_EV_MOD event applies a non-destructive, signed offset to sounding voices.
 * Two decisions govern where it lands, and both are part of the determinism oracle
 * (a per-voice MPE expression — pitch bend / pressure — must reach exactly the right
 * voice, identically on every board, from the identical ordered event stream):
 *
 *   1. WHICH voice(s): §9.5 addressing. A voice key (ev_voice) != 0 targets the ONE
 *      active voice whose key matches (a mod to a gone voice is ignored); key == 0 is
 *      "whole-part" — every active voice modulates (the channel-level §9.4 reading).
 *   2. WHICH target: a mod id routes to a dedicated per-voice field (pitch bend ->
 *      bend_semis, pressure -> z_gain) or to the §9.4 param mod[] layer; an unknown
 *      id is ignored.
 *
 * engine.c's evq_apply_due() DEV_EV_MOD case was the only place this lived, on the
 * daemon-private synth_voice type reachable from no host target — so it was only ever
 * exercised on hardware. Factoring the two decisions out over PLAIN scalars (no
 * device.h, no daemon types, the bend/pressure ids passed in) makes them unit-testable
 * in CI on every push while engine.c's real mod path calls THESE — so a regression in
 * MPE voice targeting is caught off-hardware, before it ships a wrong-but-deterministic
 * render. Mirrors voice_alloc.h / arp_select.h.
 */
#ifndef HARP_DEVICE_EVQ_MOD_H
#define HARP_DEVICE_EVQ_MOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* §P2.1 event ROUTING: which part a channel-tagged event lands on — the channel
 * modulo the part count (channel 0 -> part 0, so a ch0 --set still hits part 0; a
 * channel >= nparts wraps). Pure; `nparts` is the device NPARTS, passed in. */
static inline size_t harp_evt_part(uint32_t channel, size_t nparts) {
    return (size_t)(channel % (uint32_t)nparts);
}

/* §9.5 per-voice mod addressing: does a DEV_EV_MOD carrying voice key `ev_voice`
 * apply to a voice that is `v_active` with key `v_voice_id`? Pure: a function of the
 * three scalars. ev_voice == 0 => whole-part (any active voice); else only the active
 * voice whose key matches. An inactive voice never takes a mod. */
static inline bool harp_mod_targets_voice(uint32_t ev_voice, bool v_active,
                                          uint32_t v_voice_id) {
    if (!v_active) return false;
    return ev_voice == 0 || v_voice_id == ev_voice;
}

/* §9.5 mod TARGET classification. A mod id is either a dedicated per-voice expression
 * axis (pitch bend / pressure) or a normalized param-mod (when param_idx >= 0); any
 * other id is ignored. `bend_id`/`pressure_id` are the wire constants (HARP_MOD_PITCH_BEND
 * / HARP_MOD_PRESSURE) passed in to keep this header free of the device/protocol headers;
 * `param_idx` is the caller's param_index(ev_a) (< 0 when ev_a is not a real param). */
typedef enum {
    HARP_MODT_IGNORE = 0, /* unknown id — drop */
    HARP_MODT_BEND,       /* X axis: voice bend_semis (semitones, unclamped) */
    HARP_MODT_PRESSURE,   /* Z axis: voice z_gain (loudness) */
    HARP_MODT_PARAM,      /* §9.4 param mod[] layer at param_idx */
} harp_mod_target_kind;

static inline harp_mod_target_kind harp_mod_target(uint32_t ev_a, int param_idx,
                                                   uint32_t bend_id, uint32_t pressure_id) {
    if (ev_a == bend_id) return HARP_MODT_BEND;
    if (ev_a == pressure_id) return HARP_MODT_PRESSURE;
    if (param_idx >= 0) return HARP_MODT_PARAM;
    return HARP_MODT_IGNORE;
}

/* §7.1 stale-epoch rule: an event — or an inner tstamp instant (ramp-end §9.4, txn-commit
 * §9.6) — timestamped in an epoch OLDER than the device's current epoch MUST be discarded and
 * counted (evt_stale_epoch, §14.2). epoch 0 = "now" (always current); a future epoch shouldn't
 * arrive but is not stale. Pure predicate so the outer-envelope guard and both inner-instant
 * guards in handle_evt_msg share one tested rule. */
static inline bool harp_evt_epoch_stale(uint64_t ev_epoch, uint32_t cur_epoch) {
    return ev_epoch != 0 && ev_epoch < cur_epoch;
}

/* §9.5/§9.6: does a decoded set/ramp/mod event DIRTY the persistent live ref (firing state.changed
 * + advancing the §11.3 closure hash)? Pure over the wire scalars. Only a WHOLE-PART (voice == 0)
 * set (etype 1) or ramp (etype 5) dirties; a per-voice (voice != 0) set/ramp is transient (§9.5) and
 * a mod (etype 6) is non-destructive (§9.4) — neither dirties; a txn-buffered edit (have_txn) defers
 * all dirtying to commit (§9.6). Mirrors engine.c's apply side, which sets g_touch_pending only for
 * voice == 0. */
static inline bool harp_evt_dirties_live(uint32_t etype, uint32_t voice, bool have_txn) {
    if (have_txn) return false;      /* §9.6: buffered — dirties only at commit */
    if (voice != 0) return false;    /* §9.5: per-voice is transient */
    return etype == 1 || etype == 5; /* whole-part set or ramp (mod/etype 6 is non-destructive) */
}

#endif /* HARP_DEVICE_EVQ_MOD_H */
