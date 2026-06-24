#!/bin/bash
# reconcile-readonly-eth-test — §11.4 action 3 "Open read-only" over the §8.7 loopback.
#
# When the user EXPLICITLY picks Read-only at the reconcile offer, the shell MUST disable
# automation *write* (not merely skip the connect-time auto-push). re-audit med-open-ro-noop +
# test gap gap-open-ro-test: choice==2 used to only log + return, leaving readOnlyDefault_ false on
# a matching engine — so live param/automation writes still reached the device.
#
# This drives the REAL VST3 host: stage a project, dirty the device's live so the host posts a
# reconcile offer, pick choice 2 (Read-only) via the device --panel-sock while the host's offer
# window is open, and issue a live --set-at param write. The poll BLOCKS the render until the pick,
# so the explicit read-only hold (roExplicit_) is set before the write fires. Assert the write was
# SUPPRESSED ("read-only: suppressed") and the project was NOT re-asserted.
#
# Co-existence: unique port; kills ONLY its own pids; perl-alarm watchdog; workspace-RELATIVE state
# dir. Exit 0 pass / 1 fail / 2 N/A (binaries absent).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PROBE="${PROBE:-./build/harp-probe}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47994}"
SERIAL="${SERIAL:-SIM-RO-0001}"
DEVDIR=reconcile-ro-eth-state           # workspace-RELATIVE (Git Bash /tmp->C:\ trips device mkdir)
DEVLOG=/tmp/reconcile-ro-dev.log
HOSTLOG=/tmp/reconcile-ro-host.log
STATE=/tmp/reconcile-ro.state
SOCK=/tmp/reconcile-ro-panel.sock
PHOST=/tmp/reconcile-ro-phost
fail() { echo "RECONCILE-RO FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] && [ -x "$HOSTBIN" ] && [ -x "$PROBE" ] || { echo "RECONCILE-RO SKIP: build device+host+probe first"; exit 2; }
[ -n "$PLUG" ] && [ -d "$PLUG" ] || { echo "RECONCILE-RO SKIP: harp-shell.vst3 not found"; exit 2; }

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null; rm -f "$SOCK"' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }
panel() { python3 -c "
import socket,sys
s=socket.socket(socket.AF_UNIX); s.connect('$SOCK')
s.sendall((sys.argv[1]+'\n').encode()); sys.stdout.write(s.recv(4096).decode())" "$1" 2>/dev/null; }

rm -rf "$DEVDIR" "$PHOST"; : > "$DEVLOG"; rm -f "$STATE" "$SOCK"; mkdir -p "$PHOST"
echo "── start device (serial $SERIAL, --panel-sock) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" --panel-sock "$SOCK" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# 0) stage a project bundle (borns the device + sets hasBundle_ on the load run below).
: > "$HOSTLOG"
HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" \
  perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 1 --realtime --save-state "$STATE" >"$HOSTLOG" 2>&1
[ -s "$STATE" ] || { cat "$HOSTLOG"; fail "save-state produced no fixture at $STATE"; }
echo "   ✓ staged a project fixture ($(wc -c <"$STATE") bytes)"

# 1) dirty the device's live so it DIVERGES from the saved bundle — the host then posts a reconcile
#    offer (a clean hash-match would take pushStateLocked's silent path, no offer).
"$PROBE" -d 127.0.0.1:$PORT -s "$PHOST" knob 3 0.7 >/dev/null 2>&1 || fail "probe knob (create conflict) failed"
echo "   ✓ dirtied the device live (param 3) -> a reconcile conflict"

# 2) run the host with the offer window open + a live --set-at write; concurrently pick Read-only (2)
#    as soon as the host posts the offer. The poll blocks the render, so the hold is set first.
: > "$HOSTLOG"
HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" HARP_RECONCILE_TIMEOUT_MS=8000 \
  perl -e 'alarm 40; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 2 --realtime \
  --load-state "$STATE" --set-at 1.0:3=0.9 >"$HOSTLOG" 2>&1 & HP=$!
chose=0
for _ in $(seq 1 80); do
  g=$(panel "reconcile-get")
  if echo "$g" | grep -q '"pending":true'; then panel "reconcile-choose 2" >/dev/null; chose=1; break; fi
  kill -0 "$HP" 2>/dev/null || break
  sleep 0.2
done
wait "$HP"; rc=$?; HP=""
[ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "host HUNG (perl-alarm watchdog fired)"; }
[ "$chose" -eq 1 ] || { cat "$DEVLOG"; cat "$HOSTLOG"; fail "host never posted a reconcile offer — could not drive choice 2 (no conflict?)"; }

# 3) the host MUST have honored Read-only (choice 2) by dropping the live --set-at write.
grep -q "reconcile -> Read-only" "$HOSTLOG"   || { cat "$HOSTLOG"; fail "host did not record the Read-only pick (choice 2 not received from the panel)"; }
grep -q "read-only: suppressed" "$HOSTLOG"     || { cat "$HOSTLOG"; fail "host did NOT suppress the live --set-at write after Open-read-only (med-open-ro-noop: the explicit action was a no-op for the write gate)"; }
grep -q "project state re-asserted" "$HOSTLOG" && { cat "$HOSTLOG"; fail "host RE-ASSERTED the project despite the Read-only pick"; }
echo "   ✓ explicit §11.4 Open-read-only: live writes SUPPRESSED + project not re-asserted"
echo "RECONCILE-RO PASS: the explicit Read-only reconcile pick disables automation writes (med-open-ro-noop)"
