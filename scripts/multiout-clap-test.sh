#!/bin/sh
# multiout-clap-test — M4 MULTI-OUT + PER-CHANNEL PARAMS for the CLAP main (Kontakt-style).
# Same first-class multi-out contract as the VST3 shell, proven through the CLAP host:
#   * 17 output ports (port 0 = Main Mix, ports 1..16 = Part N) — clap_plugin_audio_ports.
#   * a satellite's MIDI CC on channel N edits part N's device param. CLAP carries the CC as a
#     raw CLAP_EVENT_MIDI (status 0xB0|N), which the plugin decodes and queues with §9.4 key 5 = N.
#     clap-host's --set-ch CH:ID=V injects exactly that MIDI CC.
#
# Proofs (deterministic offline hashes; notes on channel 3 -> the part captured on port 4):
#   A. ISOLATION  — port 4 (played part) != port 8 (a part with no notes -> silent).
#   B. PER-CHAN   — --set-ch 3:7 (played part's Master Level) CHANGES port 4's hash.
#   C. CROSS-PART — --set-ch 7:7 (a different part's level) LEAVES port 4's hash == ref.
# Fresh device per render (so the part starts at its default), synced on "listening on PORT".
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
# The clap-host binary: eth-suite exports it as CHOST (it reserves HOSTBIN for the VST3 host),
# so prefer CHOST and fall back to the local build path when run standalone.
CHOST="${CHOST:-./build-vst/clap-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-clap.clap 2>/dev/null | head -1)}"
PORT="${PORT:-17949}"
fail() { echo "MULTIOUT-CLAP FAIL: $1"; [ -n "${DP:-}" ] && kill "$DP" 2>/dev/null; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$CHOST" ] || fail "$CHOST (clap-host) not built"
[ -n "$PLUG" ] || fail "harp-clap.clap not found"

# offline hash of one output port with an optional --set-ch; FRESH device, synced on the listen
# log. The render is fully deterministic, so a non-empty hash is final on the first try; a
# bounded retry only covers a transient device-startup/connect race that yields an empty capture
# (no audio line) — never a wrong hash, just an occasionally missed one under suite load.
oh() { # $1 = capture port, $2 = set-ch spec ("" for none)
    set_arg=""; [ -n "$2" ] && set_arg="--set-ch $2"
    h=""
    for attempt in 1 2 3; do
        rm -rf multiout-clap-state; : > /tmp/multiout-clap-dev.log
        "$DEVICED" --port "$PORT" --serial SIM-MO-CL --state-dir multiout-clap-state \
            >/tmp/multiout-clap-dev.log 2>&1 & DP=$!
        trap 'kill "$DP" 2>/dev/null; rm -rf multiout-clap-state' EXIT INT TERM
        i=0; while [ $i -lt 50 ]; do grep -q "listening on $PORT" /tmp/multiout-clap-dev.log 2>/dev/null && break; sleep 0.2; i=$((i+1)); done
        if grep -q "listening on $PORT" /tmp/multiout-clap-dev.log; then
            h=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' \
                "$CHOST" "$PLUG" --channel 3 --capture-port "$1" $set_arg \
                --notes 62,69,74,65 --seconds 1.2 --hash 2>/dev/null | grep -oE "output-hash: [0-9a-f]+" | cut -d' ' -f2)
        fi
        kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; rm -rf multiout-clap-state; DP=
        [ -n "$h" ] && break
    done
    echo "$h"
}

echo "── multiout-clap: one main instance, 17 ports, per-channel CC routing on 127.0.0.1:$PORT"
ref=$(oh 4 "");          [ -n "$ref" ]    || fail "port-4 render produced no audio (HUNG?)"
silent=$(oh 8 "");       [ -n "$silent" ] || fail "port-8 render produced no audio (HUNG?)"
p3=$(oh 4 "3:7=0.05");   [ -n "$p3" ]     || fail "set-part-3 render produced no audio (HUNG?)"
p7=$(oh 4 "7:7=0.05");   [ -n "$p7" ]     || fail "set-part-7 render produced no audio (HUNG?)"
echo "  port4(played)=$ref  port8(silent)=$silent  set-played=$p3  set-other=$p7"

[ "$silent" != "$ref" ] || fail "port 8 (no notes) == port 4 (played) — ports NOT isolated"
[ "$p3" != "$ref" ]     || fail "played part's param did NOT change port 4 ($p3 == ref) — CC routing broken"
[ "$p7" = "$ref" ]      || fail "other part's param LEAKED into port 4 ($p7 != ref $ref) — parts not isolated"
echo "MULTIOUT-CLAP PASS (17 ports isolated; per-channel CC routed to the played part, isolated from others — §9.4 key 5)"
