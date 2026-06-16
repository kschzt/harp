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

# recover: reset the device to a known-clean state by restarting the daemon, then
# wait until it is claimable. A CI job killed mid-USB-stream (cancellation/timeout)
# OR a long suite's cumulative rapid claim/release can leave the gadget poisoned
# ("device busy / never connects") until a device-side reattach (see host/usb_io.c
# lore). Used as the start-of-suite preflight AND, in run(), to self-heal a test
# that reports the bus busy (rc=3). Graceful if the Pi is unreachable.
recover() {
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI" 'sudo -n systemctl restart harp-deviced-usb' 2>/dev/null \
        || { echo "   (recover skipped: $PI unreachable)"; return 0; }
    for i in $(seq 1 20); do
        "$PROBE" -d "usb:$SERIAL" identify >/dev/null 2>&1 && { echo "   device claimable after ~${i}s"; return 0; }
        sleep 1
    done
}
echo "──── preflight: device reset (daemon restart on $PI)"
recover
echo

PASS=0; FAIL=0; SKIP=0
# Sub-test exit codes: 0 pass; 2 = not applicable on this rig (legit SKIP, e.g.
# multidevice needs two boards); 3 = device busy/unavailable (a hard FAIL — the
# runner owns the device exclusively, so "busy" means a poisoned bus or a stray
# claimer, never something to skip past); any other code = FAIL.
run() {
    echo "──── $1"
    "$@"; rc=$?
    # rc=3 = bus busy/poisoned. On a runner that owns the device exclusively this
    # is a cycling-poisoned gadget (the prior claim-heavy test left it wedged), not
    # a stray claimer — recover (daemon restart) and retry the test ONCE before
    # ruling. A genuine inability to claim then still fails (one retry, no masking).
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
run scripts/replug-test.sh
echo "════ hw-tests (linux): $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
