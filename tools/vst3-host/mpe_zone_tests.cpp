/* Unit tests for shell/mpe_zone.h — the classic-MPE (member-channel MIDI) ->
 * §9.4/§9.5 zone-collapse state machine the AU shell (and any raw-MIDI shell)
 * funnels MPE through. No device, no SDK: pure logic, so it runs in CI on every
 * push and pins the behavior the hardware mpe-raw test then confirms end-to-end.
 *
 * What MUST hold (and is asserted below):
 *   - INACTIVE is the byte-identical non-MPE path: every note keeps its own
 *     channel, every expression axis returns valid=false.
 *   - an MCM (RPN 6) engages a zone (auto-detect); the channel picks lower/upper.
 *   - a member note collapses onto the instance part (NEVER the member channel)
 *     and binds chan -> the §9.5 voice key (part<<8)|note.
 *   - pitch bend / pressure / CC74 resolve to that voice with the right SIGNED
 *     value; neutral pitch (8192) is exactly 0 (so an unbent voice is untouched).
 *   - RPN 0 sets the member vs master PB range by the channel it arrives on; an
 *     MCM re-seeds the ±2 / ±48 defaults.
 *   - the pitch scaling is EXACT at the power-of-two bend values the cross-format
 *     check relies on (value14 6144, range 48 -> exactly -12.0 semitones). */
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "mpe_zone.h"

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

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-6f; }

/* INACTIVE: the non-MPE pass-through must echo the note's own channel and emit
 * nothing on the expression axes — the byte-identical golden path. */
static void test_inactive_passthrough() {
    MpeZone z;
    CHECK(!z.active());
    MpeNote n = z.noteOn(3, 60); /* note on MIDI channel 3 */
    CHECK(n.accepted && n.part == 3); /* its OWN channel, not the instance part */
    MpeNote f = z.noteOff(3, 60);
    CHECK(f.accepted && f.part == 3);
    /* a channel != the instance part proves the zone is not silently re-parting */
    z.setPart(7);
    CHECK(z.noteOn(3, 60).part == 3); /* still its own channel while inactive */
    /* expression is inert while inactive */
    CHECK(!z.pitchBend(1, 4096).valid);
    CHECK(!z.channelPressure(1, 100).valid);
    CHECK(!z.cc(1, 74, 100).valid);
}

/* An MCM (RPN 6) on ch0 engages a lower zone; ch15 an upper zone. */
static void test_mcm_engages_zone() {
    MpeZone z;
    z.setPart(0);
    /* RPN 6 = MCM, member count 5, on the lower-zone master (ch0) */
    z.cc(0, 101, 0); /* RPN MSB */
    z.cc(0, 100, 6); /* RPN LSB -> RPN 6 */
    z.cc(0, 6, 5);   /* data: 5 members */
    CHECK(z.active());
    CHECK(z.lowerZone && z.members == 5);
    CHECK(z.isMaster(0) && !z.isMaster(15));
    CHECK(z.isMember(1) && z.isMember(5) && !z.isMember(6) && !z.isMember(0));
    /* MCM re-seeds the MPE default PB ranges */
    CHECK(approx(z.masterPbRange, HARP_MPE_DEFAULT_MASTER_PB_RANGE));
    CHECK(approx(z.memberPbRange, HARP_MPE_DEFAULT_MEMBER_PB_RANGE));

    MpeZone u;
    u.cc(15, 101, 0);
    u.cc(15, 100, 6);
    u.cc(15, 6, 4);
    CHECK(u.active() && !u.lowerZone && u.members == 4);
    CHECK(u.isMaster(15) && u.isMember(14) && u.isMember(11) && !u.isMember(10));

    /* member count 0 deactivates the zone */
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 0);
    CHECK(!z.active());
}

/* A member note collapses onto the instance part and binds its voice key; a note
 * outside the zone is rejected for the caller to drop. */
static void test_zone_collapse_and_binding() {
    MpeZone z;
    z.setPart(5);
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 3); /* 3-member lower zone */
    /* three notes on three DIFFERENT member channels all collapse onto part 5 */
    CHECK(z.noteOn(1, 60).part == 5);
    CHECK(z.noteOn(2, 64).part == 5);
    CHECK(z.noteOn(3, 67).part == 5);
    /* and each binds chan -> (part<<8)|note, the key the device mints at note-on */
    MpeMod m0 = z.pitchBend(1, 0);
    CHECK(m0.valid && m0.voiceKey == ((5u << 8) | 60u));
    MpeMod m2 = z.pitchBend(3, 0);
    CHECK(m2.valid && m2.voiceKey == ((5u << 8) | 67u));
    /* a note outside the zone (ch 6 with only 3 members) is rejected */
    MpeNote bad = z.noteOn(6, 72);
    CHECK(!bad.accepted);
    /* note-off clears the binding: a later bend on that channel has no voice */
    z.noteOff(1, 60);
    CHECK(!z.pitchBend(1, 4096).valid);
}

