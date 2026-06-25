#!/usr/bin/env bash
# eth-latency-probe — measure the TRUE end-to-end (event -> audible audio) latency of a
# live HARP session. The host's REPORTED PDC (latencySamples) is blind to the free-running
# RTP jitter buffer (ethTargetFrames), so it badly under-counts what you actually feel.
#
# METHOD: --set-at flips Master Level (param 7) 0.5 -> 0.0 at a known stream position
# (0.5 s). We render to WAV and detect where the audio actually goes silent; the lateness
# of that step vs the set-point is the felt latency. Sweeping ETH_TARGET (the RTP buffer)
# and watching the step MOVE isolates the buffer's contribution with no timing theory.
#
# GOTCHAS (each cost a debugging round):
#   1. Device param state PERSISTS. Every run MUST self-reset (--set-at 0.05:7=0.5 FIRST),
#      or a prior run's Master Level=0 poisons it and you get silent renders.
#   2. Run each transport at ITS OWN safe cushion. USB needs ~512 (HARP_CUSHION_FRAMES
#      default); at 128 it gaps, and the dropouts fake a (negative/bogus) step.
#   3. The ABSOLUTE delta carries a ~reported-sized offset (--set-at is PDC-stamped), so
#      trust the COMPARISON (and the ETH_TARGET differential) over the exact millisecond.
#
# usage:
#   HARP_ETH_DEVICE=192.168.10.1:47987 HARP_DEVICE_SERIAL=KR260-0001 \
#       scripts/eth-latency-probe.sh 2048 512 384      # eth: sweep these ETH_TARGETs
#   HARP_DEVICE_SERIAL=PI4B-0001 scripts/eth-latency-probe.sh usb     # USB baseline
set -u
cd "$(dirname "$0")/.."
PLUG=${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}
HOSTBIN=${HOSTBIN:-./build-vst/harp-vst3-host}
[ -x "$HOSTBIN" ] && [ -n "$PLUG" ] || { echo "need $HOSTBIN + harp-shell.vst3"; exit 1; }

detect() { # $1=wav -> prints "base=.. delta=.. (..ms)" or INVALID/NO-TRANS
python3 - "$1" <<'PY'
import wave, array, sys
w=wave.open(sys.argv[1],'rb'); ch,sr,n=w.getnchannels(),w.getframerate(),w.getnframes()
a=array.array('h'); a.frombytes(w.readframes(n)); xs=a[0::ch]
W=64
def rms(i):
    seg=xs[i:i+W]; return (sum(v*v for v in seg)/len(seg))**0.5 if seg else 0.0
base=max(rms(int(t*sr)) for t in (0.2,0.3,0.4)); setp=int(0.5*sr)
if base<100: print(f"base={base:.0f} INVALID(silent pre-step — persisted ML=0? gapping?)"); sys.exit()
thr=0.2*base
step=next((i for i in range(int(0.4*sr),n-W,W) if rms(i)<thr and rms(i-W)>=thr), None)
print(f"base={base:.0f} NO-TRANSITION(stayed loud)" if step is None
      else f"base={base:.0f} delta={step-setp} ({(step-setp)/sr*1000:+.1f}ms felt-lateness)")
PY
}

run() { # $1=ETH_TARGET("" for usb) -> one render+detect
  local T="$1" extra=""
  [ -n "$T" ] && extra="HARP_ETH_TARGET=$T"
  env $extra HARP_CUSHION_FRAMES="${HARP_CUSHION_FRAMES:-128}" \
    perl -e 'alarm 25; exec @ARGV' "$HOSTBIN" "$PLUG" --realtime --seconds 2 --block "${BLOCK:-64}" \
    --set-at 0.05:7=0.5 --set-at 0.5:7=0.0 --out /tmp/elp.wav >/tmp/elp.log 2>&1
  local un rep; un=$(grep -oE "underruns: [0-9]+" /tmp/elp.log | grep -oE "[0-9]+$"); rep=$(grep -oE "reported-samples=[0-9]+" /tmp/elp.log | head -1 | cut -d= -f2)
  printf "  ETH_TARGET=%-6s reported=%-5s under=%-5s  " "${T:-(usb)}" "${rep:-?}" "${un:-?}"; detect /tmp/elp.wav
}

if [ "${1:-}" = usb ]; then
  : "${HARP_DEVICE_SERIAL:?set HARP_DEVICE_SERIAL for usb}"
  unset HARP_ETH_DEVICE; HARP_CUSHION_FRAMES=${HARP_CUSHION_FRAMES:-512}  # USB-safe floor
  echo "USB baseline (serial $HARP_DEVICE_SERIAL, cushion $HARP_CUSHION_FRAMES):"; run ""; run ""
else
  : "${HARP_ETH_DEVICE:?set HARP_ETH_DEVICE=IP:PORT for eth}"
  echo "eth probe ($HARP_ETH_DEVICE), sweeping ETH_TARGET ${*:-2048 384}:"
  for T in "${@:-2048 384}"; do run "$T"; run "$T"; done
fi
# restore Master Level so we don't leave the device silent
env perl -e 'alarm 15; exec @ARGV' "$HOSTBIN" "$PLUG" --realtime --seconds 0.5 --block "${BLOCK:-64}" --set-at 0.05:7=0.5 --out /tmp/elp_rst.wav >/dev/null 2>&1 && echo "(Master Level restored to 0.5)"
