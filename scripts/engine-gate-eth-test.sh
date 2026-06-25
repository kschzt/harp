#!/bin/bash
# engine-gate-eth-test — §13.4 device-side engine-version load gate over the §8.7 loopback.
#
# Starts harp-deviced with a REPORTED engine (--engine-ver 2.2.0) that differs in MINOR from the
# compile-time ENGINE_VERSION (2.1.0) its snapshots are stamped with, so the device's own fresh
# snapshot is a "foreign-engine" snapshot to itself. harp-probe `engine-gate` then asserts a
# state.refset of it is REFUSED 'incompatible' WITHOUT the consent flag (bit 0x4) and LOADS WITH it
# — proving both the device gate (refuses a MINOR diff, not only MAJOR) AND the now-reachable
# host-side consent override (med-134-minor + the consent half of med-consent-unreachable, §13.4).
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-47926}"
DEVDIR=engine-gate-state   # workspace-RELATIVE (Git Bash /tmp -> C:\ trips the MinGW device mkdir)
DEVLOG=/tmp/engine-gate-dev.log
fail() { echo "ENGINE-GATE-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf "$DEVDIR"' EXIT INT TERM
rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" --engine-ver 2.2.0 >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

"$PROBE" -d "127.0.0.1:$PORT" engine-gate || { cat "$DEVLOG"; fail "engine-gate assertions failed"; }
echo "ENGINE-GATE-ETH PASS"
