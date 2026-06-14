#!/bin/bash
# Linux hardware conformance kit — the self-hosted-CI variant of
# hw-tests.sh. Same sub-tests, repointed for the closet rig: the board
# PI4B-0002 hanging off the NUC runner over USB, the Linux VST3 plugin
# path (~/.vst3, the install-linux target), the networked web panel
# (harptest.local:8080), and ci@-ssh for the replug daemon restart.
#
# Differences from the macOS hw-tests.sh, all via env (the sub-scripts are
# otherwise identical and shared):
#   - PLUG/VST point at ~/.vst3 instead of ~/Library/Audio/Plug-Ins
#   - the bus board is PI4B-0002, not the desk unit PI4B-0001
#   - replug restarts the daemon as ci@harptest.local (never jak)
#   - no AU: it is macOS-only, so golden/tempo-lock auto-skip the AU half
#   - multidevice-test self-skips (it needs two boards; the rig has one)
# The Ableton "Live" claim guards are harmless here (no DAW on the runner).
set -u
cd "$(dirname "$0")/.."

# the Linux VST3 bundle (cmake --build build-vst --target install-linux)
export PLUG="${PLUG:-$HOME/.vst3/harp-shell.vst3}"
export VST="$PLUG"                                  # recall/timing/soak read $VST
export HARP_HOST="${HARP_HOST:-harptest.local}"     # web panel host (network)
export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0002}"
export HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
export PROBE="${PROBE:-./build/harp-probe}"
export PI="${PI:-ci@harptest.local}"               # replug ssh target (NOT jak)
export SERIAL="$HARP_DEVICE_SERIAL"                # replug pins this board

PASS=0; FAIL=0; SKIP=0
run() {
    echo "──── $1"
    if "$@"; then PASS=$((PASS+1));
    elif [ $? -eq 2 ]; then SKIP=$((SKIP+1));
    else FAIL=$((FAIL+1)); fi
    echo
}
run scripts/golden-test.sh
run scripts/recall-test.sh
run scripts/timing-test.sh
run scripts/soak.sh "${SOAK_SECONDS:-30}"
run scripts/tempo-lock-test.sh
run scripts/multidevice-test.sh
run scripts/replug-test.sh
echo "════ hw-tests (linux): $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
