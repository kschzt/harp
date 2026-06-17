/* harp_engine_logic_tests — host-compiled unit tests for the two determinism-
 * critical PURE policies the device render rests on, factored out of engine.c so
 * they can run in CI without a board:
 *   - harp_voice_pick   (device/voice_alloc.h): §9.5 voice allocation / steal.
 *   - harp_arp_select   (device/arp_select.h):  §9.7 arp note selection, 4 modes.
 *
 * These are exactly the functions engine.c calls on the real note-on / arp-step
 * path (so this is the real logic, not a parallel copy). The chord/voice-pool and
 * groove golden hashes pin them byte-identical on hardware; this pins the steal
 * order and every arp mode/octave/clamp branch off-hardware, where a regression
 * would otherwise only surface as a wrong-but-deterministic render no PR CI runs.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "arp_select.h"
#include "voice_alloc.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

/* §9.5 voice allocation: first FREE slot in index order, else steal the oldest
 * (smallest alloc_seq). */
static void test_voice_pick(void) {
    /* a cold pool: slot 0 is free, taken first */
    bool active[8] = {0};
    uint64_t seq[8] = {0};
    CHECK(harp_voice_pick(active, seq, 8) == 0);

    /* slots 0..2 filled in order -> the next note takes slot 3 (first free) */
    active[0] = active[1] = active[2] = true;
    seq[0] = 0; seq[1] = 1; seq[2] = 2;
    CHECK(harp_voice_pick(active, seq, 8) == 3);

    /* a freed slot in the MIDDLE is preferred over any steal (index order) */
    active[3] = active[4] = active[5] = active[6] = active[7] = true;
    active[4] = false; /* free slot 4 */
    CHECK(harp_voice_pick(active, seq, 8) == 4);

    /* a fully active pool steals the OLDEST (min alloc_seq), not slot 0 */
    bool full[8] = {true, true, true, true, true, true, true, true};
    uint64_t ages[8] = {5, 2, 8, 1, 9, 3, 7, 4}; /* min is index 3 (age 1) */
    CHECK(harp_voice_pick(full, ages, 8) == 3);

    /* the steal is purely by age, independent of slot index: make slot 7 oldest */
    uint64_t ages2[8] = {50, 20, 80, 30, 90, 33, 70, 9};
    CHECK(harp_voice_pick(full, ages2, 8) == 7);

    /* first-free still beats a much-older active slot */
    bool one_free[8] = {true, true, true, true, true, true, false, true};
    uint64_t old0[8] = {1, 2, 3, 4, 5, 6, 7, 8}; /* slot 0 is oldest-active */
    CHECK(harp_voice_pick(one_free, old0, 8) == 6); /* the free slot, not slot 0 */
}

/* §9.7 arp note selection across the four modes, octave span, pitch sort, clamp. */
static void test_arp_select(void) {
    int order[8];
    const uint8_t sorted[3] = {60, 64, 67};
    const uint8_t press[3]  = {67, 60, 64}; /* press order != pitch order */

    /* mode 1 (up), one octave: cycles low->high then wraps */
    CHECK(harp_arp_select(1, 0, sorted, 3, 0, order).note == 60);
    CHECK(harp_arp_select(1, 1, sorted, 3, 0, order).note == 64);
    CHECK(harp_arp_select(1, 2, sorted, 3, 0, order).note == 67);
    CHECK(harp_arp_select(1, 3, sorted, 3, 0, order).note == 60); /* wrap */

    /* up SORTS by pitch: an unsorted latch still ascends 60,64,67 */
    CHECK(harp_arp_select(1, 0, press, 3, 0, order).note == 60);
    CHECK(harp_arp_select(1, 1, press, 3, 0, order).note == 64);
    CHECK(harp_arp_select(1, 2, press, 3, 0, order).note == 67);

    /* mode 2 (down): high->low */
    CHECK(harp_arp_select(2, 0, sorted, 3, 0, order).note == 67);
    CHECK(harp_arp_select(2, 1, sorted, 3, 0, order).note == 64);
    CHECK(harp_arp_select(2, 2, sorted, 3, 0, order).note == 60);

    /* mode 3 (up-down, no repeated endpoints): 60,64,67,64,60,64,67,... */
    int updown[7] = {60, 64, 67, 64, 60, 64, 67};
    for (int s = 0; s < 7; s++)
        CHECK(harp_arp_select(3, s, sorted, 3, 0, order).note == updown[s]);

    /* mode 4 (as-played): keeps PRESS order, no sort */
    CHECK(harp_arp_select(4, 0, press, 3, 0, order).note == 67);
    CHECK(harp_arp_select(4, 1, press, 3, 0, order).note == 60);
    CHECK(harp_arp_select(4, 2, press, 3, 0, order).note == 64);
    /* sel indexes back into the (unsorted) latch for the velocity lookup */
    CHECK(harp_arp_select(4, 0, press, 3, 0, order).sel == 0);

    /* octave span (octaves=1 -> two octaves): step 3 = same note one octave up */
    CHECK(harp_arp_select(1, 0, sorted, 3, 1, order).note == 60);
    CHECK(harp_arp_select(1, 3, sorted, 3, 1, order).note == 72); /* 60 + 12 */
    CHECK(harp_arp_select(1, 5, sorted, 3, 1, order).note == 79); /* 67 + 12 */

    /* >127 clamp: an octave that would exceed 127 drops back to the base pitch */
    const uint8_t high[1] = {120};
    CHECK(harp_arp_select(1, 0, high, 1, 1, order).note == 120);
    CHECK(harp_arp_select(1, 1, high, 1, 1, order).note == 120); /* 132 clamped */

    /* single-note latch: up-down degenerates to a single repeated note (cycle 1) */
    const uint8_t one[1] = {72};
    CHECK(harp_arp_select(3, 0, one, 1, 0, order).note == 72);
    CHECK(harp_arp_select(3, 1, one, 1, 0, order).note == 72);
}

int main(void) {
    test_voice_pick();
    test_arp_select();
    printf("harp-engine-logic-tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
