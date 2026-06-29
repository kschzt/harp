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

HARD=0; DST=0
# report any uninterruptible (D-state) thread in the device daemon — the hard-wedge signature.
dthreads() { $SSH 'P=$(pgrep -x harp-deviced|head -1); [ -z "$P" ]&&exit; for t in /proc/$P/task/*; do
  tid=$(basename $t); s=$(sed -E "s/^[^)]*\) //" /proc/$tid/stat 2>/dev/null|cut -d" " -f1)
  [ "$s" = "D" ]&&echo "D-STATE tid$tid: $(sudo cat /proc/$tid/stack 2>/dev/null|head -5|tr "\n" "@")"; done; true' 2>/dev/null; }
probe_ok() { perl -e 'alarm 8; exec @ARGV' "$PROBE" -d usb:$SER params 2>&1 | grep -qi "Master Level"; }
recover() { $SSH 'sudo systemctl restart harp-deviced-usb' >/dev/null 2>&1; sleep 3; }
stream_bg() { HARP_DEVICE_SERIAL=$SER HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" --set 7=0.7 \
  --channel 1 --part 1 --notes 50,53,57,60 --seconds 15 --realtime --out /tmp/usbstress.wav >/dev/null 2>&1 & echo $!; }
# after a stress: capture D-state, then assert recovery (fresh claim / 1 restart / NEVER a reboot)
assert_recovers() { local tag="$1" d; d=$(dthreads); [ -n "$d" ] && { DST=$((DST+1)); echo ">>> [$tag] D-STATE: $d"; }
  probe_ok && return; recover; probe_ok && return; recover; recover
  probe_ok || { HARD=1; echo ">>> [$tag] STILL STUCK after 3 restarts = REBOOT-CLASS WEDGE"; }; }

echo "=== USB stress: target $SER via $SSHH, $N iters/vector ==="
recover; probe_ok || { echo "device not reachable at start; aborting"; exit 2; }

echo "--- crash: SIGKILL host mid-stream + fresh claim, ${N}x ---"
for i in $(seq 1 $N); do HP=$(stream_bg); sleep 2; kill -9 $HP 2>/dev/null; wait $HP 2>/dev/null; assert_recovers "crash.$i"; done

echo "--- killd: SIGKILL the daemon mid-stream -> systemd auto-restart, ${N}x ---"
for i in $(seq 1 $N); do recover; HP=$(stream_bg); sleep 1.5
  $SSH 'sudo pkill -9 -x harp-deviced' >/dev/null 2>&1; kill -9 $HP 2>/dev/null; wait $HP 2>/dev/null; sleep 3
  assert_recovers "killd.$i"; done

echo "--- stopstr: systemctl stop racing a live stream, ${N}x ---"
for i in $(seq 1 $N); do recover; HP=$(stream_bg); sleep 2
  $SSH 'sudo systemctl stop harp-deviced-usb' >/dev/null 2>&1; sleep 1; kill -9 $HP 2>/dev/null; wait $HP 2>/dev/null
  $SSH 'sudo systemctl start harp-deviced-usb' >/dev/null 2>&1; sleep 2; assert_recovers "stopstr.$i"; done

if [ -x "$USBCTL" ]; then
  echo "--- wedgestop: set_config(0) soft-wedge THEN systemctl stop (UDC-unbind a wedged gadget), ${N}x ---"
  for i in $(seq 1 $N); do recover; "$USBCTL" $SER cfg0 >/dev/null 2>&1; sleep 1
    $SSH 'sudo systemctl stop harp-deviced-usb' >/dev/null 2>&1; sleep 1
    $SSH 'sudo systemctl start harp-deviced-usb' >/dev/null 2>&1; sleep 2; assert_recovers "wedgestop.$i"; done
else echo "--- wedgestop: SKIP (no \$USBCTL set_config helper) ---"; fi

echo "=== RESULT: D-states=$DST  reboot-class-wedges=$HARD ==="
recover
if [ "$HARD" = 0 ] && [ "$DST" = 0 ] && probe_ok; then echo "USB-STRESS PASS — gadget rock-solid (no D-state, no reboot-class, recovers every time)"; exit 0
else echo "USB-STRESS FAIL — investigate (D-states=$DST reboot-class=$HARD)"; exit 1; fi
