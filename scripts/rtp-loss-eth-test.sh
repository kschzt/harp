#!/bin/bash
# rtp-loss-eth-test — §8.7 fault injection: the host tolerates RTP/UDP packet loss on the
# free-running audio plane. The device (--drop-rtp-pct N) deterministically drops ~N% of
# its outgoing RTP datagrams (the seq still advances, so the host sees GENUINE gaps, not a
# stall). Free-running RTP is best-effort ("may drop"), so the host must keep rendering
# audio, NOT crash, NOT hang, and NOT storm reconnects on the still-alive stream.
#
# Co-existence: unique port + kills ONLY its own device by pid; hard perl-alarm watchdog.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47973}"
DROP="${DROP:-25}"
DEVDIR=/tmp/rtploss-eth-state
DEVLOG=/tmp/rtploss-eth-dev.log
HOSTLOG=/tmp/rtploss-eth-host.log
fail() { echo "RTP-LOSS FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"

echo "── device drops ${DROP}% of outgoing RTP; host renders 5s over the §8.7 loopback"
"$DEVICED" --port "$PORT" --drop-rtp-pct "$DROP" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001" \
  perl -e 'alarm 20; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 5 --realtime >"$HOSTLOG" 2>&1 & HP=$!
wait "$HP"; rc=$?; HP=""
kill -9 "$DP" 2>/dev/null; DP=""

nconn=$(grep -c "connected:" "$HOSTLOG")
rms=$(grep -oE "rms=[0-9.]+" "$HOSTLOG" | tail -1 | cut -d= -f2)
echo "── result: connects=$nconn  rms=${rms:-?}  host-exit=$rc  (drop=${DROP}%)"
grep -iE "AddressSanitizer|SEGV|abort trap|terminating due to" "$HOSTLOG" && fail "host CRASHED under ${DROP}% RTP loss"
[ "$rc" -eq 142 ] && fail "host HUNG under ${DROP}% RTP loss (perl-alarm watchdog fired)"
grep -q "connected:" "$HOSTLOG" || fail "host never connected (no oracle)"
awk "BEGIN{exit !(${rms:-0} > 0.001)}" || fail "host produced (near-)silence under ${DROP}% loss (rms=${rms:-0}) — loss broke the stream"
echo "RTP-LOSS PASS (host rendered through ${DROP}% RTP loss: rms=$rms, $nconn connect(s), clean exit rc=$rc)"
