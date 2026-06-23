/* Unit tests for the RefDev params-blob codec (P3 closer step 3).
 *
 * WHY A SEPARATE BINARY FROM harp-tests:
 *   The codec under test — refdev_encode_params_blob / refdev_parse_params_blob
 *   (device/state.c) — is the genuine shipping code, not a fork. We link the
 *   REAL device/state.c here so a regression in the daemon's serializer is
 *   caught. But state.c #includes device.h (pthread/POSIX) and shares its
 *   translation unit with engine/store-coupled functions, so it cannot be
 *   dropped into harp-tests: that target links only harpcore and is the
 *   cross-platform (incl. Windows/MSVC) core test. Pulling state.c in would
 *   make the core test POSIX-only and drag the whole device. So this is a
 *   device-linked test (the NOTE's option B). harp-tests stays clean.
 *
 *   The codec is PURE (header contract: "no store I/O, no g_parts touch") and
 *   the only external it touches at runtime is the g_params[] id table. The
 *   other state.c functions (engine_snapshot_objects, load, ...) are linked but
 *   NEVER called from here; state_stubs.c satisfies the linker for the device
 *   symbols they reference. g_params is provided REAL (correct ids/order) so
 *   the codec's id-matching is exercised for real, not against a fake table.
 *
 * Mirrors tests/harp_tests.c: the same CHECK macro and the wire-contract
 * assertion style of test_event_channel (build exact bytes, decode, compare).
 */
#include <stdio.h>
#include <string.h>

#include "harp/cbor.h"

#include "device.h" /* NPARTS, NPARAMS, refdev_{encode,parse}_params_blob */

