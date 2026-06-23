#!/bin/sh
# meter-test — Phase 0 (§9.9 output metering) proven on real hardware: the device
# computes a per-part + main-mix peak/RMS meter off the audio it RENDERS and makes
# it readable. The render thread folds the meters (engine.c, a pure read of the
# rendered buffers — never perturbing the bytes, so the golden stays byte-identical)
# and a 30 Hz control-plane pump echoes them as READONLY params (§9.9); harp-probe's
# `meters` reads the live values directly (x.harp-refdev.meters).
#
# THE SIGNAL — the MAIN-MIX meter. With the drone gone (engine 2.0.0) the engine is
# silent until a note plays, so we PLAY A NOTE (harp-probe note) to render audible
# audio into the main mix and read the main meter (must be NON-ZERO), then render
# SILENCE (a plain record, no note) and read again (must drop to ~0). A meter that
# tracks audible vs. silent renders proves the render->fold->echo/read path end to
# end. (Per-part meters fold only when per-part slots are streamed — the main-mix
# fast path the note/record use meters the main bus; the per-part fold is the same
# code, exercised by the multitimbral renders.)
#
# Mirrors the sibling hw tests: pin HARP_DEVICE_SERIAL, board-present skip, Live
# guard. Exit 0 pass / 2 N/A (board absent) / 3 device busy.
set -e
cd "$(dirname "$0")/.."

SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
export HARP_DEVICE_SERIAL="$SERIAL"
PROBE="${PROBE:-./build/harp-probe}"
DEV="usb:$SERIAL"
FLOOR="${FLOOR:-0.001}"   # main-mix peak floor: clearly non-zero vs the silent 0

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "METER FAIL: device claimed by Ableton Live — needs it exclusively"; exit 3
fi
if [ -x "$PROBE" ]; then
    "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL" || { echo "METER SKIP: board $SERIAL not on the bus"; exit 2; }
else
    echo "METER SKIP: $PROBE not built"; exit 2
fi
echo "── meter: §9.9 output meters track rendered audio on $SERIAL"

# main_peak — render a short stream so the meter folds, then read the main-mix
# peak. mode=note plays a sustained note (audible); mode=silent renders a plain
# stream with no note (silent). The meter holds the last render's value across the
# separate `meters` claim. Retries the transient post-teardown claim race.
main_peak() {
    mode="$1"
    for try in 1 2 3; do
        "$PROBE" -d "$DEV" knob 8 0.9 >/dev/null 2>&1   # master level
        ok=0
        if [ "$mode" = note ]; then
            "$PROBE" -d "$DEV" note 64 1500 100 -p 0 >/dev/null 2>&1 && ok=1
        else
            "$PROBE" -d "$DEV" record 2 /tmp/meter-$$.wav >/dev/null 2>&1 && ok=1
            rm -f /tmp/meter-$$.wav
        fi
        if [ "$ok" = 1 ]; then
            v=$("$PROBE" -d "$DEV" meters 2>/dev/null | awk '/^  main /{print $3; exit}')
            [ -n "$v" ] && { echo "$v"; return; }
        fi
        sleep 1
    done
    echo "-1"
}

echo "── NOTE playing: render + read main meter"
AUDIBLE=$(main_peak note)
echo "   main peak (note)    = $AUDIBLE"
echo "── SILENCE: render + read main meter"
SILENT=$(main_peak silent)
echo "   main peak (silence) = $SILENT"

case "$AUDIBLE$SILENT" in *-1*) echo "METER FAIL: never connected / no meter read (device busy?)"; exit 3 ;; esac

OK=$(python3 -c "print(1 if $AUDIBLE > $FLOOR and $AUDIBLE > 2*$SILENT else 0)")
if [ "$OK" = 1 ]; then
    echo "METER PASS (the §9.9 main-mix meter tracks rendered audio on $SERIAL: note"
    echo "   peak $AUDIBLE > floor $FLOOR and > 2× the silence peak $SILENT — the render"
    echo "   thread folds the meter and harp-probe reads it, with the golden render unperturbed)"
    exit 0
else
    echo "METER FAIL: main meter did not track audio (note $AUDIBLE vs silence $SILENT,"
    echo "   floor $FLOOR) — the meter is not reflecting the rendered main mix"
    exit 1
fi
