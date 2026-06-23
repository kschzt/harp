#!/bin/bash
# reported-latency-test — §6.4/§8.7 reported PDC latency the plugin advertises to the DAW
# (getLatencySamples / clap latency.get / kAudioUnitProperty_Latency), a DETERMINISTIC read-back.
#
# Two things are gated:
#  (1) §8.7 FREE-RUNNING (RTP) PDC INCLUDES THE JITTER BUFFER. The reported latency on the
#      free-running path is ethTargetFrames + event headroom, NOT the USB ring — reporting the
#      USB figure under-stated network-audio latency and mis-aligned DAW automation against
#      other tracks (2026-06-23 audit, PR2b). ethTargetFrames = clamp(device rt-floor key 14,
#      2*B, 12288); we declare a KNOWN --rt-floor so the value is pinned. vst3-host drives
#      --realtime (which negotiates free-running RTP); host-paced/offline has no jitter buffer.
#  (2) CROSS-FORMAT AGREEMENT, host-paced (offline): VST3 / CLAP / AU reach the SAME runtime
#      latency by different codepaths and MUST agree to the sample (a divergence mis-aligns a
#      DAW). Host-paced = the USB-ring value max(2*B,512)+max(B,256).
#
# clap-host / au-host don't drive --realtime, so the free-running value is gated via vst3 — the
# runtime's latencySamples() is format-agnostic, so all formats yield the same free-running value.
set -u
cd "$(dirname "$0")/.."
unset HARP_CUSHION_BLOCKS   # the ONLY env knob that shifts the expected latency

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) OSID=windows; EXE=.exe ;; Darwin) OSID=macos; EXE= ;; *) OSID=linux; EXE= ;; esac
find1() { find "$1" -name "$2" -type f 2>/dev/null | head -1; }

DEVICED="${DEVICED:-./build/harp-deviced}"
[ -x "$DEVICED" ] || DEVICED="$(find1 . "harp-deviced$EXE")"
HOSTBIN="${HOSTBIN:-$(find1 build-vst "harp-vst3-host$EXE")}"
CHOST="${CHOST:-$(find1 build-vst "clap-host$EXE")}"
AUHOST="${AUHOST:-$(find1 build-vst "au-host")}"
VPLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
CPLUG="$(find build-vst build -maxdepth 5 -name 'harp-clap.clap' 2>/dev/null | head -1)"
AUCOMP="$HOME/Library/Audio/Plug-Ins/Components/harp-au.component"
PORT="${PORT:-47961}"
BLOCK="${BLOCK:-256}"
RTFLOOR=1024
DEVDIR=replat-eth-state
DEVLOG=/tmp/replat-eth-dev.log
have() { [ -n "${1:-}" ] && [ -x "$1" ]; }

hr=$(( BLOCK > 256 ? BLOCK : 256 ))
EXPECT_HP=$(( (2*BLOCK > 512 ? 2*BLOCK : 512) + hr ))         # host-paced (offline): USB ring
EXPECT_FR=$(( (2*BLOCK > RTFLOOR ? 2*BLOCK : RTFLOOR) + hr )) # free-running (RTP): jitter buffer

fail() { echo "REPORTED-LATENCY FAIL: $1"; FAIL=1; }
FAIL=0
[ -x "$DEVICED" ] || { echo "REPORTED-LATENCY FAIL: harp-deviced not built"; exit 1; }
have "$HOSTBIN" && [ -d "$VPLUG" ] || { echo "REPORTED-LATENCY FAIL: vst3 host/bundle not built"; exit 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --rt-floor "$RTFLOOR" --state-dir "$DEVDIR" --panel-sock "" >>"$DEVLOG" 2>&1 & DP=$!
trap 'kill -9 "$DP" 2>/dev/null' EXIT
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; echo "REPORTED-LATENCY FAIL: device didn't start"; exit 1; }
export HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001"

echo "── reported-latency on $OSID: block=$BLOCK → host-paced=$EXPECT_HP, free-running=$EXPECT_FR samples @48k"

# check NAME WANT MODE BIN [args...] : run a host, grep its reported-latency line, assert == WANT.
check() {
    name="$1"; want="$2"; mode="$3"; shift 3; log="/tmp/replat-$name.log"
    perl -e 'alarm 30; exec @ARGV' "$@" --block "$BLOCK" --seconds 0.2 $mode >"$log" 2>&1
    [ $? -eq 142 ] && { cat "$log"; fail "$name HUNG (watchdog)"; return; }
    got=$(grep -E 'latency: reported-samples=[0-9]+' "$log" | tail -1 | sed -nE 's/.*reported-samples=([0-9]+).*/\1/p')
    if [ -z "$got" ]; then cat "$log"; fail "$name printed no latency line"
    elif [ "$got" != "$want" ]; then fail "$name reported-samples=$got, want $want (block=$BLOCK)"
    else echo "   ✓ $name: reported-samples=$got (== $want)"; fi
}

# (1) the §8.7 FIX: free-running (RTP) PDC includes the jitter buffer. vst3-host drives --realtime.
check vst3-freerun "$EXPECT_FR" "--realtime" "$HOSTBIN" "$VPLUG"
# (2) cross-format agreement, host-paced (offline) — the SAME runtime value via 3 codepaths.
check vst3 "$EXPECT_HP" "" "$HOSTBIN" "$VPLUG"
if have "$CHOST" && [ -n "$CPLUG" ]; then check clap "$EXPECT_HP" "" "$CHOST" "$CPLUG"; else echo "   ⏭ SKIP clap (not built on $OSID)"; fi
if [ "$OSID" = macos ] && have "$AUHOST" && [ -d "$AUCOMP" ]; then check au "$EXPECT_HP" "" "$AUHOST"; \
   else [ "$OSID" = macos ] && echo "   ⏭ SKIP au (au-host/component not installed)"; fi

[ "$FAIL" = 0 ] && echo "REPORTED-LATENCY PASS: free-running=$EXPECT_FR (jitter buffer) + host-paced=$EXPECT_HP cross-format (§6.4/§8.7)" || { echo "REPORTED-LATENCY FAIL"; exit 1; }
