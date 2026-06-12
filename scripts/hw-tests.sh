#!/bin/bash
# The hardware conformance kit: everything CI can't run because it needs
# the real device on the bus. Run before and after meaningful changes.
# Device must be unclaimed (close the DAW). ~2 minutes + soak time.
set -u
cd "$(dirname "$0")/.."
PASS=0; FAIL=0; SKIP=0
run() {
    echo "──── $1"
    if "$@"; then PASS=$((PASS+1));
    elif [ $? -eq 2 ]; then SKIP=$((SKIP+1));
    else FAIL=$((FAIL+1)); fi
    echo
}
run scripts/recall-test.sh
run scripts/timing-test.sh
run scripts/soak.sh "${SOAK_SECONDS:-30}"
run scripts/tempo-lock-test.sh
run scripts/replug-test.sh
echo "════ hw-tests: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
