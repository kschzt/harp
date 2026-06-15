#!/bin/sh
# IDM-flood stress test (§9 event plane under abuse).
#
# Hammers the shell's event plane through the VST3 plugin with tight automation
# (8 params under dense LFOs on 64-sample blocks), 20 notes/s, and a per-beat
# loop wrap, then asserts the DEVICE dropped or delayed nothing during the
# flood — the zero-tolerance counters (§14.2) must not move. Run on a host with
# the device attached (the hardware runner). Byte-exact DETERMINISM is a
# separate property, covered in-session by t15 (recall round-trip) and T17 (arp
# groove); a cross-process flood can't assert it because params persist as
# recall state and automation interpolates from the current value.
#
# Env overrides: HARP_VST3_HOST, HARP_SHELL, HARP_PROBE, HARP_FLOOD_SECONDS.
set -e

HOST=${HARP_VST3_HOST:-./build-vst/harp-vst3-host}
PLUGIN=${HARP_SHELL:-$HOME/.vst3/harp-shell.vst3}
PROBE=${HARP_PROBE:-./build/harp-probe}
SECS=${HARP_FLOOD_SECONDS:-4}

# device counters are cumulative since the daemon started — measure the DELTA
# across the flood so a previous step's events can't cause a false failure.
ctr() { "$PROBE" -d usb counters 2>/dev/null | grep -E "[. ]$1 = " | grep -oE '[0-9]+$' | tail -1; }

fail=0
echo "[flood] baseline counters..."
for k in evq_drops evt_late fence_timeouts frame_errors; do
    eval "before_$k=$(ctr "$k")"
done

echo "[flood] hammering the event plane for ${SECS}s (dense automation + notes + loop wraps)..."
"$HOST" "$PLUGIN" --flood --reset --seconds "$SECS" >/dev/null
sleep 1 # let the device flush the session before reading counters

echo "[flood] delta over the flood (each must be 0; ramp_late is budgeted, not checked):"
for k in evq_drops evt_late fence_timeouts frame_errors; do
    eval "b=\$before_$k"
    a=$(ctr "$k")
    d=$(( ${a:-0} - ${b:-0} ))
    echo "  $k: $b -> $a  (delta $d)"
    if [ "$d" -ne 0 ]; then
        echo "FAIL: $k rose by $d under flood (expected 0)"
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "PASS: event plane survived the IDM flood with zero drops/late/timeouts."
else
    exit 1
fi
