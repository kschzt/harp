#!/bin/bash
# The hardware conformance kit: everything CI can't run because it needs
# the real device on the bus. Run before and after meaningful changes.
# Device must be unclaimed (close the DAW). ~2 minutes + soak time.
set -u
cd "$(dirname "$0")/.."
# Deterministic device on a multi-board bus: single-device tests pin one
# board (the desk unit with the web panel); multidevice-test clears this.
export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
SERIAL="$HARP_DEVICE_SERIAL"
PROBE="${PROBE:-./build/harp-probe}"
PI="${PI:-jak@harp.local}"   # ssh target for device recovery (the desk board)

# recover: reset the device by restarting its daemon, then wait until claimable.
# A long suite's cumulative rapid claim/release (esp. the re-neg's audio.stop/start
# in late-sink) can wedge the USB gadget ("device busy / never connects"); a daemon
# restart re-creates the FunctionFS endpoints clean. Used as the start-of-suite
# preflight AND, in run(), to self-heal a test that reports the bus busy (rc=3).
recover() {
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI" 'sudo -n systemctl restart harp-deviced-usb' 2>/dev/null \
        || { echo "   (recover skipped: $PI unreachable)"; return 0; }
    for i in $(seq 1 20); do
        "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL" && { echo "   device back after ~${i}s"; sleep 2; return 0; }
        sleep 1
    done
}
echo "──── preflight: device reset (daemon restart on $PI)"; recover; echo

PASS=0; FAIL=0; SKIP=0
# Sub-test exit codes: 0 pass; 2 = not applicable on this rig (legit SKIP, e.g.
# multidevice needs two boards); 3 = device busy/unavailable (a hard FAIL — the
# suite needs exclusive ownership, never a silent skip); any other code = FAIL.
run() {
    echo "──── $1"
    "$@"; rc=$?
    # rc=3 = bus busy/poisoned (a cycling-wedged gadget, not a stray claimer when
    # the suite owns the device) — recover and retry the test ONCE before ruling.
    if [ $rc -eq 3 ]; then
        echo "   ↻ device reported busy (rc=3) — recovering and retrying once"
        recover
        "$@"; rc=$?
    fi
    if   [ $rc -eq 0 ]; then PASS=$((PASS+1));
    elif [ $rc -eq 2 ]; then SKIP=$((SKIP+1));
    else FAIL=$((FAIL+1));
         [ $rc -eq 3 ] && echo "   ↑ device still not exclusively claimable after recovery — FAILURE"; fi
    echo
}
run scripts/golden-test.sh
run scripts/multitimbral-test.sh
run scripts/recall-test.sh
run scripts/recall-perpart-test.sh
run scripts/cross-format-recall-test.sh
run scripts/timing-test.sh
run scripts/soak.sh "${SOAK_SECONDS:-30}"
run scripts/tempo-lock-test.sh
run scripts/multidevice-test.sh
run scripts/session-share-test.sh
run scripts/alias-play-test.sh
run scripts/alias-group-e2e.sh
run scripts/alias-part-audio-test.sh
run scripts/late-sink-test.sh
run scripts/part-param-iso-test.sh
run scripts/meter-test.sh
run scripts/replug-test.sh
echo "════ hw-tests: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
