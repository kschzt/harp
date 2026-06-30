#!/bin/bash
# reconnect-eth-test — §8.7 fault injection: the shell survives a mid-session device
# DISCONNECT and RECONNECTS when the device returns. Over the loopback (no hardware):
# start the device, connect the host (rendering), KILL the device mid-render, RESTART it
# on the same port, then assert the host reconnected (a 2nd "connected:") and exited
# cleanly — never hung (hard perl-alarm watchdog) or crashed. Exercises the supervisor/
# reconnect path that clean-localhost tests never hit. (We have USB replug-test; this is
# its §8.7-Ethernet analogue.)
#
# Co-existence: unique port + kills ONLY its own device by pid (no broad pkill), so it
# won't fight a co-running agent's harp-deviced on the same box.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17971}"
DEVDIR=reconnect-eth-state   # workspace-RELATIVE: an absolute /tmp arg is path-converted by
                             # Git Bash to C:/Users/.../Temp/..., whose drive component trips
                             # the device's recursive mkdir on Windows (see eth-tests.sh)
DEVLOG=/tmp/reconnect-eth-dev.log
HOSTLOG=/tmp/reconnect-eth-host.log
fail() { echo "RECONNECT FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""
start_dev() { "$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!; }
stop_dev()  { [ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; DP=""; }
HP=""
trap 'stop_dev; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { # $1 = how many "listening" lines to expect by now
    for _ in $(seq 1 25); do [ "$(grep -c "listening on $PORT" "$DEVLOG" 2>/dev/null)" -ge "$1" ] && return 0; sleep 0.2; done
    return 1
}

rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"
echo "── start device + host (rendering over the §8.7 loopback)"
start_dev; wait_listen 1 || { cat "$DEVLOG"; fail "device didn't start"; }

# host renders 8s realtime; perl alarm is a HARD watchdog so a stuck reconnect can't hang.
HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001" \
  perl -e 'alarm 25; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 8 --realtime >"$HOSTLOG" 2>&1 &
HP=$!
for _ in $(seq 1 60); do grep -q "connected:" "$HOSTLOG" 2>/dev/null && break; sleep 0.1; done
grep -q "connected:" "$HOSTLOG" || fail "host never connected initially (no oracle)"
echo "   host connected + rendering"
sleep 1.5

echo "── DISCONNECT: kill the device mid-render"
stop_dev
sleep 1.5   # host should see the link drop and supervise for hot-plug

echo "── RESTART the device on the same port"
start_dev; wait_listen 2 || { cat "$DEVLOG"; fail "device didn't restart"; }

wait "$HP"; rc=$?; HP=""
stop_dev

nconn=$(grep -c "connected:" "$HOSTLOG")
echo "── result: connects=$nconn  host-exit=$rc"
grep -iE "AddressSanitizer|SEGV|abort trap|terminating due to" "$HOSTLOG" && fail "host CRASHED across the disconnect/reconnect"
[ "$rc" -eq 142 ] && fail "host HUNG on the disconnect (perl-alarm watchdog fired) — supervisor/reconnect stuck"
[ "$nconn" -ge 2 ] || fail "host did NOT reconnect after the device returned (only $nconn connect(s)) — §8.7 reconnect path broken"
echo "RECONNECT PASS (host survived a mid-session disconnect + reconnected: $nconn connects, clean exit rc=$rc)"
