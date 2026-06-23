#!/bin/sh
# mpe-test — MPE input across ALL THREE shells: the three MPE expression axes
# drive §9.5 per-voice modulation, deterministically and non-destructively, and
# the SAME gesture renders byte-identically no matter which shell decoded it.
#   CLAP  note expressions (TUNING/BRIGHTNESS/PRESSURE) — the per-note path.
#   VST3  Note Expression (kTuning/kBrightness) — Cubase + the per-note UI.
#   AU    classic MPE as RAW 16-channel MIDI (MCM + member channels) — Logic and
#         Ableton Live, collapsed onto one part by shell/mpe_zone.h.
#
# An MPE controller's per-note expression arrives in CLAP as note expressions
# (TUNING = pitch X, BRIGHTNESS = timbre Y, PRESSURE = Z) keyed by note_id; the
# CLAP shell maps each to a per-voice mod on the addressed voice (TUNING ->
# device per-voice pitch bend in semitones, PRESSURE -> per-voice loudness gain,
# BRIGHTNESS -> Filter Cutoff). clap-host injects these on chosen chord notes and
# renders OFFLINE (byte-exact), so we assert exact-hash RELATIONS (no baked MPE
# magic numbers — they hold on any board the oracle holds on):
#
#   NEUTRAL      bend 0 / pressure 0 == the plain chord (the device gates the
#                pitch/loudness paths on != 0, so an unbent voice is byte-exact).
#   PER-VOICE X  the SAME chord with a +4 st bend on note 0 vs 1 vs 2 -> three
#                DISTINCT mixes, each != plain (the bend lands on the addressed
#                voice, not the part).
#   DETERMINISTIC the bend render repeats byte-identically.
#   AXIS-INDEP   a pitch bend != a timbre (cutoff) move on the same voice (the
#                two axes drive different params, don't clobber).
#   PER-VOICE Z  pressure on note 0 vs 1 -> distinct, each != plain.
#   NONDESTRUCT  a plain chord after all the MPE renders == the first plain
#                (the per-voice expression never touches the base/recall).
#
# Mirrors clap-test.sh / note-expr-test.sh house style. Exit 0 / 2 N/A / 3 busy.
set -u
cd "$(dirname "$0")/.."

export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
SERIAL="$HARP_DEVICE_SERIAL"
H="${CLAPHOST:-build-vst/clap-host}"
CLAP="${CLAP:-}"
if [ -z "$CLAP" ]; then
    CLAP=$(find build-vst -name 'harp-clap.clap' 2>/dev/null | head -1)
    [ -z "$CLAP" ] && CLAP="build-vst/harp-clap.clap"
fi
PROBE="${PROBE:-./build/harp-probe}"
CHORD_HASH="eaed2355f68db8ed"   # re-baselined: drone removed (== golden-test.sh / clap-test.sh)
S="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
   --set 8=0.6 --set 9=0 --set 10=0.6 --set 11=0.5 --set 12=0 --set 13=0"
fail() { echo "MPE FAIL: $1"; exit 1; }

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "MPE FAIL: device claimed by Ableton Live — needs it exclusively"; exit 3
fi
if [ -x "$PROBE" ]; then
    "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL" \
        || { echo "MPE SKIP: board $SERIAL not on the bus"; exit 2; }
else
    echo "MPE SKIP: $PROBE not built"; exit 2
fi
[ -x "$H" ] || { echo "MPE SKIP: clap-host $H not built"; exit 2; }
[ -e "$CLAP" ] || { echo "MPE SKIP: $CLAP not built"; exit 2; }
echo "── mpe: CLAP note-expression -> §9.5 per-voice pitch/timbre/pressure on $SERIAL"

"$H" "$CLAP" $S --seconds 0.5 >/dev/null 2>&1 \
    || { echo "MPE FAIL: settle render did not complete (device busy/absent?)"; exit 3; }

# r ARGS... -> the owner main-mix hash of a chord render with extra ARGS
r() { "$H" "$CLAP" $S --chord 60,64,67 --seconds 2.0 "$@" --hash 2>/dev/null | grep output-hash | cut -d' ' -f2; }

