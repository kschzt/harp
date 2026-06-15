#!/bin/sh
# REAPER end-to-end test: a real third-party VST3 host (REAPER), headless under
# xvfb, loads harp-shell.vst3 and renders a MIDI-driven project THROUGH the real
# device over USB. Asserts (1) two renders are byte-identical audio — determinism
# through a real DAW (the device's parameter state is pinned via the front panel
# between runs, since params persist as recall state and DAW automation
# interpolates from the current value) — and (2) the device dropped/delayed
# nothing. Run on a host with the device + REAPER + xvfb (the hardware runner).
#
# Env overrides: HARP_REAPER, HARP_PROBE, HARP_SHELL.
set -e

REAPER=${HARP_REAPER:-/opt/REAPER/reaper}
PROBE=${HARP_PROBE:-./build/harp-probe}
SHELL_VST3=${HARP_SHELL:-$HOME/.vst3/harp-shell.vst3}
SCRIPTS="$HOME/.config/REAPER/Scripts"
CACHE="$HOME/.config/REAPER/reaper-vstplugins64.ini"
HERE=$(cd "$(dirname "$0")" && pwd)
OUTDIR=$(mktemp -d)   # per-run, per-user — avoids cross-user /tmp collisions

[ -x "$REAPER" ] || { echo "FAIL: REAPER not installed at $REAPER (provision the runner: /opt/REAPER)"; exit 1; }
command -v xvfb-run >/dev/null || { echo "FAIL: xvfb-run missing (provision the runner)"; exit 1; }
[ -e "$SHELL_VST3" ] || { echo "FAIL: shell not installed at $SHELL_VST3"; exit 1; }
mkdir -p "$SCRIPTS"
cleanup() { rm -f "$SCRIPTS/__startup.lua"; rm -rf "$OUTDIR"; }
trap cleanup EXIT INT TERM

echo "[reaper-e2e] warm-up launch to scan ~/.vst3 ..."
xvfb-run -a timeout 40 "$REAPER" -nonewinst >/dev/null 2>&1 || true
if ! grep -qi 'harp_shell\|HARP RefDev' "$CACHE" 2>/dev/null; then
    echo "FAIL: REAPER did not scan harp-shell (check VST3 path)"; exit 1
fi
echo "[reaper-e2e] REAPER scanned the shell."

cp "$HERE/reaper-e2e.lua" "$SCRIPTS/__startup.lua"

pin_state() {
    for i in 1 2 3 4 5 6 7 8; do "$PROBE" -d usb knob "$i" 0.5 >/dev/null 2>&1; done
    "$PROBE" -d usb knob 9 0.0 >/dev/null 2>&1
}
ctr() { "$PROBE" -d usb counters 2>/dev/null | grep -E "[. ]$1 = " | grep -oE '[0-9]+$' | tail -1; }
render() { # $1 = basename
    rm -f "$OUTDIR/$1.wav" "$OUTDIR/status.txt"
    HARP_E2E_OUTDIR="$OUTDIR" HARP_E2E_NAME="$1" HARP_E2E_STATUS="$OUTDIR/status.txt" \
        xvfb-run -a timeout 90 "$REAPER" -nonewinst >/dev/null 2>&1 || true
    for _ in $(seq 1 30); do
        grep -q rendered "$OUTDIR/status.txt" 2>/dev/null && break
        grep -q ERROR "$OUTDIR/status.txt" 2>/dev/null && break
        sleep 1
    done
    echo "  status: $(cat "$OUTDIR/status.txt" 2>/dev/null)"
    [ -f "$OUTDIR/$1.wav" ] || { echo "FAIL: no render output $OUTDIR/$1.wav"; exit 1; }
}

b_drop=$(ctr evq_drops); b_late=$(ctr evt_late); b_fto=$(ctr fence_timeouts)

echo "[reaper-e2e] render 1 ..."; pin_state; render r1
echo "[reaper-e2e] render 2 ..."; pin_state; render r2

fail=0
# Compare the PCM data only — REAPER stamps a render timestamp into a WAV
# metadata chunk, so the audio (data chunk) is what must be byte-identical.
if python3 - "$OUTDIR/r1.wav" "$OUTDIR/r2.wav" <<'PY'
import sys, wave
def pcm(p):
    w = wave.open(p, "rb"); d = w.readframes(w.getnframes()); w.close(); return d
sys.exit(0 if pcm(sys.argv[1]) == pcm(sys.argv[2]) else 1)
PY
then
    echo "PASS determinism: two REAPER renders are byte-identical audio through the device"
else
    echo "FAIL determinism: REAPER render PCM differs"
    fail=1
fi

a_drop=$(ctr evq_drops); a_late=$(ctr evt_late); a_fto=$(ctr fence_timeouts)
for pair in "evq_drops $b_drop $a_drop" "evt_late $b_late $a_late" "fence_timeouts $b_fto $a_fto"; do
    set -- $pair
    d=$(( ${3:-0} - ${2:-0} ))
    echo "  $1 delta=$d"
    [ "$d" -ne 0 ] && { echo "FAIL: $1 rose by $d"; fail=1; }
done

[ "$fail" -eq 0 ] && echo "PASS: REAPER e2e — real-DAW render, byte-deterministic, lossless" || exit 1
