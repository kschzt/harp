#!/bin/sh
# clap-test — Phase 4: the CLAP shell renders byte-identically to the VST3/AU
# shells (same HarpRuntime core), and CLAP's native per-note PARAM_MOD drives
# §9.5 per-voice modulation.
#
# clap-host dlopen()s harp-clap.clap and drives it OFFLINE (host-paced) with the
# exact render structure of harp-vst3-host (rate 48000, block 256, set-at-block-0,
# chord held 0.1s..end, notes at 0.6s spacing), so the device sees an identical
# event/pull stream. We assert against THE oracle hashes (golden-test.sh) — a
# CLAP render that matches them is byte-identical across formats — plus the
# per-voice mod relations (no baked mod hashes, so they hold on any board):
#
#   CROSS-FORMAT  CLAP melody == VST3/AU golden (78794729), CLAP chord == chord
#                 golden (eaed2355). The third shell renders the SAME bytes.
#   PER-VOICE     a per-note PARAM_MOD (Filter Cutoff, amount = brightness-0.5)
#                 on chord note idx 0/1/2 gives THREE DISTINCT mixes — the mod
#                 lands on the addressed voice, not the part. brightness 1.0 also
#                 byte-MATCHES the VST3 Note Expression render (note-expr-test):
#                 a CLAP per-note mod and a VST3 note expression are identical.
#   NEUTRAL       brightness 0.5 (amount 0) == plain chord (pipeline live, inert).
#   NONDESTRUCT   a plain chord after a modulated one == the first (base untouched).
#
# Conventions mirror golden-test.sh / note-expr-test.sh. Exit 0 pass / 2 N/A
# (board / clap-host / .clap absent) / 3 device busy.
set -u
cd "$(dirname "$0")/.."

export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
SERIAL="$HARP_DEVICE_SERIAL"
H="${CLAPHOST:-build-vst/clap-host}"
CLAP="${CLAP:-}"
if [ -z "$CLAP" ]; then # locate the built module (its subdir differs across generators)
    CLAP=$(find build-vst -name 'harp-clap.clap' 2>/dev/null | head -1)
    [ -z "$CLAP" ] && CLAP="build-vst/harp-clap.clap"
fi
PROBE="${PROBE:-./build/harp-probe}"

GOLDEN_HASH="eb6a3b838d1d8ec3"  # re-baselined: §6.4 PDC fold #75 (== golden-test.sh)
CHORD_HASH="979fbd7d2b2aafe6"   # re-baselined: §6.4 PDC fold #75 (== golden-test.sh)
VST3_MOD_HASH="bee1987d778baaaa" # re-baselined: drone removed

S="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
   --set 7=0.6 --set 8=0 --set 9=0.6 --set 10=0.5 --set 11=0 --set 12=0"
fail() { echo "CLAP FAIL: $1"; exit 1; }

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "CLAP FAIL: device claimed by Ableton Live — the suite needs it exclusively"; exit 3
fi
if [ -x "$PROBE" ]; then
    "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL" \
        || { echo "CLAP SKIP: board $SERIAL not on the bus"; exit 2; }
else
    echo "CLAP SKIP: $PROBE not built"; exit 2
fi
[ -x "$H" ] || { echo "CLAP SKIP: clap-host $H not built"; exit 2; }
[ -e "$CLAP" ] || { echo "CLAP SKIP: $CLAP not built"; exit 2; }
echo "── clap: CLAP shell cross-format identity + per-voice mod on $SERIAL"

"$H" "$CLAP" $S --seconds 0.5 >/dev/null 2>&1 \
    || { echo "CLAP FAIL: settle render did not complete (device busy/absent?)"; exit 3; }

# r ARGS... — render through the CLAP shell, echo the owner main-mix hash
r() { "$H" "$CLAP" $S "$@" --hash 2>/dev/null | grep output-hash | cut -d' ' -f2; }

MELODY=$(r --notes 62,69,74,65 --seconds 2.6)
CHORD=$(r --chord 60,64,67 --seconds 2.0)
NEUTRAL=$(r --chord 60,64,67 --brightness 0.5 --seconds 2.0)
MOD=$(r --chord 60,64,67 --brightness 1.0 --seconds 2.0)
V0=$MOD
V1=$(r --chord 60,64,67 --brightness 1.0 --brightness-idx 1 --seconds 2.0)
V2=$(r --chord 60,64,67 --brightness 1.0 --brightness-idx 2 --seconds 2.0)
AFTER=$(r --chord 60,64,67 --seconds 2.0)

for n in MELODY CHORD NEUTRAL MOD V1 V2 AFTER; do
    eval "v=\$$n"
    [ -n "$v" ] || { echo "CLAP FAIL: $n produced no hash (device busy/absent?)"; exit 3; }
done
echo "   melody=$MELODY chord=$CHORD neutral=$NEUTRAL mod=$MOD"
echo "   per-voice idx0=$V0 idx1=$V1 idx2=$V2 ; after=$AFTER"

[ "$MELODY" = "$GOLDEN_HASH" ] || fail "CLAP melody $MELODY != golden $GOLDEN_HASH — not byte-identical to VST3/AU"
[ "$CHORD" = "$CHORD_HASH" ]   || fail "CLAP chord $CHORD != chord golden $CHORD_HASH"
[ "$NEUTRAL" = "$CHORD_HASH" ] || fail "CLAP neutral mod $NEUTRAL != plain chord — a zero offset must be byte-identical"
[ "$MOD" = "$VST3_MOD_HASH" ]  || fail "CLAP per-note mod $MOD != VST3 Note Expression $VST3_MOD_HASH — the two formats must modulate identically"
[ "$MOD" != "$CHORD_HASH" ]    || fail "CLAP mod did not change the render"
[ "$V0" != "$V1" ] && [ "$V1" != "$V2" ] && [ "$V0" != "$V2" ] \
    || fail "per-voice not distinct (idx0=$V0 idx1=$V1 idx2=$V2) — the mod hit the part, not one voice"
[ "$AFTER" = "$CHORD_HASH" ]   || fail "plain chord after modulation ($AFTER) != before — the mod altered the base (NOT non-destructive)"

echo "CLAP PASS (on $SERIAL: the CLAP shell renders melody/chord byte-identically to"
echo "   VST3/AU; CLAP per-note PARAM_MOD == VST3 Note Expression ($MOD); three voices"
echo "   give three distinct mixes; neutral is inert and the base/recall is untouched)"
