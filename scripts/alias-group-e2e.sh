#!/bin/sh
# alias-group-e2e — P6, the E2E multitimbral proof through the REAL VST3 PLUGIN
# CHAIN (not the runtime directly): a multi-out MAIN instance summing N parts.
#
# tsan-host proves the same fact at the RUNTIME layer (one owner, N part sinks,
# RMS-separated). This proves it one layer up — through the full VST3 plugin: real
# component/processor, setActive, process() — using the production host
# harp-vst3-host in its multi-channel mode (tools/vst3-host/main.cpp, --instances/
# --aliases). ONE plugin instance is created from the module factory; the host
# drives N MIDI channels into it (a note on channel C routes to device part C,
# §9.4 key 5), and the instance's bus 0 (the main mix) SUMS every engaged part.
# This is exactly what a DAW does feeding one HARP main track on channels 0..N-1
# (the per-channel MIDI routing is the DAW's job) — no registry, no sharing, no
# per-instance Part parameter: a single claimer, routing by note channel alone.
#
# THE SIGNAL — the bus-0 MAIN-MIX content hash, the golden/multitimbral oracle.
#   This renders OFFLINE through harp-vst3-host — the byte-deterministic path
#   golden-test.sh and multitimbral-test.sh both hash against the real device. So
#   we use the same '--hash' -> 'output-hash: <hex>' extractor: identical hash ==
#   identical audio. The proof is a DIFFERENCE of hashes:
#     - single  = --instances 1: channel 0 only -> part 0; parts 1..N-1 silent.
#     - group   = --instances N: channels 0..N-1 each struck a DISTINCT note
#                 (main.cpp transposes each channel's note by its channel), so the
#                 device turns parts 1..N-1 active and SUMS them into the main mix.
#   A single part can NEVER produce the group's summed multi-part mix, so a group
#   hash that DIFFERS from the single-channel hash means the sibling parts engaged
#   — through the full plugin chain. (Same logic as multitimbral-test.sh's A!=Z: a
#   changed hash == the note/part reached the mix.)
#
# Mirrors golden-test.sh / multitimbral-test.sh house conventions: build-vst host,
# the $PLUG bundle (Linux CI overrides -> ~/.vst3), pinned HARP_DEVICE_SERIAL, the
# Live claim guard, and harp-probe to confirm the board is on the bus. Needs ONE
# board on the bus and Live closed.
# Exit 0 pass / 2 N/A (board absent / host not built) / 3 device busy.
set -u
cd "$(dirname "$0")/.."

# Pin one board on a multi-board bus, exactly like the other single-device tests
# (the desk unit with the web panel); overridable for CI's closet rig.
export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
SERIAL="$HARP_DEVICE_SERIAL"

V="${HOSTBIN:-build-vst/harp-vst3-host}"
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"  # Linux CI overrides -> ~/.vst3
PROBE="${PROBE:-./build/harp-probe}"
INSTANCES="${INSTANCES:-4}"   # owner part 0 + 3 siblings parts 1..3 — a real group
NOTE="${NOTE:-60}"            # base note; each alias is transposed by its channel
DUR="${DUR:-2.0}"            # seconds per render — long enough for each note's envelope

# The device is exclusive: a DAW holding the claim makes every render come back as
# silence, so single==group (a bogus pass). Guard up front (mirror golden-test.sh)
# and treat busy as a hard FAIL, never a silent skip.
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "ALIAS-GROUP-E2E FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

# need the pinned board on the bus; without it the owner never connects and the
# group is unobservable — legitimately N/A on this rig.
if [ -x "$PROBE" ]; then
    if ! "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL"; then
        echo "ALIAS-GROUP-E2E SKIP: board $SERIAL not on the bus"
        exit 2
    fi
else
    echo "ALIAS-GROUP-E2E SKIP: $PROBE not built (need it to confirm the board is present)"
    exit 2
fi
[ -x "$V" ] || { echo "ALIAS-GROUP-E2E SKIP: host $V not built"; exit 2; }

echo "── alias-group-e2e: one main instance on $SERIAL driven on $INSTANCES channels — expect a multi-part mix"

# SETTLE once into a known, AUDIBLE, deterministic voice through the plugin
# (mirrors multitimbral-test.sh): audible Master Level (id 7, the 2.1.0 map), a chosen tone (3), and a FAST
# envelope (attack 5, release 6) so each struck note's full attack+decay lands
# inside the capture and the hash is stable run-to-run. The drone is gone, so the
# struck notes are the only sound — without an audible patch the mix would be
# silent and single==group trivially. Single-channel render, so it warms part 0
# cleanly before the group run.
SETTLE="--set 7=0.6 --set 3=0.7 --set 5=0.05 --set 6=0.1"
$V "$PLUG" $SETTLE --seconds 0.5 >/dev/null 2>&1 \
    || { echo "ALIAS-GROUP-E2E FAIL: settle render did not complete (device busy/absent?)"; exit 3; }

# hash ARGS... — run the host through the real plugin with the given args (+ --hash)
# and echo the bus-0 main-mix content hash. Mirrors golden-test.sh's extractor:
# 'output-hash: <hex>'. The multi-channel path prints the same line for the one
# instance's captured bus-0 main mix (tools/vst3-host/main.cpp run_multi_instance).
hash() {
    $V "$PLUG" "$@" --hash 2>/dev/null | grep output-hash | cut -d' ' -f2
}

# ---- baseline: ONE instance, channel 0 only -> part 0 (parts 1..15 silent) ----
echo "── single-channel baseline (--instances 1, channel 0 -> part 0)"
SINGLE=$(hash --instances 1 --notes "$NOTE" --seconds "$DUR")
echo "   single main-mix hash: ${SINGLE:-<none>}"

# ---- the group: N channels into one instance, parts 0..N-1 each a distinct note ----
echo "── alias group (--instances $INSTANCES, channels 0..$((INSTANCES - 1)) each a distinct note)"
GROUP=$(hash --instances "$INSTANCES" --notes "$NOTE" --seconds "$DUR")
echo "   group main-mix hash:  ${GROUP:-<none>}"

# A missing hash means the host never rendered/connected — the device was busy or
# absent, not a clean comparison. Hard FAIL (busy), never a vacuous pass.
if [ -z "$SINGLE" ] || [ -z "$GROUP" ]; then
    echo "ALIAS-GROUP-E2E FAIL: host produced no main-mix hash (device busy/absent?)"
    exit 3
fi

# ---- verdict: the group mix must DIFFER from the single-channel mix ----
# A single part can never produce the group's summed multi-part mix, so a group
# hash that differs from the single-channel hash means the device summed sibling
# parts into its bus-0 main mix — MORE THAN ONE part engaged, through the full
# plugin chain. (multitimbral-test.sh's A!=Z, at the e2e layer.)
if [ "$SINGLE" != "$GROUP" ]; then
    echo "ALIAS-GROUP-E2E PASS (one main instance on $SERIAL driven on $INSTANCES channels; the"
    echo "   bus-0 main mix ($GROUP) differs from the single-channel run ($SINGLE) — sibling"
    echo "   parts engaged through the full VST3 plugin chain)"
    exit 0
else
    echo "ALIAS-GROUP-E2E FAIL: group main-mix hash ($GROUP) equals the single-channel hash"
    echo "   ($SINGLE) — only one part appears engaged (siblings not reaching the device?)"
    exit 1
fi
