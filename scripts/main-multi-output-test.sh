#!/bin/sh
# main-multi-output-test (M5) — the whole-device multi-out LAYOUT, host-side.
#
# The multi-out main is ONE plugin that owns the whole device and declares 17
# stereo output buses: bus 0 = the summed Main Mix, buses 1..16 = the 16 per-part
# stereo pairs (a DAW routes the ones it uses). This asserts that declaration
# straight from the plugin via harp-vst3-host --list — no device, no render, fully
# deterministic and platform-independent (the bus declaration is the same on VST3
# everywhere):
#   - exactly 17 output buses (getBusCount(kAudio, kOutput) == 17)
#   - bus 0 named "Main Mix", stereo (2 ch)
#   - buses 1..16 named "Part N", stereo (2 ch)
# This is the structural contract the per-part audio routing + the whole-device
# recall ride on, so it must hold on every platform.
# Exit 0 pass / 1 fail / 2 N/A (host or plugin not built).
set -u
cd "$(dirname "$0")/.."

V="${HOSTBIN:-build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
[ -z "$PLUG" ] && PLUG="$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3"
[ -x "$V" ] || { echo "MAIN-MULTI-OUT SKIP: host $V not built"; exit 2; }
[ -e "$PLUG" ] || { echo "MAIN-MULTI-OUT SKIP: plugin $PLUG not built"; exit 2; }

echo "── main-multi-output: assert the 17-bus whole-device layout"
echo "   plugin: $PLUG"
OUT=$("$V" "$PLUG" --list 2>/dev/null)

# exactly 17 output buses
N=$(echo "$OUT" | sed -n 's/^output-buses (\([0-9]*\)).*/\1/p')
[ "$N" = 17 ] || {
    echo "MAIN-MULTI-OUT FAIL: declared ${N:-<none>} output buses, want 17 (stale single-out plugin?)"
    echo "$OUT" | grep -iE "output-buses|out-bus" | head
    exit 1
}
echo "   17 output buses declared ✓"

# bus 0 = "Main Mix", stereo
echo "$OUT" | grep 'out-bus\[0\]' | grep -q '"Main Mix" channels=2' || {
    echo "MAIN-MULTI-OUT FAIL: bus 0 is not 'Main Mix' stereo:"
    echo "$OUT" | grep 'out-bus\[0\]'
    exit 1
}
echo "   bus 0 = \"Main Mix\" stereo ✓"

# buses 1..16 = "Part N", stereo
fail=0; k=1
while [ "$k" -le 16 ]; do
    echo "$OUT" | grep "out-bus\[$k\]" | grep -q "\"Part $k\" channels=2" || {
        echo "MAIN-MULTI-OUT FAIL: bus $k is not 'Part $k' stereo: $(echo "$OUT" | grep "out-bus\[$k\]")"
        fail=1
    }
    k=$((k + 1))
done
[ "$fail" = 0 ] || exit 1
echo "   buses 1..16 = \"Part 1\"..\"Part 16\" stereo ✓"

# defence-in-depth: NO output bus is mono/odd (every declared bus is stereo)
NON2=$(echo "$OUT" | grep 'out-bus\[' | grep -vc 'channels=2')
[ "$NON2" = 0 ] || { echo "MAIN-MULTI-OUT FAIL: $NON2 output bus(es) are not stereo"; exit 1; }

echo "MAIN-MULTI-OUT PASS (17 stereo buses: Main Mix + Part 1..16 — the whole-device multi-out layout)"
