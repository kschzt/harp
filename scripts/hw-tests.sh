#!/bin/bash
# The hardware conformance kit: everything CI can't run because it needs
# the real device on the bus. Run before and after meaningful changes.
# Device must be unclaimed (close the DAW). ~2 minutes + soak time.
set -u
cd "$(dirname "$0")/.."
# Deterministic device on a multi-board bus: single-device tests pin one
# board (the desk unit with the web panel); multidevice-test clears this.
export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
PASS=0; FAIL=0; SKIP=0
# Sub-test exit codes: 0 pass; 2 = not applicable on this rig (legit SKIP, e.g.
# multidevice needs two boards); 3 = device busy/unavailable (a hard FAIL — the
# suite needs exclusive ownership, never a silent skip); any other code = FAIL.
run() {
    echo "──── $1"
    "$@"; rc=$?
    if   [ $rc -eq 0 ]; then PASS=$((PASS+1));
    elif [ $rc -eq 2 ]; then SKIP=$((SKIP+1));
    else FAIL=$((FAIL+1));
         [ $rc -eq 3 ] && echo "   ↑ device was not exclusively claimable — counted as a FAILURE, not a skip"; fi
    echo
}
run scripts/golden-test.sh
run scripts/multitimbral-test.sh
run scripts/recall-test.sh
run scripts/recall-perpart-test.sh
run scripts/timing-test.sh
run scripts/soak.sh "${SOAK_SECONDS:-30}"
run scripts/tempo-lock-test.sh
run scripts/multidevice-test.sh
run scripts/session-share-test.sh
run scripts/alias-play-test.sh
run scripts/alias-group-e2e.sh
run scripts/replug-test.sh
echo "════ hw-tests: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
