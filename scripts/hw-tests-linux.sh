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
#   - no AU: it is macOS-only, so golden/tempo-lock auto-skip the AU half and
#     cross-format-recall-test self-SKIPs (exit 2) — VST3<->AU project move is
#     exercised only on the macOS desk unit (no AU-capable automated runner)
#   - multidevice-test RUNS: the rig now has two boards (PI4B-0002 + PI4B-0003,
#     the 2nd via PCI USB-controller passthrough) so it exercises the selection
#     rules (exact / same-model-fallback / never-cross-model); self-skips (exit 2)
#     only if a board is absent
# The Ableton "Live" claim guards are harmless here (no DAW on the runner).
set -u
cd "$(dirname "$0")/.."

# Automated conformance: never block a shell recall waiting for a front-panel pick
# (mirrors hw-tests.sh). The §11.4 reconcile defaults to a 30s window for live DAW
# use; 0 = fall back immediately (archive-protected Push), keeping the suite fast.
export HARP_RECONCILE_TIMEOUT_MS="${HARP_RECONCILE_TIMEOUT_MS:-0}"

# the Linux VST3 bundle (cmake --build build-vst --target install-linux)
export PLUG="${PLUG:-$HOME/.vst3/harp-shell.vst3}"
export VST="$PLUG"                                  # recall/timing/soak read $VST
export HARP_HOST="${HARP_HOST:-harptest.local}"     # web panel host (network)
export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0002}"
export HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
export PROBE="${PROBE:-./build/harp-probe}"
export PI="${PI:-ci@harptest.local}"               # replug ssh target (NOT jak)
export SERIAL="$HARP_DEVICE_SERIAL"                # replug pins this board

# recover: reset the device to a known-clean state by restarting the daemon (re-creates
# the FunctionFS gadget), then wait until claimable. A CI job killed mid-USB-stream
# (cancellation/timeout) OR a long suite's cumulative rapid claim/release can leave the
# gadget poisoned ("device busy / never connects"). Each claimable probe is time-boxed
# (`timeout`) so a wedged device can NEVER hang the whole suite — a probe against a
# half-enumerated gadget can otherwise block indefinitely (it once stalled the suite
# ~20min to the job timeout). Used as the start-of-suite preflight AND, in run(), to
# self-heal a test that reports the bus busy (rc=3). Graceful if the Pi is unreachable.
#
# THE LEVER for the host-side wedge a bare daemon restart can't clear: the runner VM
# (PCI-passthrough) keeps a stale USB node/claim because a restart re-creates the gadget
# device-side but never makes the HOST see a disconnect — harp-deviced binds the UDC
# (device/ffs.c) and a plain restart leaves it bound. The daemon now UNBINDS its UDC on
# SIGTERM (harp-deviced.c on_term), so `systemctl stop` makes the host see a real
# disconnect (it drops its node + stale claim); the pause lets the host settle, then
# `start` re-binds -> the host re-enumerates a fresh device. Unlike the host-side
# USBDEVFS_RESET tried before (which reset the passthrough into a hung state and was
# removed), this is the gadget unplugging itself — it never touches the host controller.
# (Done daemon-side, not via a configfs `tee`, because ci@ has sudo only for systemctl.)
recover() {
    ssh -o BatchMode=yes -o ConnectTimeout=8 "$PI" '
        sudo -n systemctl stop harp-deviced-usb 2>/dev/null
        sleep 2
        sudo -n systemctl start harp-deviced-usb' 2>/dev/null \
        || { echo "   (recover skipped: $PI unreachable)"; return 0; }
    for i in $(seq 1 20); do
        timeout 8 "$PROBE" -d "usb:$SERIAL" identify >/dev/null 2>&1 && { echo "   device claimable after ~${i}s"; return 0; }
        sleep 1
    done
}
echo "──── preflight: device reset (UDC re-plug on $PI)"
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
run scripts/note-expr-test.sh
run scripts/voice-steal-test.sh
run scripts/clap-test.sh
run scripts/mpe-test.sh
run scripts/multitimbral-test.sh
run scripts/recall-test.sh
run scripts/recall-perpart-test.sh
run scripts/cross-format-recall-test.sh  # self-SKIPs on Linux (AU is macOS-only) — keeps the macOS-only gate visible in the log
run scripts/timing-test.sh
run scripts/soak.sh "${SOAK_SECONDS:-30}"
run scripts/tempo-lock-test.sh
run scripts/multidevice-test.sh
run scripts/alias-group-e2e.sh
run scripts/late-sink-test.sh
# alias-part-audio + part-param-iso run on eth.yml (the §8.7 loopback / eth-tests.sh, both
# rate-lock AND varispeed-ASRC), NOT here: both sustain a WIDE multi-part stream (the full
# slot union) that this VM-passthrough USB rig can't hold reliably. The per-part demux/
# isolation is transport-agnostic, so the loopback covers it reliably (and adds real §8.7
# multichannel-ASRC coverage); the USB rig keeps the tests it can actually hold. (M3 DONE:
# both now drive the single-owner multi-out harness — tsan-host's one private runtime + the
# per-part sinks, no registry — but stay on the loopback; eth-tests.sh depends on them.)
echo "──── alias-part-audio + part-param-iso: run on eth.yml (loopback — the sustained wide multi-part stream wedges this passthrough USB rig); SKIP here"; SKIP=$((SKIP+2))
run scripts/meter-test.sh
run scripts/replug-test.sh
echo "════ hw-tests (linux): $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
