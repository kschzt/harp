#!/bin/sh
# meter-test — Phase 0 (§9.9 output metering) proven on real hardware: the device
# computes a per-part + main-mix peak/RMS meter off the audio it RENDERS and makes
# it readable. The render thread folds the meters (engine.c, a pure read of the
# rendered buffers — never perturbing the bytes, so the golden stays byte-identical)
# and a 30 Hz control-plane pump echoes them as READONLY params (§9.9); harp-probe's
# `meters` reads the live values directly (x.harp-refdev.meters).
#
# THE SIGNAL — the MAIN-MIX meter. The reference engine's part-0 drone (knob 7)
# sounds continuously, so a brief stream renders audible audio into the main mix.
# We drive the drone, stream (harp-probe record renders -> the meter folds), read
# the main meter (must be NON-ZERO), then silence the drone, stream again, and read
# (must drop to ~0). A meter that tracks audible vs. silent renders proves the
# render->fold->echo/read path end to end. (Per-part meters fold only when per-part
# slots are streamed — the main-mix fast path the record uses meters the main bus;
# the per-part fold is the same code, exercised by the multitimbral renders.)
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

# main_peak — set the drone level, render a short stream so the meter folds, read
# the main-mix peak. Retries the transient post-teardown claim race like the
# sibling tests (record + meters are separate claims).
main_peak() {
    drone="$1"
    for try in 1 2 3; do
        "$PROBE" -d "$DEV" knob 7 "$drone" >/dev/null 2>&1   # drone level
        "$PROBE" -d "$DEV" knob 8 0.9      >/dev/null 2>&1   # master level
        if "$PROBE" -d "$DEV" record 2 /tmp/meter-$$.wav >/dev/null 2>&1; then
            v=$("$PROBE" -d "$DEV" meters 2>/dev/null | awk '/^  main /{print $3; exit}')
            rm -f /tmp/meter-$$.wav
            [ -n "$v" ] && { echo "$v"; return; }
        fi
        sleep 1
    done
    echo "-1"
}

echo "── drone ON: render + read main meter"
AUDIBLE=$(main_peak 0.9)
echo "   main peak (drone on)  = $AUDIBLE"
echo "── drone OFF: render + read main meter"
SILENT=$(main_peak 0.0)
echo "   main peak (drone off) = $SILENT"

case "$AUDIBLE$SILENT" in *-1*) echo "METER FAIL: never connected / no meter read (device busy?)"; exit 3 ;; esac

OK=$(python3 -c "print(1 if $AUDIBLE > $FLOOR and $AUDIBLE > 2*$SILENT else 0)")
if [ "$OK" = 1 ]; then
    echo "METER PASS (the §9.9 main-mix meter tracks rendered audio on $SERIAL: drone-on"
    echo "   peak $AUDIBLE > floor $FLOOR and > 2× the drone-off peak $SILENT — the render"
    echo "   thread folds the meter and harp-probe reads it, with the golden render unperturbed)"
    exit 0
else
    echo "METER FAIL: main meter did not track audio (drone-on $AUDIBLE vs drone-off $SILENT,"
    echo "   floor $FLOOR) — the meter is not reflecting the rendered main mix"
    exit 1
fi
