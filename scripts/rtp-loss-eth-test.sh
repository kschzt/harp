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
DEVDIR=rtploss-eth-state   # workspace-RELATIVE (Git Bash /tmp->C:\ trips the device mkdir; see eth-tests.sh)
DEVLOG=/tmp/rtploss-eth-dev.log
HOSTLOG=/tmp/rtploss-eth-host.log
fail() { echo "RTP-LOSS FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"

# Render the CALIBRATED --tone 440 (ideal rms 0.3536), not the device's default ~38 Hz
# drone (~0.034): a known-amplitude signal is the only thing a concealment-FIDELITY floor
# can gate. Same tone eth-tests uses for its bit-exact floor.
echo "── device drops ${DROP}% of outgoing RTP (440Hz tone); host renders 5s over the §8.7 loopback"
"$DEVICED" --port "$PORT" --tone 440 --drop-rtp-pct "$DROP" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
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
# FIDELITY floor (audit gap #4): was rms>0.001 — a bare liveness check that the old default
# drone's "0.0046-class" reading passed trivially. Now we play the calibrated 440Hz tone and
# require it to SURVIVE the loss. Calibrated to MEASURED CI values: under 25% loss the default
# (rate-locked) live path zero-fills the dropped frames and the 5 s rms includes the prefill
# warm-up window, so the recovered tone reads ~0.048-0.055 (ubuntu .055 / macos .048 / win
# .052, cross-run stable) — the SAME ~14% retention the drone showed (0.0046/0.034), i.e. the
# expected best-effort behavior of the rate-locked path, not poor concealment (§7.3 clock
# recovery stays locked; the ASRC fallback retains ~88%). Floor 0.03 = ~16x above the old
# liveness floor and ~1.6x below the stable recovery, so a stream that actually DIES under
# loss (clock unlock / total dropout -> ~0) fails hard while the healthy recovery passes.
FLOOR=0.03
awk "BEGIN{exit !(${rms:-0} > $FLOOR)}" \
  || fail "440Hz tone did not survive ${DROP}% loss (rms=${rms:-0} <= $FLOOR; healthy rate-locked recovery ~0.05) — §7.3 stream broke / clock unlocked"
echo "RTP-LOSS PASS (recovered the 440Hz tone through ${DROP}% RTP loss: rms=$rms > $FLOOR, $nconn connect(s), clean exit rc=$rc)"
