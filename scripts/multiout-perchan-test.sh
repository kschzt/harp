#!/bin/sh
# multiout-perchan-test — M2 PER-CHANNEL PARAMS for the multi-out main (Kontakt-style).
# A single main instance drives EVERY part's params per-event: a satellite track's MIDI CC on
# channel N edits part N's device param (host IMidiMapping -> a synthetic id the processor
# decodes and queues with §9.4 key 5 = N). vst3-host's --set-ch CH:ID=V exercises the SAME
# synthetic-id path the DAW's MIDI CC arrives on.
#
# Proof (deterministic offline hashes; notes on channel 3 -> part 3, capture bus 4 = part 3):
#   1. --set-ch 3:7 (part 3's Master Level) CHANGES bus 4's hash  (the param routed to part 3)
#   2. --set-ch 7:7 (part 7's level) LEAVES bus 4's hash == ref    (isolated — part 3 untouched)
# Fresh device per render (so part 3 starts at its default), synced on "listening on PORT".
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17948}"
fail() { echo "MULTIOUT-PERCHAN FAIL: $1"; [ -n "${DP:-}" ] && kill "$DP" 2>/dev/null; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

# offline part-3-bus hash with an optional --set-ch; FRESH device, synced on the listen log.
oh() {
    rm -rf multiout-perchan-state; : > /tmp/multiout-perchan-dev.log
    "$DEVICED" --port "$PORT" --serial SIM-MO-PC --state-dir multiout-perchan-state \
        >/tmp/multiout-perchan-dev.log 2>&1 & DP=$!
    trap 'kill "$DP" 2>/dev/null; rm -rf multiout-perchan-state' EXIT INT TERM
    i=0; while [ $i -lt 30 ]; do grep -q "listening on $PORT" /tmp/multiout-perchan-dev.log 2>/dev/null && break; sleep 0.2; i=$((i+1)); done
    grep -q "listening on $PORT" /tmp/multiout-perchan-dev.log || fail "device didn't start on $PORT"
    set_arg=""; [ -n "$1" ] && set_arg="--set-ch $1"
    h=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' \
        "$HOSTBIN" "$PLUG" --out-buses 17 --channel 3 --capture-bus 4 $set_arg \
        --notes 62,69,74,65 --seconds 1.2 --hash 2>/dev/null | grep -oE "output-hash: [0-9a-f]+" | cut -d' ' -f2)
    kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; rm -rf multiout-perchan-state; DP=
    echo "$h"
}

echo "── multiout-perchan: one main instance, per-channel param routing on 127.0.0.1:$PORT"
ref=$(oh "");          [ -n "$ref" ]   || fail "ref render produced no audio (HUNG?)"
p3=$(oh "3:7=0.05");   [ -n "$p3" ]    || fail "part-3 render produced no audio (HUNG?)"
p7=$(oh "7:7=0.05");   [ -n "$p7" ]    || fail "part-7 render produced no audio (HUNG?)"
echo "  bus4(part3) hashes: ref=$ref  set-part3=$p3  set-part7=$p7"

[ "$p3" != "$ref" ] || fail "part-3 param did NOT change bus 4 ($p3 == ref) — channel routing broken"
[ "$p7" = "$ref" ]  || fail "part-7 param LEAKED into bus 4 ($p7 != ref $ref) — parts not isolated"
echo "MULTIOUT-PERCHAN PASS (per-channel param routed to part 3, isolated from part 7 — §9.4 key 5 per event)"
