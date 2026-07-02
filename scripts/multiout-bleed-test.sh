#!/bin/sh
# multiout-bleed-test — ADJACENT-part zero-bleed isolation for the Kontakt/Overbridge-style
# multi-out main instance over §8.7 free-running RTP (the wide-union >8-slot multi-packet path).
#
# COVERAGE GAP this closes: multiout-iso-test.sh already pins zero-bleed, but only between
# parts EIGHT slots apart (channels 3 & 7 -> buses 4 & 8). The most likely per-part routing
# bug — an off-by-one/off-by-two SLOT index — bleeds into the ADJACENT bus, which a
# far-apart pair cannot see: part C uses slots {2+2C, 3+2C}; part C+1 uses {4+2C, 5+2C},
# the very next pair. This test drives a note on part C and asserts the IMMEDIATELY adjacent
# parts (C-1 -> bus C, C+1 -> bus C+2) are BIT-EXACT silent, at the LOW / MIDDLE / HIGH ends
# of the 16-part range (channels 1, 8, 15) so a boundary-only slot bug can't hide. For the
# middle part it also FULL-SCANS every per-part bus (total isolation: only bus 0 + the one
# part bus sound). An unfed sink is STRUCTURALLY silent (rms == 0.00000), so the bleed
# assertions are jitter-INDEPENDENT and stay strict on every OS.
#
# Plus an OFFLINE (host-paced TCP) BIT-EXACT SILENCE golden: two DIFFERENT unplayed part
# buses must hash IDENTICALLY (true all-zero silence — a partial bleed would differ between
# sinks) AND be deterministic run-to-run, while the SOUNDING part bus must hash DIFFERENTLY
# (guards a false "everything is silent" pass — the same host-paced render path USB drives).
#
# channel C -> device part C (§P2.1) -> surfaces on bus C+1; bus 0 = summed main mix.
# POSIX-only (the perl-alarm capture harness; the protocol is OS-independent so mac+Linux cover it).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17995}"
FLOOR="${FLOOR:-0.005}"   # non-silence liveness floor (a sounding bus is observed ~0.046)
SECS="${SECS:-1.2}"
# Timing oracle (mirrors multiout-iso/eth-tests): the realtime captures come off the device's
# FREE-RUNNING RTP emit. On Linux that emit is clock_nanosleep(TIMER_ABSTIME)-paced (deterministic)
# → STRICT single-shot. macOS/Windows take the #else RELATIVE-nanosleep path, which can underrun the
# render under shared-runner load and drag a SOUNDING bus's rms under the floor — the runner's
# timing, not a routing bug. So on mac/win retry the LIVENESS captures for a clean window; the floor
# stays the strict 0.005 level (a real silent/broken/bleeding bus stays sub-floor every try AND
# trips the strict Linux job, so the retry masks nothing). The BLEED checks are bit-exact and never
# retried.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WIN=1;; *) WIN=0;; esac
case "$(uname -s)" in Darwin) MAC=1;; *) MAC=0;; esac
ISO_TRIES=1; { [ "$MAC" = 1 ] || [ "$WIN" = 1 ]; } && ISO_TRIES=3
fail() { echo "MULTIOUT-BLEED FAIL: $1"; [ -n "${DP:-}" ] && kill -9 "$DP" 2>/dev/null; rm -rf multiout-bleed-state; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

gt() { awk "BEGIN{exit !($1 > $2)}"; }
# realtime rms of one render: cap <channel> <capture-bus>
cap() {
    HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 20; exec @ARGV' \
        "$HOSTBIN" "$PLUG" --realtime --out-buses 17 --channel "$1" --capture-bus "$2" \
        --notes 62,69,74,65 --seconds "$SECS" 2>/dev/null | grep -oE "rms=[0-9.]+" | tail -1 | cut -d= -f2
}
# offline (host-paced TCP) deterministic hash of one render: ohash <channel> <capture-bus>
ohash() {
    HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' \
        "$HOSTBIN" "$PLUG" --out-buses 17 --channel "$1" --capture-bus "$2" \
        --notes 62,69,74,65 --seconds "$SECS" --hash 2>/dev/null | grep -oE "output-hash: [0-9a-f]+" | tail -1 | cut -d' ' -f2
}

echo "── multiout-bleed: 17-bus main over §8.7 RTP (34-slot union, multi-packet) on 127.0.0.1:$PORT"
rm -rf multiout-bleed-state; : > /tmp/multiout-bleed-dev.log
"$DEVICED" --port "$PORT" --serial SIM-MO-BLEED --state-dir multiout-bleed-state >/tmp/multiout-bleed-dev.log 2>&1 & DP=$!
trap 'kill -9 "$DP" 2>/dev/null; rm -rf multiout-bleed-state' EXIT INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/multiout-bleed-dev.log 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" /tmp/multiout-bleed-dev.log || { cat /tmp/multiout-bleed-dev.log; fail "device didn't start on $PORT"; }

# check_channel <channel> <full|adj> : drive part C, assert part-bus sounds + main sounds
# (liveness, retried on mac/win), and the ADJACENT part buses are bit-exact silent. `full`
# additionally scans EVERY per-part bus for total isolation.
check_channel() {
    C="$1"; mode="$2"
    pb=$((C + 1)); la=$C; ra=$((C + 2))   # part bus, left-adjacent (part C-1), right-adjacent (part C+1)
    try=0
    while :; do
        try=$((try + 1))
        r_pb=$(cap "$C" "$pb"); r_main=$(cap "$C" 0)
        if gt "${r_pb:-0}" "$FLOOR" && gt "${r_main:-0}" "$FLOOR"; then break; fi
        [ "$try" -ge "$ISO_TRIES" ] && break
        echo "  ↻ channel $C liveness sub-floor (free-running RTP underrun on a loaded runner, not a routing bug) — retrying"
    done
    gt "${r_pb:-0}" "$FLOOR"   || fail "channel $C -> part bus $pb is SILENT (rms=${r_pb:-?}); routing/demux broken"
    gt "${r_main:-0}" "$FLOOR" || fail "channel $C -> main mix (bus 0) is SILENT (rms=${r_main:-?})"
    # ADJACENT parts: bit-exact zero (jitter-independent). This is the gap multiout-iso misses.
    r_la=$(cap "$C" "$la"); [ "${r_la:-1}" = "0.00000" ] || fail "BLEED: channel $C leaked into L-adjacent (part $((C-1))) bus $la (rms=${r_la:-?}); adjacent buses not isolated"
    if [ "$ra" -le 16 ]; then
        r_ra=$(cap "$C" "$ra"); [ "${r_ra:-1}" = "0.00000" ] || fail "BLEED: channel $C leaked into R-adjacent (part $((C+1))) bus $ra (rms=${r_ra:-?}); adjacent buses not isolated"
        echo "  channel $C: part bus $pb=$r_pb SOUNDS; adjacent buses $la & $ra bit-exact silent; main=$r_main"
    else
        echo "  channel $C: part bus $pb=$r_pb SOUNDS; adjacent bus $la bit-exact silent (bus $ra > 16, off the union); main=$r_main"
    fi
    if [ "$mode" = full ]; then
        n_silent=0
        B=1
        while [ "$B" -le 16 ]; do
            if [ "$B" -ne "$pb" ]; then
                r=$(cap "$C" "$B"); [ "${r:-1}" = "0.00000" ] || fail "BLEED: channel $C leaked into bus $B (rms=${r:-?}); buses not isolated"
                n_silent=$((n_silent + 1))
            fi
            B=$((B + 1))
        done
        echo "  channel $C FULL-SCAN: only bus 0 (main) + bus $pb (part) sound; all $n_silent other per-part buses bit-exact silent"
    fi
}

check_channel 1  adj    # LOW boundary: part 1 -> bus 2, adjacents 1 & 3
check_channel 8  full   # MIDDLE: part 8 -> bus 9, adjacents 8 & 10, plus total-isolation full scan
check_channel 15 adj    # HIGH boundary: part 15 -> bus 16, adjacent 15 (bus 17 off the union)

# ── OFFLINE (host-paced TCP) BIT-EXACT SILENCE golden ───────────────────────────────────
# The same host-paced render path USB drives. channel 8 -> part bus 9. Two DIFFERENT unplayed
# part buses (7 and 10) must hash IDENTICALLY (true all-zero silence — a partial bleed would
# make two sinks differ) AND be deterministic run-to-run; the SOUNDING bus 9 must hash
# DIFFERENTLY (a "false silence" guard: proves the device actually rendered a part, so the
# equal-silence result above is real isolation, not a dead render).
s7a=$(ohash 8 7); s7b=$(ohash 8 7); s10=$(ohash 8 10); snd=$(ohash 8 9)
echo "  offline hashes: silent bus7 #1=${s7a:-HUNG} #2=${s7b:-HUNG}  silent bus10=${s10:-HUNG}  sounding bus9=${snd:-HUNG}"
[ -n "$s7a" ] && [ -n "$s10" ] || fail "OFFLINE silent-bus capture HUNG (host-paced pacing stall)"
[ "$s7a" = "$s7b" ]  || fail "OFFLINE silent bus 7 non-deterministic (#1=$s7a #2=$s7b)"
[ "$s7a" = "$s10" ]  || fail "OFFLINE silent buses 7 and 10 differ ($s7a vs $s10) — a partial bleed differs between adjacent-region sinks"
[ -n "$snd" ]        || fail "OFFLINE sounding-bus capture HUNG"
[ "$snd" != "$s7a" ] || fail "OFFLINE sounding bus 9 hashes as SILENCE ($snd) — device rendered nothing (false-pass guard tripped)"
echo "  offline golden: silent buses 7==10 bit-identical ($s7a), deterministic; sounding bus 9 distinct ($snd)"

kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; rm -rf multiout-bleed-state; DP=
echo "MULTIOUT-BLEED PASS (adjacent-part zero-bleed at low/mid/high boundaries + full-scan total isolation + offline bit-exact silence golden)"
