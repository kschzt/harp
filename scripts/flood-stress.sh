#!/bin/sh
# IDM-flood stress + determinism gate (§8.3 audio.deterministic, §9 event plane).
#
# Pins the device to a known parameter state via the front panel (which bypasses
# the shell's automation thinning, unlike in-band param sets), then hammers the
# event plane through the VST3 plugin with dense automation (8 params under LFOs
# on 64-sample blocks), 20 notes/s, and a per-beat loop wrap. Asserts:
#   1. two such floods are BYTE-IDENTICAL — determinism holds under the hammer;
#   2. the device dropped/delayed nothing — zero-tolerance counters (§14.2)
#      do not move across the flood (ramp_late is budgeted, not checked).
# Run on a host with the device attached (the hardware runner).
#
# Env overrides: HARP_VST3_HOST, HARP_SHELL, HARP_PROBE, HARP_FLOOD_SECONDS.
set -e

HOST=${HARP_VST3_HOST:-./build-vst/harp-vst3-host}
PLUGIN=${HARP_SHELL:-$HOME/.vst3/harp-shell.vst3}
PROBE=${HARP_PROBE:-./build/harp-probe}
# A full minute of continuous hammering per run (×2 runs) — long enough that
# any drift, leak, or counter creep under the IDM load has time to surface;
# 4 s proved nothing. Duration is --seconds (the pattern loops); block size
# stays 64 (that is the per-second automation density, not the length).
SECS=${HARP_FLOOD_SECONDS:-60}

# Pin a known start state on the device (front-panel sets persist past the
# session and are not thinned). Params 1-8 to mid; arp off so the flood's
# determinism doesn't depend on transport-anchored arp timing.
pin_state() {
    for i in 1 2 3 4 5 6 7 8; do "$PROBE" -d usb knob "$i" 0.5 >/dev/null 2>&1; done
    "$PROBE" -d usb knob 9 0.0 >/dev/null 2>&1
}
flood_hash() {
    "$HOST" "$PLUGIN" --flood --seconds "$SECS" --hash 2>/dev/null | sed -n 's/^output-hash: //p'
}
ctr() { "$PROBE" -d usb counters 2>/dev/null | grep -E "[. ]$1 = " | grep -oE '[0-9]+$' | tail -1; }

fail=0
for k in evq_drops evt_late fence_timeouts frame_errors; do eval "before_$k=$(ctr "$k")"; done

echo "[flood] pinning device state + hammering, run 1/2 ..."
pin_state; H1=$(flood_hash)
echo "[flood] pinning device state + hammering, run 2/2 ..."
pin_state; H2=$(flood_hash)
echo "  hash1=$H1"
echo "  hash2=$H2"

if [ -n "$H1" ] && [ "$H1" = "$H2" ]; then
    echo "PASS determinism: two IDM-flood renders are byte-identical ($H1)"
else
    echo "FAIL determinism: $H1 != $H2"
    fail=1
fi

sleep 1
echo "[flood] zero-tolerance counter deltas over the flood:"
for k in evq_drops evt_late fence_timeouts frame_errors; do
    eval "b=\$before_$k"
    a=$(ctr "$k")
    d=$(( ${a:-0} - ${b:-0} ))
    echo "  $k: delta $d"
    if [ "$d" -ne 0 ]; then echo "FAIL: $k rose by $d under flood"; fail=1; fi
done

if [ "$fail" -eq 0 ]; then
    echo "PASS: event plane byte-deterministic and lossless under the IDM flood."
else
    exit 1
fi
