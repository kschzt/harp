#!/bin/sh
# REAPER end-to-end test: a real third-party VST3 host (REAPER), headless under
# xvfb, loads harp-shell.vst3 and drives the real device over USB. Two scenarios:
#
#   1. Determinism — render the same pinned device state twice; the audio must be
#      byte-identical (determinism through a real DAW's render path).
#   2. Recall round-trip — render state A and SAVE the project, then SCRAMBLE the
#      device to a different state B, then REOPEN the saved project. Loading it
#      must re-push A onto the device (the §10 Recall Bundle, restored through a
#      real DAW's setState), so the reopened render must equal the saved render
#      AND the scrambled render must differ from it (or the test is vacuous).
#
# Device params are pinned via the front panel (harp-probe knob) — they persist
# as recall state and DAW automation interpolates from the current value, so an
# explicit pin is what makes the comparison meaningful. Also asserts the device
# dropped/delayed nothing. Run on a host with the device + REAPER + xvfb (the
# hardware runner). Env overrides: HARP_REAPER, HARP_PROBE, HARP_SHELL.
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

# pin the device to a known state. $1 = value for knobs 1-8 (arp/knob 9 stays off
# so determinism does not depend on transport-anchored arp timing).
pin() {
    for i in 1 2 3 4 5 6 7 8; do "$PROBE" -d usb knob "$i" "$1" >/dev/null 2>&1; done
    "$PROBE" -d usb knob 9 0.0 >/dev/null 2>&1
}
ctr() { "$PROBE" -d usb counters 2>/dev/null | grep -E "[. ]$1 = " | grep -oE '[0-9]+$' | tail -1; }

# render() $1=basename $2=mode(build|open) $3=path (save target if build, project if open)
render() {
    R_NAME="$1"; R_MODE="${2:-build}"; R_SAVE=""; R_PROJECT=""
    if [ "$R_MODE" = "open" ]; then R_PROJECT="$3"; else R_SAVE="$3"; fi
    rm -f "$OUTDIR/$R_NAME.wav" "$OUTDIR/status.txt"
    HARP_E2E_OUTDIR="$OUTDIR" HARP_E2E_NAME="$R_NAME" HARP_E2E_STATUS="$OUTDIR/status.txt" \
    HARP_E2E_MODE="$R_MODE" HARP_E2E_SAVE="$R_SAVE" HARP_E2E_PROJECT="$R_PROJECT" \
        xvfb-run -a timeout 120 "$REAPER" -nonewinst >/dev/null 2>&1 || true
    for _ in $(seq 1 40); do
        grep -q rendered "$OUTDIR/status.txt" 2>/dev/null && break
        grep -q ERROR "$OUTDIR/status.txt" 2>/dev/null && break
        sleep 1
    done
    echo "  [$R_NAME/$R_MODE] status: $(cat "$OUTDIR/status.txt" 2>/dev/null)"
    [ -f "$OUTDIR/$R_NAME.wav" ] || { echo "FAIL: no render output $OUTDIR/$R_NAME.wav"; exit 1; }
}

# byte-compare the PCM data only — REAPER stamps a render timestamp into a WAV
# metadata chunk, so the audio (data chunk) is what must match. Returns 0 if equal.
pcm_eq() {
    python3 - "$1" "$2" <<'PY'
import sys, wave
def pcm(p):
    w = wave.open(p, "rb"); d = w.readframes(w.getnframes()); w.close(); return d
sys.exit(0 if pcm(sys.argv[1]) == pcm(sys.argv[2]) else 1)
PY
}

fail=0
b_drop=$(ctr evq_drops); b_late=$(ctr evt_late); b_fto=$(ctr fence_timeouts)

# --- scenario 1: determinism — state A rendered twice (save the project on the
#     first render so scenario 2 can reopen it) ---
echo "[reaper-e2e] determinism: render state A twice ..."
pin 0.5; render a1 build "$OUTDIR/recall.rpp"
pin 0.5; render a2 build
if pcm_eq "$OUTDIR/a1.wav" "$OUTDIR/a2.wav"; then
    echo "PASS determinism: two REAPER renders are byte-identical through the device"
else
    echo "FAIL determinism: REAPER render PCM differs"; fail=1
fi

# --- scenario 2: recall round-trip — scramble the device, reopen the project ---
echo "[reaper-e2e] recall: scramble device to state B, render, then reopen project ..."
pin 0.2; render scrambled build       # fresh project adopts the scrambled state B
if pcm_eq "$OUTDIR/scrambled.wav" "$OUTDIR/a1.wav"; then
    echo "FAIL recall(setup): scrambled state B renders identical to saved state A — the scramble had no audible effect, test would be vacuous"; fail=1
else
    echo "  scramble confirmed: state B differs from saved state A"
fi
# device is still in state B; opening the saved project must recall A onto it
render recalled open "$OUTDIR/recall.rpp"
if pcm_eq "$OUTDIR/recalled.wav" "$OUTDIR/a1.wav"; then
    echo "PASS recall: reopening the project re-pushed the saved state through REAPER (recalled == saved, despite a scrambled device)"
else
    echo "FAIL recall: reopened render != saved — the Recall Bundle was not re-asserted on load"; fail=1
fi

a_drop=$(ctr evq_drops); a_late=$(ctr evt_late); a_fto=$(ctr fence_timeouts)
for pair in "evq_drops $b_drop $a_drop" "evt_late $b_late $a_late" "fence_timeouts $b_fto $a_fto"; do
    set -- $pair
    d=$(( ${3:-0} - ${2:-0} ))
    echo "  $1 delta=$d"
    [ "$d" -ne 0 ] && { echo "FAIL: $1 rose by $d"; fail=1; }
done

[ "$fail" -eq 0 ] && echo "PASS: REAPER e2e — real-DAW determinism + recall round-trip, lossless" || exit 1
