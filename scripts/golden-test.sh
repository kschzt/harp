#!/bin/sh
# golden-test — THE oracle, as a repo-tracked test instead of a /tmp
# ritual. Renders the canonical sequences through BOTH shells against
# the real device and asserts the known-good hashes:
#
#   golden render (notes, arp off)  — must match across VST3 and AU
#   arp groove (16ths @ 120 BPM)    — must match across VST3 and AU
#   chord (3 notes held together)   — must match across VST3 and AU
#
# These hashes change ONLY when behavior is intentionally changed (e.g.
# reported latency moved); re-capture deliberately, never casually, and
# say why in the commit. History:
#   76470294cdee6393  golden, cushion-5 era (sync transport)
#   65770cc8ddcbd7c2  golden, MONO engine (one voice; overlapping notes stole it)
#   787947297e858ec3  golden, VOICE POOL (NVOICES=8; overlapping note-tails ring on
#                     their own voices instead of cutting off — the §9.5 poly change)
#   3b5a16e234168457  groove, arp era — HELD byte-identical across the voice-pool
#                     change: the arp is monophonic on voice 0, so its path is the
#                     control proving the core DSP is untouched (only note-overlap moved)
#   eaed2355f68db8ed  chord, polyphony oracle (added with the voice pool): 60,64,67
#                     sounded TOGETHER. A mono steal would render only the last note
#                     (≈ single-note energy); the pool sums 3 voices (~1.6x rms)
set -e
cd "$(dirname "$0")/.."

GOLDEN_HASH="787947297e858ec3"  # re-baselined: drone removed (engine 2.0.0)
GROOVE_HASH="3b5a16e234168457"  # re-baselined: drone removed (engine 2.0.0)
CHORD_HASH="eaed2355f68db8ed"   # re-baselined: drone removed

V=build-vst/harp-vst3-host
A=build-vst/au-host
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"  # Linux CI overrides -> ~/.vst3
AUCOMP="$HOME/Library/Audio/Plug-Ins/Components/harp-au.component"

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "GOLDEN FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

SETTLE="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
        --set 8=0.6 --set 9=0 --set 10=0.6 --set 11=0.5 --set 12=0 --set 13=0"
ARP="--set 9=0.25 --set 10=0.6 --set 11=0.5 --set 12=0.0 --set 8=0.7 \
     --set 5=0.05 --set 6=0.1 --set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 13=0"

check() { # check NAME EXPECTED CMD...
    name=$1; want=$2; shift 2
    got=$("$@" 2>/dev/null | grep output-hash | cut -d' ' -f2)
    if [ "$got" != "$want" ]; then
        echo "GOLDEN FAIL: $name = $got (want $want)"
        exit 1
    fi
    echo "   $name: $got"
}

echo "── golden: canonical renders vs known-good hashes (both shells)"
$V "$PLUG" $SETTLE --seconds 0.5 >/dev/null 2>&1
check "vst3 golden" "$GOLDEN_HASH" $V "$PLUG" $SETTLE --notes 62,69,74,65 --seconds 2.6 --hash
$V "$PLUG" $ARP --seconds 0.5 >/dev/null 2>&1
check "vst3 groove" "$GROOVE_HASH" $V "$PLUG" $ARP --bpm 120 --chord 60,64,67 --seconds 4 --hash
# arp off (SETTLE has param 9=0), so --chord holds 60,64,67 simultaneously: polyphony
$V "$PLUG" $SETTLE --seconds 0.5 >/dev/null 2>&1
check "vst3 chord " "$CHORD_HASH" $V "$PLUG" $SETTLE --chord 60,64,67 --seconds 2.0 --hash

if [ -x "$A" ] && [ -d "$AUCOMP" ]; then
    $A $SETTLE --seconds 0.5 >/dev/null 2>&1
    check "au golden  " "$GOLDEN_HASH" $A $SETTLE --notes 62,69,74,65 --seconds 2.6 --hash
    $A $ARP --seconds 0.5 >/dev/null 2>&1
    check "au groove  " "$GROOVE_HASH" $A $ARP --bpm 120 --chord 60,64,67 --seconds 4 --hash
    $A $SETTLE --seconds 0.5 >/dev/null 2>&1
    check "au chord   " "$CHORD_HASH" $A $SETTLE --chord 60,64,67 --seconds 2.0 --hash
else
    echo "   (AU shell not installed; VST3-only run)"
fi
echo "GOLDEN PASS (the oracle is law)"