/* Pitch bend resolves to the bound voice with semitones = norm * range, EXACT at
 * the power-of-two values the cross-format hash check uses. Master is part-wide. */
static void test_pitch_bend_scaling() {
    MpeZone z;
    z.setPart(0);
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 3); /* members 3, range re-seeded to 48 */
    z.noteOn(1, 60);
    /* neutral (8192) is EXACTLY zero -> an unbent voice is byte-identical */
    MpeMod neut = z.pitchBend(1, 8192);
    CHECK(neut.valid && neut.value == 0.0f);
    /* value14 6144 over ±48 -> exactly -12.0 (one octave); 6144/8192 = -0.25 */
    MpeMod down = z.pitchBend(1, 6144);
    CHECK(down.valid && down.value == -12.0f && down.voiceKey == ((0u << 8) | 60u));
    /* value14 0 over ±48 -> exactly -48.0 (the only reachable full-scale extreme) */
    MpeMod full = z.pitchBend(1, 0);
    CHECK(full.valid && full.value == -48.0f);
    /* master-channel bend is part-wide (voiceKey 0), scaled by the MASTER range */
    MpeMod mast = z.pitchBend(0, 0);
    CHECK(mast.valid && mast.voiceKey == 0 &&
          approx(mast.value, -HARP_MPE_DEFAULT_MASTER_PB_RANGE));
}

/* Channel pressure -> per-voice loudness gain (0..1); CC74 -> a SIGNED Filter
 * Cutoff offset about 0.5 (the same mapping the note-expression path uses). */
static void test_pressure_and_timbre() {
    MpeZone z;
    z.setPart(0);
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 3);
    z.noteOn(2, 64);
    MpeMod pr = z.channelPressure(2, 127);
    CHECK(pr.valid && approx(pr.value, 1.0f) && pr.voiceKey == ((0u << 8) | 64u));
    CHECK(approx(z.channelPressure(2, 0).value, 0.0f)); /* neutral pressure = 0 gain */
    MpeMod tb = z.cc(2, 74, 127);
    CHECK(tb.valid && approx(tb.value, 0.5f)); /* 127/127 - 0.5 */
    CHECK(approx(z.cc(2, 74, 0).value, -0.5f)); /* 0/127 - 0.5 */
    /* the timbre target is the Filter Cutoff param id */
    CHECK(HARP_MPE_MOD_TIMBRE == 3u);
}

/* RPN 0 sets the member PB range when it arrives on a member channel and the
 * master range when on the master; the host's --mpe-range path depends on this. */
static void test_rpn_pb_range() {
    MpeZone z;
    z.setPart(0);
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 5); /* 5-member zone; range now 48 */
    /* RPN 0 on member ch1 -> member range 24 */
    z.cc(1, 101, 0);
    z.cc(1, 100, 0);
    z.cc(1, 6, 24);
    CHECK(approx(z.memberPbRange, 24.0f));
    CHECK(approx(z.masterPbRange, HARP_MPE_DEFAULT_MASTER_PB_RANGE)); /* untouched */
    /* a full-down bend now reads -24, not -48 */
    z.noteOn(1, 60);
    CHECK(z.pitchBend(1, 0).value == -24.0f);
    /* RPN 0 on the master (ch0) -> master range 12 */
    z.cc(0, 101, 0);
    z.cc(0, 100, 0);
    z.cc(0, 6, 12);
    CHECK(approx(z.masterPbRange, 12.0f));
    CHECK(approx(z.memberPbRange, 24.0f)); /* still the member value */
}

/* The explicit toggle engages MPE even with no MCM (a default full lower zone),
 * for a host that streams raw MPE without configuring a zone. */
static void test_explicit_toggle() {
    MpeZone z;
    z.setPart(2);
    CHECK(!z.active());
    z.setEnabled(true);
    CHECK(z.active() && z.lowerZone && z.members == 15);
    CHECK(z.noteOn(4, 60).part == 2); /* collapses onto the instance part */
    z.setEnabled(false);
    CHECK(!z.active()); /* and the live bindings are cleared on disable */
}

/* The expression mod-target ids stay in lockstep with the shared shell ids the
 * device wire carries (the static_asserts in harp_au.mm pin these at compile). */
static void test_mod_ids() {
    CHECK(HARP_MPE_MOD_PITCH_BEND == 0x2001u);
    CHECK(HARP_MPE_MOD_PRESSURE == 0x2002u);
}

/* The UPPER zone (master ch15, members descending) must collapse + express
 * exactly like the lower zone — on a NON-zero instance part too (member binding
 * is correct for any part; only the master path is part-0-limited). */