PLAIN=$(r)
BEND0NEUT=$(r --bend 0 --bend-idx 0)
PRESS0NEUT=$(r --press 0 --press-idx 0)
B0=$(r --bend 4 --bend-idx 0)
B0R=$(r --bend 4 --bend-idx 0)   # repeat (determinism)
B1=$(r --bend 4 --bend-idx 1)
B2=$(r --bend 4 --bend-idx 2)
CUT0=$(r --brightness 1.0 --brightness-idx 0)  # timbre axis on voice 0
PR0=$(r --press 0.6 --press-idx 0)
PR1=$(r --press 0.6 --press-idx 1)
AFTER=$(r)

for n in PLAIN BEND0NEUT PRESS0NEUT B0 B0R B1 B2 CUT0 PR0 PR1 AFTER; do
    eval "v=\$$n"
    [ -n "$v" ] || { echo "MPE FAIL: $n produced no hash (device busy/absent?)"; exit 3; }
done
echo "   plain=$PLAIN  bend(neut)=$BEND0NEUT press(neut)=$PRESS0NEUT"
echo "   bend +4 v0=$B0 v1=$B1 v2=$B2 (repeat v0=$B0R) ; cutoff v0=$CUT0"
echo "   press .6 v0=$PR0 v1=$PR1 ; after=$AFTER"

[ "$PLAIN" = "$CHORD_HASH" ]   || fail "plain chord $PLAIN != chord golden $CHORD_HASH"
[ "$BEND0NEUT" = "$PLAIN" ]    || fail "bend 0 ($BEND0NEUT) != plain — neutral pitch must be byte-identical"
[ "$PRESS0NEUT" = "$PLAIN" ]   || fail "pressure 0 ($PRESS0NEUT) != plain — neutral pressure must be byte-identical"
[ "$B0" = "$B0R" ]             || fail "pitch bend not deterministic ($B0 vs $B0R)"
[ "$B0" != "$PLAIN" ]          || fail "pitch bend did not change the render"
[ "$B0" != "$B1" ] && [ "$B1" != "$B2" ] && [ "$B0" != "$B2" ] \
    || fail "per-voice pitch not distinct (v0=$B0 v1=$B1 v2=$B2) — the bend hit the part, not one voice"
[ "$CUT0" != "$B0" ]           || fail "a pitch bend == a timbre move on the same voice ($B0) — axes not independent"
[ "$PR0" != "$PLAIN" ]         || fail "pressure did not change the render"
[ "$PR0" != "$PR1" ]           || fail "per-voice pressure not distinct (v0=$PR0 v1=$PR1)"
[ "$AFTER" = "$PLAIN" ]        || fail "plain chord after MPE ($AFTER) != before — MPE altered the base (NOT non-destructive)"

# CROSS-FORMAT pitch: the SAME pitch gesture via VST3 Note Expression (Tuning)
# must render byte-identically to CLAP's per-note bend — one device, identical
# §9.5 mod regardless of which shell sent it. (VST3 Tuning is normalized about
# 0.5 over ±120 st; harp-vst3-host's --tuning takes semitones, so --tuning 4 ==
# CLAP --bend 4.) Skipped only if the VST3 host/bundle is absent on this rig.
V="${HOSTBIN:-build-vst/harp-vst3-host}"
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"
if [ -x "$V" ] && [ -e "$PLUG" ]; then
    rv() { "$V" "$PLUG" $S --chord 60,64,67 --seconds 2.0 "$@" --hash 2>/dev/null | grep output-hash | cut -d' ' -f2; }
    "$V" "$PLUG" $S --seconds 0.5 >/dev/null 2>&1
    VPLAIN=$(rv); VTUNE0=$(rv --tuning 0 --tuning-idx 0); VTUNE4=$(rv --tuning 4 --tuning-idx 0)
    echo "   VST3: plain=$VPLAIN tuning0=$VTUNE0 tuning+4=$VTUNE4  (CLAP bend+4=$B0)"
    [ -n "$VPLAIN" ] && [ -n "$VTUNE4" ] || fail "VST3 render produced no hash (device busy?)"
    [ "$VPLAIN" = "$CHORD_HASH" ]  || fail "VST3 plain $VPLAIN != chord golden $CHORD_HASH"
    [ "$VTUNE0" = "$PLAIN" ]       || fail "VST3 neutral Tuning ($VTUNE0) != plain — must be byte-identical"
    [ "$VTUNE4" = "$B0" ]          || fail "VST3 Tuning +4 ($VTUNE4) != CLAP bend +4 ($B0) — MPE pitch not cross-format byte-identical"
