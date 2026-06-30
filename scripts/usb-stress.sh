#!/usr/bin/env bash
# usb-stress.sh — USB FunctionFS gadget STRESS/DURESS regression test.
#
# Hammers a HARP USB device every brutal way a host (DAW) or operator can abuse it and asserts
# the gadget NEVER hard-wedges: no uninterruptible kernel D-state thread, and the device always
# returns to service (a fresh claim succeeds, or at most one device-daemon restart recovers it —
# never a reboot). Built after the 2026-06-29 "make USB rock-solid under duress" hunt, which ran
# ~175 cycles of these vectors and found 0 hard wedges (see memory usb-gadget-wedge-rootcause).
#
# The "hard mode" the historical notes feared ("kernel D-state, needs reboot") does NOT reproduce
# in the current code — this test is the standing proof of that, and the guard that keeps it true.
#
# Vectors:
#   crash    — SIGKILL the host mid-stream (a DAW crash), then a fresh claim (the user reopening)
#   killd    — SIGKILL the device daemon mid-stream (no on_term: UDC left bound, in-flight dwc2
#              force-reaped) -> systemd Restart=always brings it back
#   stopstr  — `systemctl stop` racing a LIVE stream (on_term UDC unbind vs in-flight dwc2 requests)
#   wedgestop— set_config(0) soft-wedge THEN `systemctl stop` (UDC-unbind a wedged gadget — the
#              specific sequence the old notes blamed for the reboot-class D-state)
#
# usage:  TARGET_SER=PI4B-0003 TARGET_SSH=jak@harp2.local N=10 scripts/usb-stress.sh
# env:    TARGET_SER (USB serial), TARGET_SSH (ssh host for the daemon), N (iterations/vector),
#         USBCTL (path to the set_config helper; built from tools if absent — see below)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SER="${TARGET_SER:-PI4B-0003}"; SSHH="${TARGET_SSH:-jak@harp2.local}"; N="${N:-10}"
HOST="$ROOT/build-vst/harp-vst3-host"; PROBE="$ROOT/build/harp-probe"
BUNDLE="$(find "$ROOT/build-vst" -name harp-shell.vst3 -type d | head -1)"
SSH="ssh -o ConnectTimeout=8 $SSHH"
# set_config(0) soft-wedge helper (a tiny libusb tool). Provide via $USBCTL or it's skipped.
USBCTL="${USBCTL:-/tmp/usbctl}"
# which vectors to run (subset of: crash killd stopstr wedgestop). Default all; CI uses "crash killd".
VECTORS="${VECTORS:-crash killd stopstr wedgestop}"
want() { case " $VECTORS " in *" $1 "*) return 0;; *) return 1;; esac; }

