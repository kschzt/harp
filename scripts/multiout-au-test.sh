#!/bin/sh
# multiout-au-test — M4 MULTI-OUT + PER-CHANNEL PARAMS for the Audio Unit main (Kontakt-style).
# The SAME first-class multi-out contract as the VST3 shell and the CLAP plugin, proven through
# the AU host. AU renders ONE output element per AudioUnitRender call (unlike VST3/CLAP's single
# all-bus call), so the device clock lives on element 0's main-mix pull, which also drains every
# active part sink into a per-element cache; elements 1..16 then serve that cache.
#   * 17 output elements (element 0 = Main Mix, elements 1..16 = the per-part pairs).
#   * a note on MIDI channel C routes to part C and streams on element C+1.
#   * a GP CC kPerChanCcBase+i on channel N edits part N's device param (i+1), §9.4 key 5 —
#     au-host's --set-ch CH:ID=V injects exactly that raw MIDI CC.
#
# Proofs (deterministic offline hashes; notes on channel 3 -> part 3 -> output element 4):
#   A. ISOLATION  — element 4 (played part) != element 8 (a part with no notes -> silent).
#   B. PER-CHAN   — --set-ch 3:7 (played part's Master Level) CHANGES element 4's hash.
#   C. CROSS-PART — --set-ch 7:7 (a different part's level) LEAVES element 4's hash == ref.
# Fresh device per render (so the part starts at its default), synced on "listening on PORT".
# macOS only (no AU on Linux/Windows): self-skips if au-host / the AU component are absent.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN_AU:-./build-vst/au-host}"
AUCOMP="${AUCOMP:-$HOME/Library/Audio/Plug-Ins/Components/harp-au.component}"
PORT="${PORT:-47950}"
fail() { echo "MULTIOUT-AU FAIL: $1"; [ -n "${DP:-}" ] && kill "$DP" 2>/dev/null; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
if [ ! -x "$HOSTBIN" ] || [ ! -d "$AUCOMP" ]; then
    echo "MULTIOUT-AU SKIP: au-host / harp-au.component absent (macOS only)"; exit 0
fi

# offline hash of one output element with an optional --set-ch; FRESH device, synced on listen
# log. The render is deterministic, so a non-empty hash is final on the first try; the bounded
# retry only covers a transient device-startup/connect race that yields an empty capture under
# suite load — never a wrong hash.
oh() { # $1 = capture element, $2 = set-ch spec ("" for none)
    set_arg=""; [ -n "$2" ] && set_arg="--set-ch $2"
    h=""
    for attempt in 1 2 3; do
        rm -rf multiout-au-state; : > /tmp/multiout-au-dev.log
        "$DEVICED" --port "$PORT" --serial SIM-MO-AU --state-dir multiout-au-state \
            >/tmp/multiout-au-dev.log 2>&1 & DP=$!
        trap 'kill "$DP" 2>/dev/null; rm -rf multiout-au-state' EXIT INT TERM
        i=0; while [ $i -lt 50 ]; do grep -q "listening on $PORT" /tmp/multiout-au-dev.log 2>/dev/null && break; sleep 0.2; i=$((i+1)); done
        if grep -q "listening on $PORT" /tmp/multiout-au-dev.log; then
            # au-host finds the registered AU itself (AudioComponentFindNext) — no path arg,
            # unlike vst3-host/clap-host. The $AUCOMP dir only needs to be INSTALLED (checked above).
            h=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' \
                "$HOSTBIN" --channel 3 --capture-element "$1" $set_arg \
                --notes 62,69,74,65 --seconds 1.2 --hash 2>/dev/null | grep -oE "output-hash: [0-9a-f]+" | cut -d' ' -f2)
        fi
        kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; rm -rf multiout-au-state; DP=
        [ -n "$h" ] && break
    done
    echo "$h"
}

echo "── multiout-au: one main instance, 17 elements, per-channel CC routing on 127.0.0.1:$PORT"
ref=$(oh 4 "");          [ -n "$ref" ]    || fail "element-4 render produced no audio (HUNG?)"
silent=$(oh 8 "");       [ -n "$silent" ] || fail "element-8 render produced no audio (HUNG?)"
p3=$(oh 4 "3:7=0.05");   [ -n "$p3" ]     || fail "set-part-3 render produced no audio (HUNG?)"
p7=$(oh 4 "7:7=0.05");   [ -n "$p7" ]     || fail "set-part-7 render produced no audio (HUNG?)"
echo "  elem4(played)=$ref  elem8(silent)=$silent  set-played=$p3  set-other=$p7"

[ "$silent" != "$ref" ] || fail "element 8 (no notes) == element 4 (played) — elements NOT isolated"
[ "$p3" != "$ref" ]     || fail "played part's param did NOT change element 4 ($p3 == ref) — CC routing broken"
[ "$p7" = "$ref" ]      || fail "other part's param LEAKED into element 4 ($p7 != ref $ref) — parts not isolated"
echo "MULTIOUT-AU PASS (17 elements isolated; per-channel CC routed to the played part, isolated from others — §9.4 key 5)"
