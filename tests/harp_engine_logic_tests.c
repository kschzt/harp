/* harp_engine_logic_tests — host-compiled unit tests for the two determinism-
 * critical PURE policies the device render rests on, factored out of engine.c so
 * they can run in CI without a board:
 *   - harp_voice_pick   (device/voice_alloc.h): §9.5 voice allocation / steal.
 *   - harp_arp_select   (device/arp_select.h):  §9.7 arp note selection, 4 modes.
 *   - harp_evt_part / harp_mod_target / harp_mod_targets_voice (device/evq_mod.h):
 *     §P2.1 event routing + §9.4/§9.5 modulation target + per-voice addressing.
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
#include "evq_mod.h"
#include "fence_wait.h"
#include "usb_select.h"
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

/* §P2.1 event routing + §9.4/§9.5 modulation target + per-voice addressing. These are
 * exactly the helpers engine.c's evq_apply_due() routing + the DEV_EV_MOD and the §9.5
 * per-voice DEV_EV_PARAM_SET / DEV_EV_RAMP cases call (per-voice addressing: voice!=0 lands
 * on the addressed sounding voice, not the whole part). */
static void test_evq_mod(void) {
    /* channel -> part: modulo the part count; ch0 -> part 0; wraps past nparts */
    CHECK(harp_evt_part(0, 16) == 0);
    CHECK(harp_evt_part(5, 16) == 5);
    CHECK(harp_evt_part(16, 16) == 0);  /* wrap */
    CHECK(harp_evt_part(17, 16) == 1);

    /* §9.5 per-voice addressing. ev_voice==0 = whole-part: every ACTIVE voice. */
    CHECK(harp_mod_targets_voice(0, true, 12345) == true);   /* active, whole-part */
    CHECK(harp_mod_targets_voice(0, false, 12345) == false); /* inactive never takes a mod */
    /* ev_voice != 0: ONLY the active voice whose key matches (gone voice ignored) */
    CHECK(harp_mod_targets_voice(777, true, 777) == true);   /* exact match */
    CHECK(harp_mod_targets_voice(777, true, 778) == false);  /* active but wrong key */
    CHECK(harp_mod_targets_voice(777, false, 777) == false); /* matching key but voice gone */

    /* §9.5 target classification. bend/pressure ids route to dedicated axes; else a
     * real param idx (>=0) is the §9.4 mod[] layer; any other id is ignored. The wire
     * ids are passed in — here distinct placeholders so the test is header-pure. */
    const uint32_t BEND_ID = 0xE000u, PRESS_ID = 0xD000u;
    CHECK(harp_mod_target(BEND_ID, -1, BEND_ID, PRESS_ID) == HARP_MODT_BEND);
    CHECK(harp_mod_target(PRESS_ID, -1, BEND_ID, PRESS_ID) == HARP_MODT_PRESSURE);
    CHECK(harp_mod_target(0x42, 3, BEND_ID, PRESS_ID) == HARP_MODT_PARAM);   /* real param */
    CHECK(harp_mod_target(0x42, -1, BEND_ID, PRESS_ID) == HARP_MODT_IGNORE); /* unknown id */
    /* a dedicated axis wins even if a param index were also resolvable */
    CHECK(harp_mod_target(BEND_ID, 5, BEND_ID, PRESS_ID) == HARP_MODT_BEND);
}

/* §8.3.1 fence-wait predicate (device/fence_wait.h) — exactly what host_paced_loop's fence
 * barrier calls. Real-time host-paced MUST bound the wait (a few ms, then render late + count
 * fence_timeouts); the deterministic offline bounce stays an unbounded barrier. */
static void test_fence(void) {
    /* consumed (pending<=0) or session stopping -> never wait, on either path */
    CHECK(!harp_fence_keep_waiting(0, true, false, 100, 200));
    CHECK(!harp_fence_keep_waiting(-1, true, true, 100, 200));
    CHECK(!harp_fence_keep_waiting(5, false, false, 100, 200));
    CHECK(!harp_fence_keep_waiting(5, false, true, 100, 200));
    /* OFFLINE deterministic bounce: pending + running -> wait UNBOUNDED (ignores the deadline) */
    CHECK(harp_fence_keep_waiting(5, true, true, 100, 200));
    CHECK(harp_fence_keep_waiting(5, true, true, 999999, 200)); /* now past deadline, still waits */
    /* REAL-TIME (USB): wait only BEFORE the deadline; at/after it the §8.3.1 bound is hit -> stop */
    CHECK(harp_fence_keep_waiting(5, true, false, 100, 200));
    CHECK(!harp_fence_keep_waiting(5, true, false, 200, 200)); /* deadline reached */
    CHECK(!harp_fence_keep_waiting(5, true, false, 250, 200)); /* past deadline */
}

