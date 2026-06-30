#!/bin/sh
# multiout-iso-test — M1 per-part isolation for the Kontakt/Overbridge-style multi-out main
# instance over §8.7 free-running RTP (the wide-union bounded-packet path, >8 slots).
#
# ONE main VST3 instance exposes 17 output buses: bus 0 = summed main mix, buses 1..16 =
# the per-part stereo pairs (slots {2+2k, 3+2k}). Notes on MIDI channel C route to device
# part C (§P2.1) and surface on bus C+1. The proof, per RME ("zero bus bleed"):
#   1. notes on channel C  -> bus C+1 (part C) SOUNDS  (rms > FLOOR)
#   2. same notes          -> bus D+1 (part D != C) is EXACTLY SILENT (rms == 0)  [no bleed]
#   3. same notes          -> bus 0 (main mix) SOUNDS  (>= the single part: it is the sum)
# Run with --out-buses 17 so the device streams the full 34-slot union (multi-packet RTP).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17940}"
FLOOR="${FLOOR:-0.005}"   # a clearly-sounding bus (observed ~0.034)
fail() { echo "MULTIOUT-ISO FAIL: $1"; [ -n "${DP:-}" ] && kill "$DP" 2>/dev/null; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

# rms of one render: capture_bus <channel> <capture-bus>
cap() {
    HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 20; exec @ARGV' \
        "$HOSTBIN" "$PLUG" --realtime --out-buses 17 --channel "$1" --capture-bus "$2" \
        --notes 62,69,74,65 --seconds 1.5 2>/dev/null | grep -oE "rms=[0-9.]+" | tail -1 | cut -d= -f2
}
gt() { awk "BEGIN{exit !($1 > $2)}"; }

echo "── multiout-iso: 17-bus main over §8.7 RTP (34-slot union, multi-packet) on 127.0.0.1:$PORT"
# Test two channels so a stuck mapping can't pass by luck.
for C in 3 7; do
    rm -rf multiout-iso-state; : > /tmp/multiout-iso-dev.log
    "$DEVICED" --port "$PORT" --serial SIM-MO-ISO --state-dir multiout-iso-state >/tmp/multiout-iso-dev.log 2>&1 & DP=$!
    trap 'kill "$DP" 2>/dev/null; rm -rf multiout-iso-state' EXIT INT TERM
    for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/multiout-iso-dev.log 2>/dev/null && break; sleep 0.2; done
    grep -q "listening on $PORT" /tmp/multiout-iso-dev.log || fail "device didn't start on $PORT"

    part_bus=$((C + 1))
    other=$((C == 3 ? 7 : 3)); other_bus=$((other + 1))   # a part with NO notes
    r_part=$(cap "$C" "$part_bus")
    r_other=$(cap "$C" "$other_bus")
    r_main=$(cap "$C" 0)
    echo "  channel $C: part-bus $part_bus rms=${r_part:-?}  silent-bus $other_bus rms=${r_other:-?}  main rms=${r_main:-?}"

    gt "${r_part:-0}" "$FLOOR"  || fail "channel $C -> part bus $part_bus is SILENT (rms=${r_part:-?}); routing/demux broken"
    # the unplayed part's bus must be EXACTLY zero — any bleed is a routing/demux bug
    [ "${r_other:-1}" = "0.00000" ] || fail "BLEED: channel $C leaked into part bus $other_bus (rms=${r_other:-?}); buses not isolated"
    gt "${r_main:-0}" "$FLOOR"  || fail "channel $C -> main mix (bus 0) is SILENT (rms=${r_main:-?})"

    kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; rm -rf multiout-iso-state; DP=
done

# ── OFFLINE (host-paced TCP) wide-union determinism. This is the SAME host-paced render path
# USB uses, so it guards the multi-out hang fix (the per-part sinks must fill from the initial
# union — register-before-start + no bogus epoch clear). Render part 3's bus twice offline and
# require a byte-identical hash (offline is deterministic; a hang or a dropped sink would differ).
rm -rf multiout-iso-state; : > /tmp/multiout-iso-dev.log
"$DEVICED" --port "$PORT" --serial SIM-MO-ISO --state-dir multiout-iso-state >/tmp/multiout-iso-dev.log 2>&1 & DP=$!
trap 'kill "$DP" 2>/dev/null; rm -rf multiout-iso-state' EXIT INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/multiout-iso-dev.log 2>/dev/null && break; sleep 0.2; done
ohash() { HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' \
    "$HOSTBIN" "$PLUG" --out-buses 17 --channel 3 --capture-bus 4 --notes 62,69,74,65 \
    --seconds 1.5 --hash 2>/dev/null | grep -oE "output-hash: [0-9a-f]+" | tail -1 | cut -d' ' -f2; }
oh1=$(ohash); oh2=$(ohash)
echo "  offline part-3 bus hash #1=${oh1:-HUNG} #2=${oh2:-HUNG}"
[ -n "$oh1" ] || fail "OFFLINE wide-union multi-out HUNG (no audio / host-paced pacing stall)"
[ "$oh1" = "$oh2" ] || fail "OFFLINE wide-union multi-out non-deterministic (#1=$oh1 #2=$oh2)"
kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; rm -rf multiout-iso-state; DP=

echo "MULTIOUT-ISO PASS (free-running zero-bleed isolation + offline host-paced determinism over the 34-slot union)"
