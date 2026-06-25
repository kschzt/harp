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

hr=$(( BLOCK > 256 ? BLOCK : 256 ))   # §9.2 event headroom: max(DAW block, device pacing block) — host SEND lookahead
BD=256                                # §6.4 key 3: device render/turnaround block — the §14.3 loopback measures RTT=256
EXPECT_HP=$(( (2*BLOCK > 512 ? 2*BLOCK : 512) + hr + BD ))         # host-paced: USB ring + headroom + device block
EXPECT_FR=$(( (2*BLOCK > RTFLOOR ? 2*BLOCK : RTFLOOR) + hr + BD )) # free-running: jitter buffer + headroom + device block

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

# (3) §6.4 NON-ZERO CONVERTER: a device declaring keys 1/2 (analog-in/out latency) MUST fold them into
# PDC alongside the render block (key 3) — the path the pure-digital refdev (in=out=0) never exercises.
CIN=64; COUT=32; CPORT=$((PORT+1)); CDEVDIR="${DEVDIR}-conv"; CDEVLOG=/tmp/replat-conv-dev.log
rm -rf "$CDEVDIR"; : > "$CDEVLOG"
"$DEVICED" --port "$CPORT" --rt-floor "$RTFLOOR" --in-lat "$CIN" --out-lat "$COUT" --state-dir "$CDEVDIR" --panel-sock "" >>"$CDEVLOG" 2>&1 & CDP=$!
trap 'kill -9 "$DP" "$CDP" 2>/dev/null' EXIT
for _ in $(seq 1 25); do grep -q "listening on $CPORT" "$CDEVLOG" 2>/dev/null && break; sleep 0.2; done
if grep -q "listening on $CPORT" "$CDEVLOG"; then
    HARP_ETH_DEVICE="127.0.0.1:$CPORT" check vst3-converter "$(( EXPECT_HP + CIN + COUT ))" "" "$HOSTBIN" "$VPLUG"
    rm -rf "$CDEVDIR"
else cat "$CDEVLOG"; fail "converter device (--in-lat/--out-lat) didn't start"; fi

# (4) §8.7 LOW-LATENCY FREE-RUNNING: a device declaring small nsamples (64) + a tight rt-floor, at a
# small DAW block, must report the SMALL free-running chain — buffer + nsamples + headroom(max(block,ns))
# — NOT the host-paced 512 (kBlock turnaround + kBlock headroom). The render block + event headroom both
# track ethNsamples, so this path lands ~5.3ms where the default would report 1536 (32ms). RME-grounded:
# 30s switch soak = 0 underruns, 96-note block-64 storm = 0 late events (host detector + §14.2 evt_late).
LLPORT=$((PORT+2)); LLDEVDIR="${DEVDIR}-ll"; LLDEVLOG=/tmp/replat-ll-dev.log
rm -rf "$LLDEVDIR"; : > "$LLDEVLOG"
"$DEVICED" --port "$LLPORT" --rt-nsamples 64 --rt-floor 128 --state-dir "$LLDEVDIR" --panel-sock "" >>"$LLDEVLOG" 2>&1 & LLDP=$!
trap 'kill -9 "$DP" "$CDP" "$LLDP" 2>/dev/null' EXIT
for _ in $(seq 1 25); do grep -q "listening on $LLPORT" "$LLDEVLOG" 2>/dev/null && break; sleep 0.2; done
if grep -q "listening on $LLPORT" "$LLDEVLOG"; then
  lllog=/tmp/replat-ll.log
  env HARP_ETH_DEVICE="127.0.0.1:$LLPORT" perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$VPLUG" --realtime --block 64 --seconds 0.2 >"$lllog" 2>&1
  llgot=$(grep -E 'reported-samples=[0-9]+' "$lllog" | tail -1 | sed -nE 's/.*reported-samples=([0-9]+).*/\1/p')
  if [ "$llgot" = 256 ]; then echo "   ✓ vst3-lowlat-freerun: reported-samples=256 (nsamples=64 @ block 64 = 5.3ms, vs 1536 default)"
  else fail "low-latency free-running reported=$llgot, want 256 (buffer 128 + nsamples 64 + headroom 64)"; fi
  rm -rf "$LLDEVDIR"
else cat "$LLDEVLOG"; fail "low-latency device (--rt-nsamples 64) didn't start"; fi

[ "$FAIL" = 0 ] && echo "REPORTED-LATENCY PASS: free-running=$EXPECT_FR + host-paced=$EXPECT_HP + converter folded + low-latency free-running=256 (§6.4/§8.7)" || { echo "REPORTED-LATENCY FAIL"; exit 1; }
