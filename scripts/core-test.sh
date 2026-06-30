#!/bin/bash
# core-test — §5.5 core methods over the §8.7 loopback.
#
# Launches harp-deviced; harp-probe `core-test` exercises the four core methods beyond
# hello/credit and asserts each: core.ping echoes a nonce verbatim (liveness); core.identify
# re-fetches the identity WITHOUT resetting the session (must match hello); core.changed
# delivers a D->H "re-query topic" hint (triggered via the refdev seam, since the refdev has
# no spontaneous identity change); core.bye is acknowledged as an orderly session end. No
# audio stream needed — this is the control plane only.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17922}"
DEVDIR=core-state   # workspace-RELATIVE
DEVLOG=/tmp/core-dev.log
fail() { echo "CORE FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT INT TERM
rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

"$PROBE" -d "127.0.0.1:$PORT" core-test || { cat "$DEVLOG"; fail "core-test assertions failed"; }
