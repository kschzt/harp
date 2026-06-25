/* Linker shims for fuzz-evt. fuzz/fuzz_evt.c TU-includes device/session.c so the static
 * handle_evt_msg (the §9.2 EVT-stream parser) is the REAL production code, not a copy. But
 * session.c's OTHER functions (the audio thread, snapshot/closure walk, store I/O, the front
 * panel, reconcile) reference engine/store symbols that live in engine.c / store.c / panel.c —
 * pulling those whole TUs in would drag the audio threads, libsamplerate and pthread state into
 * a parser fuzzer for no benefit. Whole-TU compilation still needs every symbol session.c
 * references to resolve, so we supply them here. Mirrors fuzz/state_stubs.c, scaled up.
 *
 * Two classes:
 *   - REACHED on the fuzz path (the parser actually calls them): evq_push / evq_push_batch /
 *     evq_full and the note-off escalation (engine_all_notes_off / engine_note_off_if) and
 *     live_ref_touch. These are NO-OP stubs — the fuzzer only asserts the PARSER survives bad
 *     bytes; it needs no real engine/queue behavior (g_evq_drops et al. stay 0). evq_full
 *     returns false so the never-drop-a-note-off escalation path is also exercised by the
 *     parser without a real ring.
 *   - NEVER reached (handle_evt_msg's grammar never routes here): everything else is an
 *     abort-stub — a loud failure, not silent UB, if a future change ever wires the fuzzer
 *     through them.
 *
 * The one REAL datum is g_params (the parser-adjacent codecs match blob ids against it); kept
 * byte-identical to state_stubs.c / engine.c, with the same drift tripwire. Constraint: this
 * file lives in fuzz/ so the parser stays fuzzable in isolation without editing the device tree
 * (another task owns that). */
#include <stdlib.h>

#include "device.h"

/* ── REAL: the param id table (ids 1..12, CONTIGUOUS — the 2.1.0 renumber reclaimed the drone's
 *    old id 7). Mirrored from engine.c / state_stubs.c; only the id field is load-bearing here. ── */
dev_param g_params[NPARAMS] = {
    {1, "Osc Pitch", 0, NULL, 0.5f},     {2, "Osc Shape", 0, NULL, 0.5f},
    {3, "Filter Cutoff", 0, NULL, 0.5f}, {4, "Filter Reso", 0, NULL, 0.5f},
    {5, "Env Attack", 0, NULL, 0.5f},    {6, "Env Release", 0, NULL, 0.5f},
    {7, "Master Level", 0, NULL, 0.5f},
    {8, "Arp Mode", 5, NULL, 0.0f},      {9, "Arp Division", 6, NULL, 0.6f},
    {10, "Arp Gate", 0, NULL, 0.5f},     {11, "Arp Octaves", 4, NULL, 0.0f},
    {12, "Glide", 0, NULL, 0.0f},
};
_Static_assert(NPARAMS == 12, "evt_stubs.c g_params mirror is out of sync with engine.c");

/* ── global counters/gauges session.c references (real, so CTR_INC has somewhere to land; the
 *    fuzz path leaves them 0). Types match device.h. ── */
_Atomic uint64_t g_evt_late;
_Atomic uint64_t g_ramp_late;
_Atomic uint64_t g_evq_drops;
_Atomic uint32_t g_evt_consumed;
_Atomic uint64_t g_fence_waits;
_Atomic uint64_t g_fence_timeouts;
_Atomic int g_touch_pending;
_Atomic float g_meter_peak[METER_NSLOTS];
_Atomic float g_meter_rms[METER_NSLOTS];

/* ── REACHED on the fuzz path: no-op so the parser runs without a real engine/queue ── */
void evq_push(dev_event ev) { (void)ev; }
bool evq_push_batch(const dev_event *evs, size_t count) {
    (void)evs;
    (void)count;
    return true; /* "the whole batch landed" — keeps txn_commit's all-or-nothing path linear */
}
bool evq_full(void) { return false; } /* never full -> the note-off escalation branch is not taken */
void evq_reset_for_new_stream(void) {}
void engine_all_notes_off(void) {}
void engine_note_off_if(uint32_t note) { (void)note; }
void live_ref_touch(device *d, bool dirty) {
    (void)d;
    (void)dirty;
}

/* ── NEVER reached on the evt-stream parse path: abort-stubs ── */
int param_index(uint32_t id) {
    (void)id;
    abort();
}
float engine_part_param_get(int part_idx, uint32_t id) {
    (void)part_idx;
    (void)id;
    abort();
}
void engine_part_param_put(int part_idx, uint32_t id, float v) {
    (void)part_idx;
    (void)id;
    (void)v;
    abort();
}
/* NB: evt_echo_param, ntf_state_changed, ntf_core_changed are DEFINED in session.c itself
 * (compiled into this fuzz TU via the #include), so they are NOT stubbed here. */
void encode_param_array(harp_cbuf *b) {
    (void)b;
    abort();
}
int engine_load_snapshot(device *d, const harp_hash *snap_h) {
    (void)d;
    (void)snap_h;
    abort();
}
void engine_meters_reset(void) { abort(); }
int do_snapshot(device *d, const char *msg, harp_hash *out, uint64_t *out_gen) {
    (void)d;
    (void)msg;
    (void)out;
    (void)out_gen;
    abort();
}
void closure_walk(struct closure_ctx *ctx, const harp_hash *h) {
    (void)ctx;
    (void)h;
    abort();
}
void live_cache_flush(device *d) {
    (void)d;
    abort();
}
bool front_panel_set(device *d, uint32_t id, double v) {
    (void)d;
    (void)id;
    (void)v;
    abort();
}
void reconcile_post_offer(const char *expect, const char *live, int dirty) {
    (void)expect;
    (void)live;
    (void)dirty;
    abort();
}
void reconcile_read(int *pending, char *expect12, char *live12, int *dirty, int *choice) {
    (void)pending;
    (void)expect12;
    (void)live12;
    (void)dirty;
    (void)choice;
    abort();
}
void *audio_thread(void *arg) {
    (void)arg;
    abort();
}
void audio_stop(device *d) {
    (void)d;
    abort();
}
int audio_open_rtp_dest(uint32_t peer_ip_net, int port) {
    (void)peer_ip_net;
    (void)port;
    abort();
}
void audio_rtp_close(audio_state *a) {
    (void)a;
    abort();
}
