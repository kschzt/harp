#!/bin/sh
# Recall round-trip through the real device + the VST3 shell, no DAW required.
# Proves the core HARP promise — reopening a project restores the hardware, even
# if someone has since changed it on the box:
#
#   1. pin the device to state A, render + SAVE the Recall Bundle (getState)
#   2. SCRAMBLE the device to a different state B, render it
#   3. LOAD the saved bundle (setState) onto the still-scrambled device, render
#
# Asserts the recalled render equals A and the scrambled render differs from A
# (so the test can't pass vacuously). The host (harp-vst3-host) saves while the
# plugin is still active — a DAW's live save — which is the path REAPER's
# offline render can't take (it deactivates before writing the project), so this
# complements the REAPER determinism e2e rather than duplicating it.
#
# Run on a host with the device attached (the hardware runner).
# Env overrides: HARP_VST3_HOST, HARP_SHELL, HARP_PROBE.
set -e

HOST=${HARP_VST3_HOST:-./build-vst/harp-vst3-host}
PLUGIN=${HARP_SHELL:-$HOME/.vst3/harp-shell.vst3}
PROBE=${HARP_PROBE:-./build/harp-probe}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM

[ -x "$HOST" ] || { echo "FAIL: harp-vst3-host not built at $HOST"; exit 1; }
[ -e "$PLUGIN" ] || { echo "FAIL: shell not installed at $PLUGIN"; exit 1; }

# pin the device to a known state via the front panel (knobs 1-6,8 = $1; id 7
# "Drone Mix" was removed with the drone — knob 7 errors and trips set -e; arp off
# so determinism doesn't depend on transport-anchored timing).
pin() {
    for i in 1 2 3 4 5 6 8; do "$PROBE" -d usb knob "$i" "$1" >/dev/null 2>&1; done
    "$PROBE" -d usb knob 9 0.0 >/dev/null 2>&1
}
ohash() { "$HOST" "$PLUGIN" "$@" 2>/dev/null | sed -n 's/^output-hash: //p'; }

echo "[recall] pin state A, render + save the Recall Bundle ..."
pin 0.5
HA=$(ohash --seconds 2 --hash --save-state "$T/A.bundle")
[ -s "$T/A.bundle" ] || { echo "FAIL: getState saved no bundle"; exit 1; }

echo "[recall] scramble the device to state B, render ..."
pin 0.2
HB=$(ohash --seconds 2 --hash)

echo "[recall] load the saved bundle onto the scrambled device, render ..."
HR=$(ohash --load-state "$T/A.bundle" --seconds 2 --hash)

echo "  saved A=$HA  scrambled B=$HB  recalled=$HR"
[ -n "$HA" ] || { echo "FAIL: no render hash for A"; exit 1; }
if [ "$HA" = "$HB" ]; then
    echo "FAIL recall(setup): scramble had no audible effect (A==B) — test would be vacuous"; exit 1
fi
if [ "$HA" != "$HR" ]; then
    echo "FAIL recall: recalled ($HR) != saved ($HA) — the bundle was not re-asserted onto the device"; exit 1
fi
echo "PASS recall: the saved state was restored onto a scrambled device (recalled == saved, scrambled differs)"
