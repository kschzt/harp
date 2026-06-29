/* Unit tests for §9.3 param-map-hash change-detection (the FROZEN GOLDEN that
 * protects stored automation lanes).
 *
 * CONTRACT (§9.3): param-map-hash MUST change iff the automatable param set
 * changes in a way that invalidates stored automation — an id's meaning, a
 * stepped param's step count, a param's removal. It MUST NOT change for an
 * informative-only edit: a label/name tweak, or adding/removing a readonly
 * meter (§9.9). A host keys its stored automation off this hash; a spurious
 * change orphans good automation, a missed change silently misroutes it.
 *
 * MECHANISM under test: the device hashes the AUTOMATABLE subset only —
 * compute_param_map_hash() -> encode_param_array_automatable() ->
 * encode_param_array_from(b, g_params, NPARAMS) -> encode_one_param() ->
 * harp_hash_compute(). The readonly meters are excluded by construction (they
 * are emitted only by encode_param_array, never fed to the hash), so a meter
 * change CANNOT move the hash — documented below as a structural property.
 *
 * WHY A TABLE-PARAMETERISED ENCODER: encode_param_array_from() takes the param
 * table as an argument purely so this test can feed hand-built VARIANT tables
 * (a label tweak / an id change / a steps change) and compare hashes. The
 * production wrapper calls it with g_params/NPARAMS, so the bytes — and thus
 * the frozen golden hash — are unchanged by the extraction (proven separately
 * by `ctest -L unit` + the golden render, which build the real engine.c table).
 *
 * STUB NOTE: device/state.c is linked for the REAL encoder; fuzz/state_stubs.c
 * resolves its other device symbols. That stub's g_params has labels==NULL for
 * stepped params, so we never hash the stub g_params here — every table this
 * test hashes (incl. the baseline) is built LOCALLY with real enum labels, so
 * encode_one_param's labels[] deref is always valid. We also never call
 * compute_param_map_hash() (its boot-assert loop calls the engine_part_param_get
 * abort-stub) — we hash variant tables directly through encode_param_array_from.
 *
 * Mirrors tests/harp_device_tests.c: same CHECK macro and wire-contract style.
 */
#include <stdio.h>
#include <string.h>

#include "harp/cbor.h"
#include "harp/object.h" /* harp_hash, harp_hash_compute, harp_hash_eq */

#include "device.h" /* dev_param, NPARAMS, encode_param_array_from */

#include "check.h"

/* Real enum labels (count == steps), mirroring device/engine.c. Stepped params
 * MUST carry non-NULL labels or encode_one_param derefs labels[s]. */
static const char *const ARP_MODES[] = {"Off", "Up", "Down", "Up-Down", "As Played"};
static const char *const ARP_DIVS[] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const char *const ARP_OCTS[] = {"1", "2", "3", "4"};

/* A LOCAL, fully-specified mirror of the shipping g_params (engine.c, ids 1..12,
 * contiguous since the 2.1.0 renumber) WITH real labels — the test baseline.
 * We deliberately do NOT hash the linked g_params: its stub mirror has NULL
 * labels for the stepped params. Variants are copies of this with one edit. */
static const dev_param BASELINE[NPARAMS] = {
    {1, "Osc Pitch", 0, NULL, 0.5f},     {2, "Osc Shape", 0, NULL, 0.5f},
    {3, "Filter Cutoff", 0, NULL, 0.5f}, {4, "Filter Reso", 0, NULL, 0.5f},
    {5, "Env Attack", 0, NULL, 0.5f},    {6, "Env Release", 0, NULL, 0.5f},
    {7, "Master Level", 0, NULL, 0.5f},
    {8, "Arp Mode", 5, ARP_MODES, 0.0f}, {9, "Arp Division", 6, ARP_DIVS, 0.6f},
    {10, "Arp Gate", 0, NULL, 0.5f},     {11, "Arp Octaves", 4, ARP_OCTS, 0.0f},
    {12, "Glide", 0, NULL, 0.0f},
};

/* Hash the automatable encoding of a table, exactly as the device does. */
static harp_hash hash_table(const dev_param *table, size_t n) {
    harp_cbuf b;
    harp_cbuf_init(&b);
    encode_param_array_from(&b, table, n);
    harp_hash h = harp_hash_compute(b.buf, b.len);
    harp_cbuf_free(&b);
    return h;
}