static int g_fail = 0, g_pass = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) {                                                       \
            g_pass++;                                                     \
        } else {                                                          \
            g_fail++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

/* The codec maps paramId<->grid-column via g_params[].id. We assert against
 * the real table (linked from state_stubs.c, mirroring engine.c's ids 1..13)
 * so the legacy-migration test can hand-pick concrete ids. */

static void test_params_blob_codec(void) {
    /* ---- 1. ROUND-TRIP: distinct value per cell, bit-exact recovery ---- */
    float orig[NPARTS][NPARAMS];
    for (int p = 0; p < NPARTS; p++)
        for (int i = 0; i < NPARAMS; i++)
            /* distinct, finite, and chosen so f32 round-trips through
             * harp_cbor_float's shortest-form encoding without loss; values
             * stay in a benign range (no inf/denormal surprises). */
            orig[p][i] = (float)(p * NPARAMS + i) * 0.013671875f + 0.001953125f;

    harp_cbuf enc;
    harp_cbuf_init(&enc);
    refdev_encode_params_blob(orig, &enc);

    float v[NPARTS][NPARAMS];
    bool present[NPARTS][NPARAMS];
    memset(v, 0, sizeof v);
    memset(present, 0, sizeof present);
    CHECK(refdev_parse_params_blob(enc.buf, enc.len, v, present));

    int rt_ok = 1;
    for (int p = 0; p < NPARTS; p++)
        for (int i = 0; i < NPARAMS; i++) {
            if (!present[p][i]) rt_ok = 0;
            /* bit-exact: the value started as f32, so encode (f32-preserving)
             * then parse (cast back to float) must reproduce it exactly. */
            if (memcmp(&v[p][i], &orig[p][i], sizeof(float)) != 0) rt_ok = 0;
        }
    CHECK(rt_ok); /* every part/param present and bit-identical */

    /* ---- 2. DETERMINISM: same state -> byte-identical payload (CAS) ---- */
    harp_cbuf enc2;
    harp_cbuf_init(&enc2);
    refdev_encode_params_blob(orig, &enc2);
    CHECK(enc.len == enc2.len && memcmp(enc.buf, enc2.buf, enc.len) == 0);

    /* a different value MUST change the bytes (encoder isn't ignoring input) */
    float tweaked[NPARTS][NPARAMS];
    memcpy(tweaked, orig, sizeof tweaked);
    tweaked[7][3] = orig[7][3] + 0.5f;
    harp_cbuf enc3;
    harp_cbuf_init(&enc3);
    refdev_encode_params_blob(tweaked, &enc3);
    CHECK(enc3.len == enc.len && memcmp(enc3.buf, enc.buf, enc.len) != 0);

    harp_cbuf_free(&enc2);
    harp_cbuf_free(&enc3);

    /* ---- 3. LEGACY MIGRATION: flat { id => value } -> part 0 only ---- */
    /* Hand-build the OLD on-wire form (a flat map whose first value is a FLOAT,
     * which is how the parser discriminates legacy vs per-part). Use real param
     * ids 3 and 8 (Filter Cutoff, Master Level). */
    harp_cbuf legacy;
    harp_cbuf_init(&legacy);
    harp_cbor_map(&legacy, 2);
    harp_cbor_uint(&legacy, 3);
    harp_cbor_float(&legacy, 0.25);
    harp_cbor_uint(&legacy, 8);
    harp_cbor_float(&legacy, 0.75);

    float lv[NPARTS][NPARAMS];
    bool lpresent[NPARTS][NPARAMS];
    memset(lv, 0, sizeof lv);
    memset(lpresent, 0, sizeof lpresent);
    CHECK(refdev_parse_params_blob(legacy.buf, legacy.len, lv, lpresent));

    /* the two ids landed in part 0, with the right values. NB id 8 is column 6
     * now, not 7: removing id 7 (Drone Mix) shifted Master Level down one slot. */
    CHECK(lpresent[0][2] && lv[0][2] == 0.25f); /* id 3 -> column index 2 */
    CHECK(lpresent[0][6] && lv[0][6] == 0.75f); /* id 8 -> column index 6 (was 7 pre-drone-removal) */

    /* nothing else in part 0 is marked present (only the two carried ids) */
    int p0_only_two = 1;
    for (int i = 0; i < NPARAMS; i++)
        if (i != 2 && i != 6 && lpresent[0][i]) p0_only_two = 0;
    CHECK(p0_only_two);

    /* parts 1..15 untouched: present false everywhere */
    int parts_1_15_clean = 1;
    for (int p = 1; p < NPARTS; p++)
        for (int i = 0; i < NPARAMS; i++)
            if (lpresent[p][i]) parts_1_15_clean = 0;
    CHECK(parts_1_15_clean);

    harp_cbuf_free(&legacy);

    /* a legacy map carrying an UNKNOWN id is well-formed but matches no column:
     * parse succeeds, nothing marked present. */
    harp_cbuf legacy_unknown;
    harp_cbuf_init(&legacy_unknown);
    harp_cbor_map(&legacy_unknown, 1);
    harp_cbor_uint(&legacy_unknown, 9999); /* not in g_params */
    harp_cbor_float(&legacy_unknown, 0.5);
    memset(lpresent, 0, sizeof lpresent);
    CHECK(refdev_parse_params_blob(legacy_unknown.buf, legacy_unknown.len, lv, lpresent));
    int none_present = 1;
    for (int p = 0; p < NPARTS; p++)
        for (int i = 0; i < NPARAMS; i++)
            if (lpresent[p][i]) none_present = 0;
    CHECK(none_present);
    harp_cbuf_free(&legacy_unknown);

    /* per-part: an OUT-OF-RANGE part index is consumed and discarded, not fatal;
     * the well-formed remaining part still lands. Outer map {0:{id3:..}, 99:{..}}.
     * (99 >= NPARTS -> skipped; cursor must stay aligned.) */
    harp_cbuf oor;
    harp_cbuf_init(&oor);
    harp_cbor_map(&oor, 2);
    harp_cbor_uint(&oor, 0);
    harp_cbor_map(&oor, 1);
    harp_cbor_uint(&oor, 3);
    harp_cbor_float(&oor, 0.6);
    harp_cbor_uint(&oor, 99); /* out-of-range part */
    harp_cbor_map(&oor, 1);
    harp_cbor_uint(&oor, 4);
    harp_cbor_float(&oor, 0.4);
    memset(lv, 0, sizeof lv);
    memset(lpresent, 0, sizeof lpresent);
    CHECK(refdev_parse_params_blob(oor.buf, oor.len, lv, lpresent));
    CHECK(lpresent[0][2] && lv[0][2] == (float)0.6); /* id 3 in part 0 landed */
    /* the out-of-range part's value went nowhere */
    int oor_clean = 1;
    for (int p = 1; p < NPARTS; p++)
        for (int i = 0; i < NPARAMS; i++)
            if (lpresent[p][i]) oor_clean = 0;
    CHECK(oor_clean);
    harp_cbuf_free(&oor);

    /* an empty map is well-formed and a no-op (present stays all-false) */
    uint8_t empty_map[] = {0xa0};
    memset(lpresent, 0, sizeof lpresent);
    CHECK(refdev_parse_params_blob(empty_map, sizeof empty_map, lv, lpresent));
    int empty_clean = 1;
    for (int p = 0; p < NPARTS; p++)
        for (int i = 0; i < NPARAMS; i++)
            if (lpresent[p][i]) empty_clean = 0;
    CHECK(empty_clean);

    /* ---- 4. FAIL-CLEAN: malformed/hostile inputs -> false, no crash ---- */
    /* (ASan in CI flags any OOB read these provoke.) */
    float fv[NPARTS][NPARAMS];
    bool fp[NPARTS][NPARAMS];

    /* empty buffer: not even a map header */
    CHECK(!refdev_parse_params_blob((const uint8_t *)"", 0, fv, fp));

    /* not a map: a bare uint */
    uint8_t not_map[] = {0x01};
    CHECK(!refdev_parse_params_blob(not_map, sizeof not_map, fv, fp));

    /* first value neither map nor float (it's a text string) -> malformed */
    harp_cbuf bad_first;
    harp_cbuf_init(&bad_first);
    harp_cbor_map(&bad_first, 1);
    harp_cbor_uint(&bad_first, 0);
    harp_cbor_text(&bad_first, "x");
    CHECK(!refdev_parse_params_blob(bad_first.buf, bad_first.len, fv, fp));
    harp_cbuf_free(&bad_first);

    /* per-part map truncated mid inner-pair: header says 1 part with 2 inner
     * pairs but only one pair's bytes follow -> structural failure. */
    harp_cbuf trunc;
    harp_cbuf_init(&trunc);
    harp_cbor_map(&trunc, 1);
    harp_cbor_uint(&trunc, 0);
    harp_cbor_map(&trunc, 2);     /* promises 2 pairs */
    harp_cbor_uint(&trunc, 1);
    harp_cbor_float(&trunc, 0.5); /* only 1 pair actually present */
    CHECK(!refdev_parse_params_blob(trunc.buf, trunc.len, fv, fp));
    harp_cbuf_free(&trunc);

    /* legacy map truncated: declares 2 pairs, second pair's value missing */
    harp_cbuf ltrunc;
    harp_cbuf_init(&ltrunc);
    harp_cbor_map(&ltrunc, 2);
    harp_cbor_uint(&ltrunc, 3);
    harp_cbor_float(&ltrunc, 0.1);
    harp_cbor_uint(&ltrunc, 8); /* key present, value absent */
    CHECK(!refdev_parse_params_blob(ltrunc.buf, ltrunc.len, fv, fp));
    harp_cbuf_free(&ltrunc);

    /* half a map: a map header with a huge declared count and no contents.
     * 0xbb + 8-byte length = 0xffffffffffffffff pairs, zero bytes follow ->
     * the decoder's bounds-checked count must reject / fail clean, never loop
     * unbounded or read OOB. */
    uint8_t huge_outer[] = {0xbb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    CHECK(!refdev_parse_params_blob(huge_outer, sizeof huge_outer, fv, fp));

    /* hostile INNER count: valid 1-part outer, inner map claims a huge pair
     * count with no pairs -> first inner uint read fails -> false. */
    uint8_t huge_inner[] = {
        0xa1,                                                       /* outer map(1) */
        0x00,                                                       /* part 0 */
        0xbb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,       /* inner map(2^64-1) */
    };
    CHECK(!refdev_parse_params_blob(huge_inner, sizeof huge_inner, fv, fp));

    /* bad major type where a part-index uint is expected: outer map(2), first
     * pair fine (per-part), then a float appears where the 2nd part key (uint)
     * must be -> harp_cdec_uint fails -> false. */
    harp_cbuf bad_key;
    harp_cbuf_init(&bad_key);
    harp_cbor_map(&bad_key, 2);
    harp_cbor_uint(&bad_key, 0);
    harp_cbor_map(&bad_key, 1);
    harp_cbor_uint(&bad_key, 1);
    harp_cbor_float(&bad_key, 0.5);
    harp_cbor_float(&bad_key, 0.9); /* should be a uint part index */
    CHECK(!refdev_parse_params_blob(bad_key.buf, bad_key.len, fv, fp));
    harp_cbuf_free(&bad_key);

    /* garbage bytes: random-looking header byte that is a reserved/ill-formed
     * CBOR additional-info -> map decode fails. 0x1f = uint with indefinite
     * additional info (ill-formed for an integer). */
    uint8_t garbage[] = {0x1f, 0x2a, 0xc0, 0xde};
    CHECK(!refdev_parse_params_blob(garbage, sizeof garbage, fv, fp));

    harp_cbuf_free(&enc);
}

int main(void) {
    test_params_blob_codec();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