static void test_upper_zone_expression() {
    MpeZone z;
    z.setPart(3);
    z.cc(15, 101, 0);
    z.cc(15, 100, 6);
    z.cc(15, 6, 4); /* 4-member UPPER zone: members ch14..11, master ch15 */
    CHECK(z.active() && !z.lowerZone && z.members == 4);
    CHECK(z.isMaster(15) && z.isMember(14) && z.isMember(11) && !z.isMember(10));
    CHECK(z.noteOn(14, 60).part == 3); /* collapses onto the instance part */
    CHECK(z.noteOn(11, 67).part == 3);
    /* bend on an upper member -> that voice, scaled by the (MCM-reseeded ±48) range */
    MpeMod b = z.pitchBend(14, 6144);
    CHECK(b.valid && b.value == -12.0f && b.voiceKey == ((3u << 8) | 60u));
    /* pressure + CC74 on an upper member resolve to the same voice */
    MpeMod pr = z.channelPressure(14, 127);
    CHECK(pr.valid && approx(pr.value, 1.0f) && pr.voiceKey == ((3u << 8) | 60u));
    MpeMod tb = z.cc(14, 74, 127);
    CHECK(tb.valid && approx(tb.value, 0.5f) && tb.voiceKey == ((3u << 8) | 60u));
    /* the upper master (ch15) is part-wide */
    MpeMod m = z.pitchBend(15, 0);
    CHECK(m.valid && m.voiceKey == 0);
}

/* RPN 0 routes by channel role BEFORE any MCM engages the zone (lowerZone
 * defaults true): ch0 -> master range, a member channel -> member range. */
static void test_rpn_before_active() {
    MpeZone z; /* fresh: no MCM, inactive */
    CHECK(!z.active());
    z.cc(0, 101, 0);
    z.cc(0, 100, 0);
    z.cc(0, 6, 7); /* RPN 0 on ch0 (the lower master) -> master range 7 */
    CHECK(approx(z.masterPbRange, 7.0f));
    CHECK(approx(z.memberPbRange, HARP_MPE_DEFAULT_MEMBER_PB_RANGE)); /* untouched */
    z.cc(1, 101, 0);
    z.cc(1, 100, 0);
    z.cc(1, 6, 24); /* RPN 0 on a member -> member range 24, even while inactive */
    CHECK(approx(z.memberPbRange, 24.0f));
    CHECK(approx(z.masterPbRange, 7.0f)); /* still the master value */
}

/* Symmetric with the note-on rejection: a NOTE-OFF outside the active zone is
 * also accepted=false (the caller drops it); a MISMATCHED member note-off must
 * NOT strand the sounding voice by clearing its binding. */
static void test_out_of_zone_and_mismatch() {
    MpeZone z;
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 3); /* 3-member lower zone */
    CHECK(z.noteOff(6, 72).accepted == false); /* ch6 outside a 3-member zone */
    z.noteOn(1, 60);
    z.noteOff(1, 62); /* WRONG note on ch1 -> must not clear the binding */
    CHECK(z.pitchBend(1, 4096).valid); /* voice still bound, expression still routes */
    z.noteOff(1, 60); /* the real off clears it */
    CHECK(!z.pitchBend(1, 4096).valid);
}

/* The explicit toggle is a force-on a zone-teardown MCM cannot defeat: a count-0
 * MCM while enabled re-seeds a usable default zone instead of dropping notes. */
static void test_count0_mcm_with_toggle() {
    MpeZone z;
    z.setEnabled(true); /* force-on: seeds a full lower zone */
    CHECK(z.active() && z.members == 15);
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 0); /* count-0 MCM: would zero members, but the toggle is still on */
    CHECK(z.active() && z.members == 15); /* re-seeded, not defeated */
    CHECK(z.noteOn(3, 60).accepted); /* member notes still flow */
}

/* The DOCUMENTED part-0 master limitation, pinned so a future fix is a conscious
 * test change: on a non-zero instance part a master (part-wide) message still
 * returns voiceKey 0 (which the runtime encodes as part 0), while a member
 * message correctly carries the instance part. */
static void test_master_nonzero_part_limit() {
    MpeZone z;
    z.setPart(5);
    z.cc(0, 101, 0);
    z.cc(0, 100, 6);
    z.cc(0, 6, 3);
    z.noteOn(1, 60);
    CHECK(z.pitchBend(1, 0).voiceKey == ((5u << 8) | 60u)); /* member: correct part */
    CHECK(z.pitchBend(0, 0).voiceKey == 0);                 /* master: part-wide (part-0 limit) */
}

int main() {
    test_inactive_passthrough();
    test_mcm_engages_zone();
    test_zone_collapse_and_binding();
    test_pitch_bend_scaling();
    test_pressure_and_timbre();
    test_rpn_pb_range();
    test_explicit_toggle();
    test_mod_ids();
    test_upper_zone_expression();
    test_rpn_before_active();
    test_out_of_zone_and_mismatch();
    test_count0_mcm_with_toggle();
    test_master_nonzero_part_limit();
    printf("mpe-zone-tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