else
    echo "   (VST3 cross-format pitch check skipped: harp-vst3-host / VST3 bundle absent)"
fi

# CLASSIC-MPE over RAW 16-channel MIDI — what Logic / Ableton Live send to an AU
# (they do NOT emit VST3/CLAP note expressions). au-host injects an MPE Config
# Message + each chord note on its OWN member channel with per-note pitch /
# pressure / CC74; the shell's mpe_zone COLLAPSES the zone onto one part and
# carries each axis as the SAME §9.5 per-voice mod the note-expression path uses.
# Two byte-identity proofs make this airtight (no baked MPE magic numbers):
#   COLLAPSE   a neutral --mpe-chord (3 notes across 3 member channels) renders
#              IDENTICALLY to those notes via plain --chord — a member channel
#              never becomes the device part (§9.4), the whole zone is one part.
#   X-FORMAT   the SAME pitch gesture via raw MIDI (value14 6144 over the ±48
#              member range == -12 st exactly) == CLAP's per-note bend -12, and
#              raw CC74=127 == CLAP Brightness 1.0 (CUT0) — one device, identical
#              §9.5 mod regardless of which shell decoded the wire.
# macOS only (no AU on Linux): self-skips if au-host / the component are absent.
A="${HOSTBIN_AU:-build-vst/au-host}"
AUCOMP="${AUCOMP:-$HOME/Library/Audio/Plug-Ins/Components/harp-au.component}"
if [ -x "$A" ] && [ -e "$AUCOMP" ]; then
    "$A" $S --seconds 0.5 >/dev/null 2>&1               # AU settle
    AUPLAIN=$("$A" $S --chord 60,64,67 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    AUNEUT=$("$A" $S --mpe-chord 60,64,67 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    AUB0=$("$A" $S --mpe-chord 60,64,67 --mpe-bend -12 --mpe-bend-idx 0 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    AUB1=$("$A" $S --mpe-chord 60,64,67 --mpe-bend -12 --mpe-bend-idx 1 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    AUPR0=$("$A" $S --mpe-chord 60,64,67 --mpe-press 0.6 --mpe-press-idx 0 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    AUTB0=$("$A" $S --mpe-chord 60,64,67 --mpe-timbre 1.0 --mpe-timbre-idx 0 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    # SAME -12 st gesture but encoded over a NON-default ±24 member range (RPN 0):
    # the rendered semitones must be identical regardless of the range used to
    # encode them on the wire — exercises RPN-0 range parsing end-to-end.
    AUB0R=$("$A" $S --mpe-chord 60,64,67 --mpe-range 24 --mpe-bend -12 --mpe-bend-idx 0 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    CLBM12=$(r --bend -12 --bend-idx 0)                 # CLAP reference: same -12 gesture
    echo "   AU raw-MPE: plain=$AUPLAIN neutral=$AUNEUT (collapse) bend-12 v0=$AUB0 v1=$AUB1"
    echo "              press .6 v0=$AUPR0 timbre=$AUTB0 bend-12@±24=$AUB0R  (CLAP bend-12=$CLBM12 cutoff=$CUT0)"
    for n in AUPLAIN AUNEUT AUB0 AUB1 AUPR0 AUTB0 AUB0R CLBM12; do
        eval "v=\$$n"
        [ -n "$v" ] || { echo "MPE FAIL: AU $n produced no hash (device busy/absent?)"; exit 3; }
    done
    [ "$AUPLAIN" = "$CHORD_HASH" ] || fail "AU plain $AUPLAIN != chord golden $CHORD_HASH"
    [ "$AUNEUT" = "$AUPLAIN" ]     || fail "AU MPE zone collapse ($AUNEUT) != plain chord — a member-channel chord MUST collapse onto ONE part byte-identically (§9.4)"
    [ "$AUB0" != "$AUNEUT" ]       || fail "AU MPE pitch bend did not change the render"
    [ "$AUB0" != "$AUB1" ]         || fail "AU MPE pitch not per-voice (v0=$AUB0 v1=$AUB1) — the bend hit the part, not one voice"
    [ "$AUPR0" != "$AUNEUT" ]      || fail "AU MPE pressure did not change the render"
    [ "$AUB0" = "$CLBM12" ]        || fail "AU raw-MPE bend ($AUB0) != CLAP note-expr bend ($CLBM12) — raw 16-ch MPE pitch not cross-format byte-identical"
    [ "$AUB0R" = "$AUB0" ]         || fail "AU raw-MPE bend over ±24 range ($AUB0R) != over ±48 ($AUB0) — the rendered semitones must not depend on the RPN-0 range used to encode them"
    [ "$AUTB0" = "$CUT0" ]         || fail "AU raw-MPE CC74 timbre ($AUTB0) != CLAP Brightness ($CUT0) — timbre axis not cross-format byte-identical"

    # MASTER-channel (zone-wide) bend reaches the INSTANCE's part, not a hardcoded
    # part 0. A master bend is a part-wide mod (voiceKey 0) the runtime routes to
    # the emitting source's part; it bends EVERY active voice on that part. Proof
    # is per-part — a chord + master bend bends the whole chord:
    #   p0: chord on part 0 + master bend -> != neutral chord on part 0.
    #   p3: chord on part 3 + master bend -> != neutral chord on part 3. With the
    #       old part-0-only bug the bend was lost to part 0 (whose only voice, the
    #       drone, is unallocated and immune), leaving part 3's chord unbent == p3.
    AUNEUT3=$("$A" $S --part 3 --mpe-chord 60,64,67 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    AUMB0=$("$A" $S --mpe-chord 60,64,67 --mpe-master-bend -12 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    AUMB3=$("$A" $S --part 3 --mpe-chord 60,64,67 --mpe-master-bend -12 --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2)
    echo "              master-bend: neutral-p3=$AUNEUT3 bend-p0=$AUMB0 bend-p3=$AUMB3"
    for n in AUNEUT3 AUMB0 AUMB3; do
        eval "v=\$$n"
        [ -n "$v" ] || { echo "MPE FAIL: AU $n produced no hash (device busy/absent?)"; exit 3; }
    done
    [ "$AUMB0" != "$AUNEUT" ]       || fail "AU master (zone-wide) bend on part 0 did not change the render — the master path is dead"
    [ "$AUMB3" != "$AUNEUT3" ]      || fail "AU master bend on a part-3 instance left the chord unbent ($AUMB3 == $AUNEUT3) — a part-wide mod must reach the instance's part, not always part 0"
else
    echo "   (AU raw-MPE check skipped: au-host / AU component absent — macOS only)"
fi

# VST3 RAW-MIDI MPE (Ableton Live on VST3 -> IMidiMapping). The "MPE" toggle
# engages the zone; per-channel pitch-bend / CC74 arrive as mapped param changes
# the shell reconstructs into the SAME §9.5 per-voice mods. Cross-format: VST3 raw
# == CLAP note-expression (== AU raw, where the AU ran), one device. And the
# toggle PERSISTS in the recall part byte (bit 7): a reloaded MPE-on project
# engages MPE with no re-arming. Uses the VST3 host/bundle resolved above.
if [ -x "$V" ] && [ -e "$PLUG" ]; then
    vrm() { "$V" "$PLUG" $S --seconds 2.0 "$@" --hash 2>/dev/null | grep output-hash | cut -d' ' -f2; }
    CLM12=$(r --bend -12 --bend-idx 0)                  # CLAP reference: -12 st
    VRNEUT=$(vrm --mpe-chord 60,64,67)
    VRB0=$(vrm --mpe-chord 60,64,67 --mpe-bend -12 --mpe-bend-idx 0)
    VRB1=$(vrm --mpe-chord 60,64,67 --mpe-bend -12 --mpe-bend-idx 1)
    VRTB=$(vrm --mpe-chord 60,64,67 --mpe-timbre 1.0 --mpe-timbre-idx 0)
    # persistence: save an MPE-armed project, reload it WITHOUT re-arming
    # (--mpe-no-arm) — MPE must engage solely from the persisted toggle (bit 7 of
    # the recall part byte). The control (no state, no arm) must NOT engage.
    VST=$(mktemp -t harp-mpe.XXXXXX)
    vrm --mpe-chord 60,64,67 --mpe-bend -12 --mpe-bend-idx 0 --save-state "$VST" >/dev/null 2>&1
    VRLOAD=$(vrm --load-state "$VST" --mpe-chord 60,64,67 --mpe-bend -12 --mpe-bend-idx 0 --mpe-no-arm)
    VRCTRL=$(vrm --mpe-chord 60,64,67 --mpe-bend -12 --mpe-bend-idx 0 --mpe-no-arm)
    rm -f "$VST"
    echo "   VST3 raw-MPE: neutral=$VRNEUT bend-12 v0=$VRB0 v1=$VRB1 timbre=$VRTB"
    echo "                persist reload=$VRLOAD control=$VRCTRL  (CLAP-12=$CLM12 cutoff=$CUT0 AU-12=${AUB0:-n/a})"
    for nm in CLM12 VRNEUT VRB0 VRB1 VRTB VRLOAD VRCTRL; do
        eval "v=\$$nm"
        [ -n "$v" ] || { echo "MPE FAIL: VST3 raw-MPE $nm produced no hash (device busy/absent?)"; exit 3; }
    done
    [ "$VRNEUT" = "$CHORD_HASH" ] || fail "VST3 raw-MPE zone collapse ($VRNEUT) != plain chord $CHORD_HASH — a member-channel chord MUST collapse onto ONE part (§9.4)"
    [ "$VRB0" = "$CLM12" ]        || fail "VST3 raw-MPE bend -12 ($VRB0) != CLAP note-expr bend -12 ($CLM12) — raw MPE not cross-format byte-identical"
    [ "$VRB0" != "$VRB1" ]        || fail "VST3 raw-MPE pitch not per-voice (v0=$VRB0 v1=$VRB1) — the bend hit the part, not one voice"
    [ "$VRTB" = "$CUT0" ]         || fail "VST3 raw-MPE CC74 timbre ($VRTB) != CLAP Brightness ($CUT0) — timbre axis not cross-format byte-identical"
    [ "$VRLOAD" = "$VRB0" ]       || fail "VST3 MPE toggle did NOT persist: reloaded MPE-on project ($VRLOAD) != armed bend ($VRB0)"
    [ "$VRCTRL" != "$VRB0" ]      || fail "VST3 raw-MPE persistence control ($VRCTRL) == armed ($VRB0) — no-state + no-arm must NOT engage MPE"
    # cross-check against the AU raw path ONLY where it ran (macOS); on Linux the
    # AU section is skipped so AUB0 is unset — ${AUB0:-} keeps `set -u` happy.
    [ -z "${AUB0:-}" ] || [ "$VRB0" = "$AUB0" ] || fail "VST3 raw-MPE bend ($VRB0) != AU raw-MPE bend ($AUB0) — the two raw-MIDI paths must render identically"
else
    echo "   (VST3 raw-MPE check skipped: harp-vst3-host / VST3 bundle absent)"
fi

echo "MPE PASS (on $SERIAL: CLAP/VST3/AU all drive §9.5 per-voice pitch/timbre/"
echo "   pressure — neutral is byte-identical, voices bend to distinct mixes, pitch"
echo "   and timbre are independent axes, the base/recall is untouched, and the same"
echo "   gesture renders identically across shells: VST3 Tuning == CLAP bend, and raw"
echo "   16-channel MPE (AU) collapses onto one part == the plain chord + == CLAP bend)"
