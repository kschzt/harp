#!/bin/bash
# reported-latency-test — §6.4 reported PDC latency the plugin advertises to the DAW
# (setLatencySamples / getLatencySamples), a DETERMINISTIC read-back (audit gap #4 item 3).
#
# Unlike the loopback RTT (transport buffering, timing-jittery, Linux-strict only), the
# reported latency is a PURE function of the DAW block size — shell/runtime.h latencyFor():
#   latency = max(2*B, 2*256) + max(B, 256)     [depth=2, kBlock=256, HARP_CUSHION_BLOCKS unset]
# = 768 samples (16 ms) for any block in {64,128,256} @ 48k. So we assert EXACT equality on
# EVERY OS and EVERY format with NO per-OS relaxation. Crucially, VST3 (getLatencySamples),
# CLAP (clap_plugin_latency.get) and AU (kAudioUnitProperty_Latency) reach the value by
# DIFFERENT codepaths into the SAME runtime — they MUST agree to the sample, so this also
# catches a cross-format latency divergence (a regression that mis-aligns audio in a DAW).
#
# A device isn't required for the value (it's block-only), but we run against the loopback
# daemon anyway — that's the proven host path and the faithful "what the DAW sees connected".
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
DEVDIR=replat-eth-state
DEVLOG=/tmp/replat-eth-dev.log
have() { [ -n "${1:-}" ] && [ -x "$1" ]; }

# §6.4 expected = max(2*B, 512) + max(B, 256)
tf=$(( 2*BLOCK > 512 ? 2*BLOCK : 512 )); hr=$(( BLOCK > 256 ? BLOCK : 256 ))
EXPECT=$(( tf + hr ))

fail() { echo "REPORTED-LATENCY FAIL: $1"; FAIL=1; }
FAIL=0
[ -x "$DEVICED" ] || { echo "REPORTED-LATENCY FAIL: harp-deviced not built"; exit 1; }
have "$HOSTBIN" && [ -d "$VPLUG" ] || { echo "REPORTED-LATENCY FAIL: vst3 host/bundle not built"; exit 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" --panel-sock "" >>"$DEVLOG" 2>&1 & DP=$!
trap 'kill -9 "$DP" 2>/dev/null' EXIT
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; echo "REPORTED-LATENCY FAIL: device didn't start"; exit 1; }
export HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001"

echo "── reported-latency on $OSID: block=$BLOCK → expect $EXPECT samples ($(awk "BEGIN{printf \"%.1f\", 1000*$EXPECT/48000}") ms @48k)"

# check NAME BIN [args...] : run a host, grep its reported-latency line, assert == EXPECT
check() {
    name="$1"; shift; log="/tmp/replat-$name.log"
    perl -e 'alarm 30; exec @ARGV' "$@" --block "$BLOCK" --seconds 0.2 >"$log" 2>&1
    [ $? -eq 142 ] && { cat "$log"; fail "$name HUNG (watchdog)"; return; }
    got=$(grep -E 'latency: reported-samples=[0-9]+' "$log" | tail -1 | sed -nE 's/.*reported-samples=([0-9]+).*/\1/p')
    if [ -z "$got" ]; then cat "$log"; fail "$name printed no latency line"
    elif [ "$got" != "$EXPECT" ]; then fail "$name reported-samples=$got, want $EXPECT (block=$BLOCK)"
    else echo "   ✓ $name: reported-samples=$got (== $EXPECT)"; fi
}

check vst3 "$HOSTBIN" "$VPLUG"
if have "$CHOST" && [ -n "$CPLUG" ]; then check clap "$CHOST" "$CPLUG"; else echo "   ⏭ SKIP clap (not built on $OSID)"; fi
if [ "$OSID" = macos ] && have "$AUHOST" && [ -d "$AUCOMP" ]; then check au "$AUHOST"; \
   else [ "$OSID" = macos ] && echo "   ⏭ SKIP au (au-host/component not installed)"; fi

[ "$FAIL" = 0 ] && echo "REPORTED-LATENCY PASS: every built format reports $EXPECT samples (§6.4)" || { echo "REPORTED-LATENCY FAIL"; exit 1; }
