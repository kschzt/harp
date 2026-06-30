#!/bin/bash
# offline-edit-eth-test — §15.5: a param edited while the device is ABSENT reaches the device
# on REATTACH (the host's live state wins, "a mismatch resolved by Push"). Over the §8.7
# loopback: start the device, connect the host (rendering), KILL the device, edit a param
# while offline (via --set-at, timed to land in the offline window), RESTART the device → the
# host replays its current params on the reconnect edge → assert via harp-probe that the
# device's live param reflects the offline edit. Pre-fix the edit is lost (the queued event
# is discarded on reconnect, and the stale bundle is what gets pushed).
#
# Co-existence: unique port + kills ONLY its own device by pid; hard perl-alarm watchdog.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PROBE="${PROBE:-./build/harp-probe}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17977}"
PID="${PID:-3}"        # a real device param (Filter Cutoff)
VAL="${VAL:-0.90}"     # the offline-edit target (default is NOT this)
DEVDIR=offedit-eth-state   # workspace-RELATIVE: an absolute /tmp arg is MSYS-path-converted
                           # to a C:\...\ drive path the MinGW device can't mkdir on Windows
DEVLOG=/tmp/offedit-eth-dev.log
HOSTLOG=/tmp/offedit-eth-host.log
PROBELOG=/tmp/offedit-eth-probe.log
fail() { echo "OFFLINE-EDIT FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
start_dev() { "$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!; }
stop_dev()  { [ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; DP=""; }
trap 'stop_dev; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do [ "$(grep -c "listening on $PORT" "$DEVLOG" 2>/dev/null)" -ge "$1" ] && return 0; sleep 0.2; done; return 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"
echo "── start device + host; param $PID will be edited to $VAL at 3s, while the device is dead (1.5–4s)"
start_dev; wait_listen 1 || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001" \
  perl -e 'alarm 25; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 8 --realtime --set-at "3:$PID=$VAL" >"$HOSTLOG" 2>&1 & HP=$!
for _ in $(seq 1 60); do grep -q "connected:" "$HOSTLOG" 2>/dev/null && break; sleep 0.1; done
grep -q "connected:" "$HOSTLOG" || fail "host never connected initially"
sleep 1.5

echo "── DISCONNECT (the --set-at edit fires at 3s, inside this offline window)"
stop_dev
sleep 2.5

echo "── RESTART the device on the same port (host re-asserts its current params on reconnect)"
start_dev; wait_listen 2 || { cat "$DEVLOG"; fail "device didn't restart"; }
wait "$HP"; rc=$?; HP=""

echo "── probe the device's LIVE param state (a fresh connection, after the host detached):"
perl -e 'alarm 12; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" params >"$PROBELOG" 2>&1 || true
stop_dev
grep -iE "param|cutoff|\b$PID\b" "$PROBELOG" | head
[ "$rc" -eq 142 ] && fail "host HUNG (perl-alarm watchdog fired)"
# pull param PID's value out of the probe dump and assert it ≈ VAL (the offline edit landed)
got=$(grep -E "^[[:space:]]*\[$PID\][[:space:]]" "$PROBELOG" | grep -oE "[0-9]\.[0-9]+" | tail -1)
echo "   device param $PID = ${got:-<unparsed>} (wanted ≈ $VAL)"
awk "BEGIN{g=${got:-(-1)}; exit !(g>0 && (g-$VAL<0.05 && $VAL-g<0.05))}" \
    || fail "device param $PID is ${got:-<unparsed>}, not ≈ $VAL — the offline edit did NOT reach the device"
echo "OFFLINE-EDIT PASS (param $PID edited while the device was absent reached it on reattach: $got ≈ $VAL)"
