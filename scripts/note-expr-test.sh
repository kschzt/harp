#!/bin/sh
# note-expr-test — Phase 3: VST3 Note Expression -> §9.5 per-voice modulation.
#
# A held chord {60,64,67} renders through the REAL VST3 plugin; a VST3 Brightness
# Note Expression on ONE chord note is mapped (shell/plugin.cpp) to a SIGNED
# Filter-Cutoff (device param 3) offset on THAT note's voice only — the §9.4
# non-destructive, §9.5 per-voice modulation path. Like golden/alias-group-e2e
# this renders OFFLINE (harp-vst3-host), so the owner main-mix bytes are
# deterministic and we assert exact-hash RELATIONS (no RMS, no baked magic
# numbers — the relations hold on any board the oracle holds on):
#
#   NEUTRAL    brightness 0.5 (offset 0) == plain chord. The whole pipeline
#              (shell -> runtime queueMod -> device decode -> per-voice apply) is
#              LIVE, but a zero offset is byte-identical to no modulation — the
#              end-to-end plumbing + neutrality proof in one.
#   MODULATE   brightness 1.0 (cutoff up, clamped) and 0.0 (down) each DIFFER
#              from plain AND from each other, and repeat byte-identically — the
#              mod reaches the render and is deterministic.
#   PER-VOICE  the SAME chord with Brightness on note index 0 vs 1 vs 2 yields
#              THREE DISTINCT hashes — the mod lands on the ADDRESSED voice, not
#              the whole part (a part-wide mod would render all three identical).
#   NONDESTRUCT a plain chord AFTER a modulated one == the first plain render —
#              the stored base cutoff (and thus recall) is untouched (§9.4).
#
# House conventions mirror golden-test.sh / alias-group-e2e.sh: build-vst host,
# the $PLUG bundle (Linux CI overrides -> ~/.vst3), pinned HARP_DEVICE_SERIAL,
# the Live claim guard, harp-probe to confirm the board. Needs ONE board, Live
# closed. Exit 0 pass / 2 N/A (board/host absent) / 3 device busy.
set -u
cd "$(dirname "$0")/.."

export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
SERIAL="$HARP_DEVICE_SERIAL"
V="${HOSTBIN:-build-vst/harp-vst3-host}"
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"
PROBE="${PROBE:-./build/harp-probe}"
# the canonical chord-voice settle: drone+level+tone+fast env, cutoff at 0.7 so a
# +0.5 mod clamps up at 1.0 and a -0.5 mod lands at 0.2 — both clearly audible.
S="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
   --set 7=0.5 --set 8=0.6 --set 9=0 --set 10=0.6 --set 11=0.5 --set 12=0 --set 13=0"
CHORD="60,64,67"
DUR="2.0"

fail() { echo "NOTE-EXPR FAIL: $1"; exit 1; }

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "NOTE-EXPR FAIL: device claimed by Ableton Live — the suite needs it exclusively"; exit 3
fi
if [ -x "$PROBE" ]; then
    "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL" \
        || { echo "NOTE-EXPR SKIP: board $SERIAL not on the bus"; exit 2; }
else
    echo "NOTE-EXPR SKIP: $PROBE not built"; exit 2
fi
[ -x "$V" ] || { echo "NOTE-EXPR SKIP: host $V not built"; exit 2; }
echo "── note-expr: VST3 Brightness -> per-voice Filter Cutoff on $SERIAL (chord $CHORD)"

# settle the base voice once (params land, no glide in the measured renders)
$V "$PLUG" $S --seconds 0.5 >/dev/null 2>&1 \
    || { echo "NOTE-EXPR FAIL: settle render did not complete (device busy/absent?)"; exit 3; }

# h ARGS... — render the chord through the plugin with extra ARGS, echo the owner
# main-mix content hash (golden-test's extractor). Empty -> device busy/absent.
h() { $V "$PLUG" $S --chord "$CHORD" --seconds "$DUR" "$@" --hash 2>/dev/null \
        | grep output-hash | cut -d' ' -f2; }

PLAIN=$(h)                          # no Note Expression at all
NEUTRAL=$(h --brightness 0.5)       # pipeline live, offset 0
BRIGHT1=$(h --brightness 1.0)       # voice idx0 cutoff up
BRIGHT2=$(h --brightness 1.0)       # ... again (determinism)
DARK=$(h --brightness 0.0)          # voice idx0 cutoff down
V0=$BRIGHT1                          # idx0 already captured above
V1=$(h --brightness 1.0 --brightness-idx 1)
V2=$(h --brightness 1.0 --brightness-idx 2)
AFTER=$(h)                          # plain again, AFTER the modulated renders

for n in PLAIN NEUTRAL BRIGHT1 BRIGHT2 DARK V1 V2 AFTER; do
    eval "v=\$$n"
    [ -n "$v" ] || { echo "NOTE-EXPR FAIL: $n produced no hash (device busy/absent?)"; exit 3; }
done
echo "   plain=$PLAIN neutral=$NEUTRAL bright=$BRIGHT1 dark=$DARK"
echo "   per-voice idx0=$V0 idx1=$V1 idx2=$V2 ; after=$AFTER"

[ "$NEUTRAL" = "$PLAIN" ]  || fail "brightness 0.5 ($NEUTRAL) != plain ($PLAIN) — a neutral offset must be byte-identical (pipeline broken or not neutral)"
[ "$BRIGHT1" = "$BRIGHT2" ] || fail "brightness 1.0 not deterministic ($BRIGHT1 vs $BRIGHT2)"
[ "$BRIGHT1" != "$PLAIN" ] || fail "brightness 1.0 did not change the render — the per-voice mod never reached the device"
[ "$DARK" != "$PLAIN" ]    || fail "brightness 0.0 did not change the render"
[ "$DARK" != "$BRIGHT1" ]  || fail "brightness 0.0 == 1.0 ($DARK) — the offset sign/magnitude is not applied"
[ "$V0" != "$V1" ] && [ "$V1" != "$V2" ] && [ "$V0" != "$V2" ] \
    || fail "per-voice not distinct (idx0=$V0 idx1=$V1 idx2=$V2) — the mod hit the part, not one voice"
[ "$AFTER" = "$PLAIN" ]    || fail "plain chord after modulation ($AFTER) != before ($PLAIN) — the mod altered the base (NOT non-destructive)"

echo "NOTE-EXPR PASS (on $SERIAL: Brightness -> per-voice Filter Cutoff — neutral is"
echo "   byte-identical, ±offset modulates deterministically, three voices give three"
echo "   distinct mixes, and the base/recall is untouched after)"