static void test_param_map_hash_change_detection(void) {
    const harp_hash base = hash_table(BASELINE, NPARAMS);

    /* (b) ID CHANGE — an id's meaning changes => stored automation on that id is
     * now misrouted. Hash MUST change. (e.g. the removed drone id 7 reused.) */
    dev_param v_id[NPARAMS];
    memcpy(v_id, BASELINE, sizeof v_id);
    v_id[6].id = 13; /* Master Level 7 -> 13 */
    harp_hash h_id = hash_table(v_id, NPARAMS);
    CHECK(!harp_hash_eq(&base, &h_id));

    /* (c) STEPS CHANGE — a stepped param's enum cardinality changes => a stored
     * step index may now be out of range / mean a different mode. Hash MUST
     * change. Arp Mode 5 -> 4 (drop "As Played"; labels shortened to match). */
    static const char *const ARP_MODES_4[] = {"Off", "Up", "Down", "Up-Down"};
    dev_param v_steps[NPARAMS];
    memcpy(v_steps, BASELINE, sizeof v_steps);
    v_steps[7].steps = 4;
    v_steps[7].labels = ARP_MODES_4;
    harp_hash h_steps = hash_table(v_steps, NPARAMS);
    CHECK(!harp_hash_eq(&base, &h_steps));

    /* (d) REMOVAL — dropping an automatable param changes the array. Hash MUST
     * change (a host must not keep automation for a param the device no longer
     * exposes). Encode the first NPARAMS-1 rows. */
    harp_hash h_rm = hash_table(BASELINE, NPARAMS - 1);
    CHECK(!harp_hash_eq(&base, &h_rm));

    /* (e) DETERMINISM — encoding the SAME table 3x is byte-identical, so the
     * hash is stable across calls (a recall/identity must reproduce it). */
    harp_cbuf e1, e2, e3;
    harp_cbuf_init(&e1);
    harp_cbuf_init(&e2);
    harp_cbuf_init(&e3);
    encode_param_array_from(&e1, BASELINE, NPARAMS);
    encode_param_array_from(&e2, BASELINE, NPARAMS);
    encode_param_array_from(&e3, BASELINE, NPARAMS);
    CHECK(e1.len == e2.len && memcmp(e1.buf, e2.buf, e1.len) == 0);
    CHECK(e2.len == e3.len && memcmp(e2.buf, e3.buf, e2.len) == 0);
    harp_cbuf_free(&e1);
    harp_cbuf_free(&e2);
    harp_cbuf_free(&e3);

    /* (f) READONLY METER — structural property: the hash input is the
     * AUTOMATABLE subset only (encode_param_array_automatable /
     * encode_param_array_from), and meters are appended ONLY by
     * encode_param_array, never fed to the hash. So adding/removing a meter
     * cannot reach this code path and cannot move the hash — there is no table
     * variant to construct. We document it here; the exclusion is enforced by
     * compute_param_map_hash() calling the automatable encoder, covered by the
     * golden render. (A regression that fed meters to the hash would change the
     * production golden and be caught by `ctest -L unit` + the golden check.) */

    /* (a) NAME/LABEL TWEAK — SHIPPED BEHAVIOR (verified empirically):
     * encode_one_param folds the display name (key 1) AND the enum labels
     * (key 9) into the hashed §9.3 descriptor. So a pure rename of a stepped
     * param DOES move the hash today. This is STRICTER than §9.3's "informative
     * tweaks shouldn't invalidate automation" intent: a rename does not change
     * any id meaning / range / step, so a host's stored automation would survive
     * it — yet the hash moves and orphans it. We PIN the shipped behavior (rename
     * => hash changes) rather than the looser spec intent so this golden's true
     * contents are documented and a future move to exclude the name/labels from
     * the hash is a deliberate, reviewed golden change — not a silent drift.
     * (The ONLY informative-only edit that is genuinely hash-invariant is the
     * readonly meter — excluded structurally, see (f).) */
    dev_param v_name[NPARAMS];
    memcpy(v_name, BASELINE, sizeof v_name);
    v_name[7].name = "Arpeggiator Mode"; /* rename a stepped param, id/steps intact */
    harp_hash hn = hash_table(v_name, NPARAMS);
    CHECK(!harp_hash_eq(&base, &hn));
}

int main(void) {
    test_param_map_hash_change_detection();
    return check_report(NULL);
}