/* §15.2 multi-device SELECTION (host/usb_select.h) — the "never bind a different
 * synth" rule, the safety core of multi-device. The fallback CHAIN (try exact serial,
 * then same-model, then any) is the caller's orchestration + the hardware
 * multidevice/replug tests; this pins the per-device MATCH decision. */
static void test_usb_select(void) {
    const uint16_t V = 0x1209, P = 0x4852; /* the HARP USB id; a 2nd model differs in vid:pid */

    /* fresh-any (both unset): ANY HARP device matches */
    CHECK(harp_usb_dev_matches("PI4B-0001", V, P, NULL, false, 0, 0));
    CHECK(harp_usb_dev_matches("PI4B-0003", V, P, NULL, false, 0, 0));

    /* exact serial (want set): only that serial — reconnect pins it, a wrong serial is refused */
    CHECK(harp_usb_dev_matches("PI4B-0001", V, P, "PI4B-0001", false, 0, 0));
    CHECK(!harp_usb_dev_matches("PI4B-0003", V, P, "PI4B-0001", false, 0, 0));

    /* same-model (want_vp): same vid:pid matches; a DIFFERENT model is NEVER bound */
    CHECK(harp_usb_dev_matches("X", V, P, NULL, true, V, P));
    CHECK(!harp_usb_dev_matches("X", V, (uint16_t)(P + 1), NULL, true, V, P)); /* diff product */
    CHECK(!harp_usb_dev_matches("X", (uint16_t)(V + 1), P, NULL, true, V, P)); /* diff vendor  */

    /* serial AND model both required: the exact serial of the right model */
    CHECK(harp_usb_dev_matches("S1", V, P, "S1", true, V, P));
    CHECK(!harp_usb_dev_matches("S2", V, P, "S1", true, V, P));                  /* wrong serial */
    CHECK(!harp_usb_dev_matches("S1", V, (uint16_t)(P + 1), "S1", true, V, P));  /* wrong model  */
}

static void test_evt_epoch_stale(void) {
    /* §7.1: epoch 0 = "now" (never stale); an older epoch is stale; current/future is live. The
     * outer envelope guard and both inner instants (ramp-end §9.4, txn-commit §9.6) share this. */
    CHECK(!harp_evt_epoch_stale(0, 5)); /* "now" sentinel — never stale */
    CHECK(harp_evt_epoch_stale(3, 5));  /* older epoch -> stale */
    CHECK(!harp_evt_epoch_stale(5, 5)); /* current -> live */
    CHECK(!harp_evt_epoch_stale(7, 5)); /* future (shouldn't arrive) -> not stale */
    CHECK(!harp_evt_epoch_stale(1, 0)); /* device epoch 0 -> nothing stale yet */
}

static void test_evt_dirties(void) {
    /* §9.5/§9.6: only a WHOLE-PART (voice 0) set/ramp dirties the live ref. Per-voice (voice != 0)
     * is transient, mod (etype 6) is non-destructive, and a txn-buffered edit defers to commit. */
    CHECK(harp_evt_dirties_live(1, 0, false));    /* whole-part set -> dirties */
    CHECK(harp_evt_dirties_live(5, 0, false));    /* whole-part ramp -> dirties */
    CHECK(!harp_evt_dirties_live(1, 777, false)); /* per-voice set -> transient */
    CHECK(!harp_evt_dirties_live(5, 777, false)); /* per-voice ramp -> transient */
    CHECK(!harp_evt_dirties_live(1, 0, true));    /* buffered -> defers to commit */
    CHECK(!harp_evt_dirties_live(5, 0, true));    /* buffered -> defers to commit */
    CHECK(!harp_evt_dirties_live(6, 0, false));   /* mod -> non-destructive */
}

int main(void) {
    test_voice_pick();
    test_arp_select();
    test_evq_mod();
    test_evt_epoch_stale();
    test_evt_dirties();
    test_fence();
    test_usb_select();
    printf("harp-engine-logic-tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
