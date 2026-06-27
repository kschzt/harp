#!/bin/sh
# mpe-test — per-note expression input across the two shells that carry it: the
# three expression axes drive §9.5 per-voice modulation, deterministically and
# non-destructively, and the SAME gesture renders byte-identically in both.
#   CLAP  note expressions (TUNING/BRIGHTNESS/PRESSURE) — the per-note path.
#   VST3  Note Expression (kTuning/kBrightness) — Cubase + the per-note UI.
# Raw 16-channel MPE (Logic/Live classic MPE, the shell/mpe_zone.h zone-collapse)
# is RETIRED: on the multi-out main a MIDI channel IS a device part (§9.4), so it
# can't also be an MPE member channel. The AU shell has no per-note path now and
# is not exercised here; per-note expression is CLAP/VST3 only.
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
CHORD_HASH="979fbd7d2b2aafe6"   # re-baselined: §6.4 PDC fold #75 (== golden-test.sh / clap-test.sh)
S="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
   --set 7=0.6 --set 8=0 --set 9=0.6 --set 10=0.5 --set 11=0 --set 12=0"
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

echo "MPE PASS (on $SERIAL: CLAP note-expression + VST3 Note Expression drive §9.5"
echo "   per-voice pitch/timbre/pressure — neutral is byte-identical, voices bend to"
echo "   distinct mixes, pitch and timbre are independent axes, the base/recall is"
echo "   untouched, and the same gesture renders identically across shells: VST3"
echo "   Tuning == CLAP bend)"
