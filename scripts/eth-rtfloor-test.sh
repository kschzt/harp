#!/bin/bash
# eth-rtfloor-test — §6.4 rt-profile (identity key 14). A device declares its safe Ethernet RT
# setpoints: sub-key 0 = ethTargetFrames jitter-buffer floor, sub-key 1 = RTP packet size
# (nsamples). The host adopts each in place of its 1024/256 defaults, clamped (floor by the
# 2*maxDawBlock structural floor; packet to [32, kBlock]). Either sub-key may be omitted.
# Reference-fleet defaults: a KR260-* device declares 320/64, a PI4B-* device 448/128, applied
# when no flag is given (harp-deviced). Undeclared (no cap/key) => host keeps 1024/256.
#
# Asserts via the host log "eth audio.start: packet=<ns> prefill/target=<frames> ...".
#
# Co-existence: unique port, kills only its own device by pid, perl-alarm watchdog,
# workspace-RELATIVE state dirs cleaned on exit (see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47990}"
fail() { echo "RTFLOOR FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf rtfloor-*-state 2>/dev/null' EXIT

# $1=label  $2=device serial  $3=device flags (word-split)  $4=expected target  $5=expected packet
check() {
  label="$1"; serial="$2"; flags="$3"; wt="$4"; wp="$5"; dir="rtfloor-$label-state"
  rm -rf "$dir"; : > /tmp/rtf-dev.log; : > /tmp/rtf-host.log
  # shellcheck disable=SC2086  (intentional word-split of $flags)
  "$DEVICED" --port "$PORT" --serial "$serial" $flags --state-dir "$dir" --panel-sock "" >/tmp/rtf-dev.log 2>&1 & DP=$!
  for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/rtf-dev.log 2>/dev/null && break; sleep 0.2; done
  grep -q "listening on $PORT" /tmp/rtf-dev.log || { cat /tmp/rtf-dev.log; fail "$label: device didn't start on $PORT"; }
  HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$serial" \
    perl -e 'alarm 20; exec @ARGV' "$HOSTBIN" "$PLUG" --realtime --seconds 3 --block 64 >/tmp/rtf-host.log 2>&1
  kill -9 "$DP" 2>/dev/null; DP=""
  gt=$(grep -oE "target=[0-9]+" /tmp/rtf-host.log | head -1 | cut -d= -f2)
  gp=$(grep -oE "packet=[0-9]+" /tmp/rtf-host.log | head -1 | cut -d= -f2)
  if [ "$gt" = "$wt" ] && [ "$gp" = "$wp" ]; then
    echo "  $label ($serial ${flags:-<none>}): target=$gt packet=$gp — OK"
  else
    grep -i "eth audio.start" /tmp/rtf-host.log
    fail "$label: target=${gt:-<none>}/packet=${gp:-<none>}, expected target=$wt/packet=$wp"
  fi
}

echo "── rt-profile declaration (block 64 -> structural floor 2*64=128; packet clamp [32, kBlock=256])"
check declared    SIM-decl   "--rt-floor 384 --rt-nsamples 64" 384  64    # both declared -> both adopted
check floor-only  SIM-fonly  "--rt-floor 384"                  384  256   # only floor; packet keeps 256 default
check pkt-only    SIM-ponly  "--rt-nsamples 96"                1024 96    # only packet; floor keeps 1024 default
check undeclared  SIM-none   ""                                1024 256   # nothing declared -> defaults
check clamp-floor SIM-cf     "--rt-floor 32"                   128  256   # floor below 2*block -> clamp up to 128
check clamp-pktlo SIM-cplo   "--rt-nsamples 16"                1024 32    # packet below 32 -> clamp up to 32
check clamp-pkthi SIM-cphi   "--rt-nsamples 9999"              1024 256   # packet above kBlock -> clamp to 256
check clamp-flhi  SIM-cfh    "--rt-floor 99999"                12288 256  # floor above the 12288 ceiling -> clamp down
echo "RTFLOOR-ETH PASS (floor+packet declared/adopted, partial maps, both clamps)"
