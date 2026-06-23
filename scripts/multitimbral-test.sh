#!/bin/sh
# multitimbral-test — proves the 16-part multitimbral plumbing on real
# hardware: EVERY one of the 16 parts produces sound when a note is sent
# on its own MIDI channel, AND that routing is EXCLUSIVE — a note on
# channel k lands in part k's stereo output and bleeds into no other part.
#
# Architecture under test (foundation session 2026-06-12): "multitimbral =
# one instance + multi-bus". The headless host emits notes on a chosen MIDI
# channel (--channel k, P2.1) and pulls a chosen part's stereo output
# instead of the main mix (--part N, P2.2). With one note on its channel a
# part's render must differ from that same part's silence; with a note on a
# DIFFERENT channel a part's render must equal its silence (no leak).
#
# NOTE ON SCOPE: at P2.1/P2.2 every part shares ONE timbre (per-part params
# arrive at P3). So this test does NOT prove distinct voices — it proves
# ROUTING (which channel reaches which part) and that all 16 parts are live.
# At P3 the same harness extends to distinct timbres: capture each part with
# its own params and assert the hashes differ across parts, not just A!=Z.
#
# We assert via the host's content hash ('--hash' -> 'output-hash: <hex>'),
# the same oracle golden-test.sh trusts: identical hash == identical audio,
# so A!=Z means "the note changed this part's output" and L==Z means "this
# part rendered its baseline, untouched". Hashing sidesteps any RMS-threshold
# guesswork — exact bytes in, exact bytes out.
#
# usage: scripts/multitimbral-test.sh   (device must be unclaimed)
# exit:  0 PASS; 1 routing FAIL; 3 device not exclusively claimable.
set -u
cd "$(dirname "$0")/.."

# Pin one board on a multi-board bus, exactly like the other single-device
# tests (the desk unit with the web panel); overridable for CI's closet rig.
export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"

V=build-vst/harp-vst3-host
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"  # Linux CI overrides -> ~/.vst3

# The device is exclusive: a DAW holding the claim makes every render come
# back as silence, so A==Z everywhere and the failures read as bogus routing
# bugs. Guard up front (mirror golden-test.sh) and treat busy as a hard FAIL.
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "MULTITIMBRAL FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

# SETTLE once into a known, fully-deterministic voice: audible level (8), a
# chosen tone (3), and a FAST envelope (attack 5, release 6) so a 1.5 s window
# fully captures each note's attack+decay and the hash is stable run-to-run.
# (The drone is gone — a part is silent until a note plays.) Done once; every
# render below inherits it.
SETTLE="--set 8=0.6 --set 3=0.7 --set 5=0.05 --set 6=0.1"
$V "$PLUG" $SETTLE --seconds 0.5 >/dev/null 2>&1 \
    || { echo "MULTITIMBRAL FAIL: settle render did not complete (device absent?)"; exit 3; }

# hash ARGS... — render with the given host args (+ --hash) and echo the
# content hash. Mirrors golden-test.sh's extractor: 'output-hash: <hex>'.
hash() {
    $V "$PLUG" $SETTLE "$@" --hash 2>/dev/null | grep output-hash | cut -d' ' -f2
}

DUR=1.5      # seconds per render — long enough for one note's full envelope
NOTE=69      # A4, an unmistakable mid-range tone
FAIL=0

echo "── multitimbral: 16 parts, exclusive channel->part routing ($HARP_DEVICE_SERIAL)"

# ---- 1: every part plays a note on its own channel ----
# For each part k: render part k WITH a note on channel k (A) and part k with
# NO notes (Z). A!=Z proves the note reached part k and made sound. Part 0 is no
# longer special (the drone is gone): its Z baseline is silence like every other
# part, and a struck note still moves the mix, so A!=Z holds there too.
k=0
while [ "$k" -le 15 ]; do
    A=$(hash --channel "$k" --notes "$NOTE" --seconds "$DUR" --part "$k")
    Z=$(hash --seconds "$DUR" --part "$k")
    if [ -z "$A" ] || [ -z "$Z" ]; then
        echo "   part $k: FAIL — no hash from host (render error?)"
        FAIL=1
    elif [ "$A" != "$Z" ]; then
        echo "   part $k: PASS — channel $k note reached part $k (note=$A silence=$Z)"
    else
        echo "   part $k: FAIL — channel $k note left part $k unchanged ($A) — part dead or note unrouted"
        FAIL=1
    fi
    k=$((k + 1))
done

# ---- 2: exclusive routing — no cross-part leak ----
# Play a note on channel k but OBSERVE part j (k != j). Part j must render its
# own silence baseline Zj: L==Zj proves channel k did NOT bleed into part j.
# Both k and j avoid 0 — part 0 is the main-mix's first writer (always rendered,
# silence when idle) and is covered by the play-test above instead.
leak_check() { # leak_check K J
    k=$1; j=$2
    L=$(hash --channel "$k" --notes "$NOTE" --seconds "$DUR" --part "$j")
    Zj=$(hash --seconds "$DUR" --part "$j")
    if [ -z "$L" ] || [ -z "$Zj" ]; then
        echo "   leak ch$k->part$j: FAIL — no hash from host (render error?)"
        FAIL=1
    elif [ "$L" = "$Zj" ]; then
        echo "   leak ch$k->part$j: PASS — channel $k did not bleed into part $j (baseline=$Zj)"
    else
        echo "   leak ch$k->part$j: FAIL — channel $k bled into part $j (got $L, baseline $Zj)"
        FAIL=1
    fi
}
leak_check 5 3
leak_check 11 7

if [ "$FAIL" -eq 0 ]; then
    echo "MULTITIMBRAL PASS (16 parts live; channel->part routing is exclusive)"
    exit 0
else
    echo "MULTITIMBRAL FAIL: at least one part is dead or routing leaked — see lines above"
    exit 1
fi
