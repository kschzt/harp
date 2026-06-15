/* Linker shims for fuzz-state. device/state.c is compiled into the
 * fuzz-state target so the params-blob codec is the real production code,
 * not a copy. But state.c's OTHER functions (snapshot capture, load,
 * front-panel set, the closure walk) reference engine and session symbols
 * that live in engine.c / session.c — pulling those whole translation
 * units in would drag the audio threads, the event queue and pthread state
 * into a parser fuzzer for no benefit.
 *
 * Whole-object-file linking still needs every symbol state.c references to
 * resolve, so we supply them here. The fuzz target (fuzz_state.c) calls
 * ONLY refdev_parse_params_blob, whose sole external datum is the g_params
 * id table — that one is REAL (byte-identical to engine.c so the id-match
 * path in the parser is exercised for actual ids). Everything else is an
 * abort-stub: never reached on the fuzz path, and a loud failure (not silent
 * UB) if a future change ever routes the fuzzer through them.
 *
 * Constraint: this file lives in fuzz/ precisely so the codec stays
 * fuzzable in isolation without editing the device tree (another task owns
 * that). */
#include <stdlib.h>

#include "device.h"

/* REAL: the parser matches blob ids against g_params[].id. Kept in lockstep
 * with device/engine.c (ids 1..13, ascending) — only the id field matters to
 * the codec, but the whole rows are mirrored so a drift is obvious. */
dev_param g_params[NPARAMS] = {
    {1, "Osc Pitch", 0, NULL, 0.5f},     {2, "Osc Shape", 0, NULL, 0.5f},
    {3, "Filter Cutoff", 0, NULL, 0.5f}, {4, "Filter Reso", 0, NULL, 0.5f},
    {5, "Env Attack", 0, NULL, 0.5f},    {6, "Env Release", 0, NULL, 0.5f},
    {7, "Drone Mix", 0, NULL, 0.5f},     {8, "Master Level", 0, NULL, 0.5f},
    {9, "Arp Mode", 5, NULL, 0.0f},      {10, "Arp Division", 6, NULL, 0.6f},
    {11, "Arp Gate", 0, NULL, 0.5f},     {12, "Arp Octaves", 4, NULL, 0.0f},
    {13, "Glide", 0, NULL, 0.0f},
};
/* Drift tripwire: this mirror is hand-kept in lockstep with engine.c. A param
 * COUNT change there bumps NPARAMS and trips this assert, forcing the rows
 * above to be updated rather than silently zero-padded. */
_Static_assert(NPARAMS == 13, "state_stubs.c g_params mirror is out of sync with engine.c");

/* Never reached on the fuzz path (refdev_parse_params_blob touches none of
 * these). abort() rather than a quiet return so a wiring mistake surfaces. */
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
void evt_echo_param(device *d, uint32_t id, float v, uint8_t channel) {
    (void)d;
    (void)id;
    (void)v;
    (void)channel;
    abort();
}
void ntf_state_changed(device *d, const harp_ref *r) {
    (void)d;
    (void)r;
    abort();
}
