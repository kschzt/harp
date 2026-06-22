#!/bin/bash
# credit-eth-test — §4.2.1b credit flow-control under STARVATION, over the §8.7 loopback.
#
# The happy path never exhausts the 16 MiB grant, so the new obj-send QUEUE + FLUSH +
# re-grant logic is dead there (the eth-suite recall/diag tests prove the empty-queue path
# stays byte-identical). This test shrinks BOTH sides' grant to 1500 bytes via the
# HARP_FORCE_CREDIT_GRANT seam — smaller than the refdev project closure (~3.3 KiB across 9
# objects) but LARGER than any single object (945 B, so no oversize-object stall). Under
# that window a recall PUSH (host->device, the full closure) cannot fit at once: the sender
# MUST queue the overflow and drain it as the receiver consumes and re-grants. If the recall
# still restores BYTE-IDENTICALLY, the queue+flush+re-grant cycle works — in order, no loss,
# no deadlock. A watchdog converts a credit deadlock into a failure, and a non-vacuity check
# asserts the closure genuinely exceeded the grant (so the test can't silently pass without
# forcing the queue).
set -u
cd "$(dirname "$0")/.."
export HARP_RECONCILE_TIMEOUT_MS=1000
export HARP_FORCE_CREDIT_GRANT=1500   # §4.2.1b seam: both grant_credit (device) + client_grant (host)

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PROBE="${PROBE:-./build/harp-probe}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47903}"
STATEDIR=credit-eth-state   # workspace-RELATIVE (see recall-eth-test.sh header)
STATEFILE=credit-eth.state
fail() { echo "CREDIT-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 bundle not found"

rm -rf "$STATEDIR" "$STATEFILE"; : > /tmp/credit-eth-dev.log
"$DEVICED" --port "$PORT" --state-dir "$STATEDIR" >/tmp/credit-eth-dev.log 2>&1 & DEVPID=$!
trap 'kill -9 "$DEVPID" 2>/dev/null' EXIT INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/credit-eth-dev.log 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" /tmp/credit-eth-dev.log || { cat /tmp/credit-eth-dev.log; fail "device didn't start on $PORT"; }

export HARP_ETH_DEVICE="127.0.0.1:$PORT"
export HARP_DEVICE_SERIAL="SIM-0001"

echo "── §4.2.1b: recall round-trip under a ${HARP_FORCE_CREDIT_GRANT}-byte credit window (forces queue+flush)"
# warm-up (settle metering/echo so HPRE is in the stable regime HPOST will also be in)
"$HOSTBIN" "$PLUG" --set 3=0.81 --set 7=0.31 --set 1=0.61 --notes 62,69,74,65 --seconds 0.6 --hash >/dev/null 2>&1
# ground-truth render + DAW save
HPRE=$("$HOSTBIN" "$PLUG" --set 3=0.81 --set 7=0.31 --set 1=0.61 --notes 62,69,74,65 \
       --seconds 0.6 --hash --save-state "$STATEFILE" 2>/dev/null | sed -n 's/^output-hash: //p')
[ -n "$HPRE" ] || fail "no pre-mutation render-hash (save render produced no output-hash)"
# musician mutates the device, then DAW reopen -> recall PUSH: the closure travels through
# the tiny window. Watchdog (perl-alarm) turns a credit deadlock into a clean failure.
"$PROBE" -d "$HARP_ETH_DEVICE" knob 3 0.10 >/dev/null 2>&1 || fail "knob mutate"
perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --load-state "$STATEFILE" --notes 62,69,74,65 \
    --seconds 0.6 --hash >/tmp/credit-eth.log 2>&1
rc=$?
[ "$rc" -eq 142 ] && { cat /tmp/credit-eth.log; fail "recall HUNG under starvation — credit DEADLOCK (watchdog fired)"; }
[ "$rc" -eq 0 ]   || { cat /tmp/credit-eth.log; fail "load/recall render failed rc=$rc under starvation"; }
grep -q "restored\|SYNCED" /tmp/credit-eth.log || { cat /tmp/credit-eth.log; fail "no recall action logged"; }
HPOST=$(sed -n 's/^output-hash: //p' /tmp/credit-eth.log)
[ -n "$HPOST" ] || fail "no post-restore render-hash (load render produced no output-hash)"
[ "$HPRE" = "$HPOST" ] || fail "EXACT-HASH mismatch under starvation: restored $HPOST != ground-truth $HPRE (queue/flush corrupted or reordered the pushed closure)"

# non-vacuity: the pushed closure must genuinely exceed the grant, or no queueing was forced.
CLOSURE=$(find "$STATEDIR/objects" -type f 2>/dev/null | xargs cat 2>/dev/null | wc -c | tr -d ' ')
[ "${CLOSURE:-0}" -gt "$HARP_FORCE_CREDIT_GRANT" ] \
    || fail "closure (${CLOSURE:-0} B) <= grant ($HARP_FORCE_CREDIT_GRANT B) — test vacuous, no queueing forced"

echo "CREDIT-ETH PASS (§4.2.1b: ${CLOSURE}B closure recalled byte-identically through a ${HARP_FORCE_CREDIT_GRANT}B window — queue+flush+re-grant, in order, no loss, no deadlock)"