HARD=0; DST=0
# report any uninterruptible (D-state) thread in the device daemon — the hard-wedge signature.
dthreads() { $SSH 'P=$(pgrep -x harp-deviced|head -1); [ -z "$P" ]&&exit; for t in /proc/$P/task/*; do
  tid=$(basename $t); s=$(sed -E "s/^[^)]*\) //" /proc/$tid/stat 2>/dev/null|cut -d" " -f1)
  [ "$s" = "D" ]&&echo "D-STATE tid$tid: $(sudo cat /proc/$tid/stack 2>/dev/null|head -5|tr "\n" "@")"; done; true' 2>/dev/null; }
probe_ok() { perl -e 'alarm 8; exec @ARGV' "$PROBE" -d usb:$SER params 2>&1 | grep -qi "Master Level"; }
recover() { $SSH 'sudo -n systemctl restart harp-deviced-usb' >/dev/null 2>&1; sleep 3; }
stream_bg() { HARP_DEVICE_SERIAL=$SER HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" --set 7=0.7 \
  --channel 1 --part 1 --notes 50,53,57,60 --seconds 15 --realtime --out /tmp/usbstress.wav >/dev/null 2>&1 & echo $!; }
# how long to let a churned xHCI quiesce after the single recovery restart (seconds).
QUIESCE_S="${QUIESCE_S:-60}"
# xHCI-passthrough backstop (Fix C): the ONLY thing that resets a genuinely wedged passthrough
# controller on this rig is a host-side `virsh destroy <guest> && virsh start <guest>` of the
# libvirt guest — NOT an in-guest reboot, NOT USBDEVFS_RESET, and uhubctl is unavailable here.
# But this suite normally runs INSIDE that guest, so it cannot virsh-destroy its own VM without
# killing the runner mid-probe; the reset is therefore necessarily host/operator-driven. If
# $BACKSTOP_CMD is set (an out-of-band command that can reset the passthrough — e.g. run from the
# libvirt host, not the runner's own guest), we invoke it and probe ONCE more; if that still
# fails it is a true fault and we fail loud. When unset (the in-guest default) we skip straight to
# the loud REBOOT-CLASS WEDGE fail, and the runbook (memory hw-rig-runner) is: from the libvirt
# host run `virsh destroy <guest> && virsh start <guest>` to reset the passthrough xHCI.
backstop() {
  [ -n "${BACKSTOP_CMD:-}" ] || return 1
  echo ">>> backstop: resetting the passthrough xHCI via \$BACKSTOP_CMD"
  eval "$BACKSTOP_CMD" >/dev/null 2>&1; sleep 10; probe_ok; }
# after a stress: capture D-state, then assert recovery WITHOUT re-disrupting the device.
# A SIGKILLed daemon churns the passthrough xHCI, so re-enumeration can take ~6-10s to settle.
# The old code did THREE rapid restarts (each = another disconnect) gated only on sleep 3, so
# every probe fired before the controller settled and was re-disrupted by the next restart —
# 3×3s could not catch a 6-10s settle, mislabelling a recoverable settle as a wedge. The real
# guarantee is "recovers from a crash with at most ONE daemon restart, never a reboot"; we test
# exactly that: one restart, then poll the (now quiesced) device every ~3s for up to QUIESCE_S
# without disturbing it in between. Only if it is STILL unclaimable after the full quiesced
# window — and the host-side xHCI backstop (if wired) also fails — do we declare a true
# REBOOT-CLASS WEDGE and fail loud. (Genuine wedges still fail: nothing here masks a dead device.)
assert_recovers() { local tag="$1" d deadline; d=$(dthreads); [ -n "$d" ] && { DST=$((DST+1)); echo ">>> [$tag] D-STATE: $d"; }
  probe_ok && return                              # fresh claim already works
  recover                                         # exactly ONE daemon restart
  deadline=$((SECONDS + QUIESCE_S))               # quiesced settle window, NO further disruption
  while [ "$SECONDS" -lt "$deadline" ]; do
    probe_ok && { echo ">>> [$tag] recovered after quiesced settle"; return; }
    sleep 3
  done
  backstop && { echo ">>> [$tag] recovered via host-side xHCI backstop"; return; }
  HARD=1; echo ">>> [$tag] STILL STUCK after ${QUIESCE_S}s quiesced window + backstop = REBOOT-CLASS WEDGE"; }

echo "=== USB stress: target $SER via $SSHH, $N iters/vector ==="
recover; probe_ok || { echo "device not reachable at start; aborting"; exit 2; }

if want crash; then echo "--- crash: SIGKILL host mid-stream + fresh claim, ${N}x ---"
for i in $(seq 1 $N); do HP=$(stream_bg); sleep 2; kill -9 $HP 2>/dev/null; wait $HP 2>/dev/null; assert_recovers "crash.$i"; done
fi

if want killd; then echo "--- killd: SIGKILL the daemon mid-stream -> systemd auto-restart, ${N}x ---"
for i in $(seq 1 $N); do recover; HP=$(stream_bg); sleep 1.5
  $SSH 'sudo -n systemctl kill -s KILL harp-deviced-usb' >/dev/null 2>&1; kill -9 $HP 2>/dev/null; wait $HP 2>/dev/null; sleep 3
  assert_recovers "killd.$i"
  sleep 2; done # short settle between brutal cycles: test the crash-recovery guarantee, not a disconnect pile-up
fi

if want stopstr; then echo "--- stopstr: systemctl stop racing a live stream, ${N}x ---"
for i in $(seq 1 $N); do recover; HP=$(stream_bg); sleep 2
  $SSH 'sudo -n systemctl stop harp-deviced-usb' >/dev/null 2>&1; sleep 1; kill -9 $HP 2>/dev/null; wait $HP 2>/dev/null
  $SSH 'sudo -n systemctl start harp-deviced-usb' >/dev/null 2>&1; sleep 2; assert_recovers "stopstr.$i"
  sleep 2; done # short settle between brutal cycles (see killd)
fi

if want wedgestop && [ -x "$USBCTL" ]; then
  echo "--- wedgestop: set_config(0) soft-wedge THEN systemctl stop (UDC-unbind a wedged gadget), ${N}x ---"
  for i in $(seq 1 $N); do recover; "$USBCTL" $SER cfg0 >/dev/null 2>&1; sleep 1
    $SSH 'sudo -n systemctl stop harp-deviced-usb' >/dev/null 2>&1; sleep 1
    $SSH 'sudo -n systemctl start harp-deviced-usb' >/dev/null 2>&1; sleep 2; assert_recovers "wedgestop.$i"; done
else echo "--- wedgestop: SKIP (no \$USBCTL set_config helper) ---"; fi

echo "=== RESULT: D-states=$DST  reboot-class-wedges=$HARD ==="
recover
if [ "$HARD" = 0 ] && [ "$DST" = 0 ] && probe_ok; then echo "USB-STRESS PASS — gadget rock-solid (no D-state, no reboot-class, recovers every time)"; exit 0
else echo "USB-STRESS FAIL — investigate (D-states=$DST reboot-class=$HARD)"; exit 1; fi
