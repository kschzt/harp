#!/bin/bash
# txn-test — §9.6 event transactions over the §8.7 loopback.
#
# Launches harp-deviced; harp-probe `txn-test` drives the EVT stream and asserts: the device
# advertises evt.txn AND reports the limits (identity key 13 — MUST >= 1 concurrent / >= 256
# events); a txn-tagged param event BUFFERS (the param does NOT move) until txn-commit applies
# the whole batch atomically; txn-abort discards; a commit of an unknown txn-id is a no-op and
# the device keeps serving. The buffered-then-not-applied check is timing-independent (a
# buffered event never reaches the evq), so it is a hard assertion, not a race.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17921}"
DEVDIR=txn-state   # workspace-RELATIVE
DEVLOG=/tmp/txn-dev.log
fail() { echo "TXN FAIL: $1"; exit 1; }
. "$(dirname "$0")/eth-extern-lib.sh"

# EXTERNAL-ENDPOINT MODE (§8.7 over a real network hop): the probe dials the already-running
# external deviced directly — no local spawn. Default loopback path below is untouched.
if eth_extern_active; then
    eth_extern_banner txn
    [ -x "$PROBE" ] || fail "$PROBE not built (needed as the external client)"
    "$PROBE" -d "$(eth_extern_ep)" txn-test || fail "txn-test assertions failed against $(eth_extern_ep)"
    echo "TXN PASS (external §9.6 over the real link: buffer/commit-atomic/abort against $(eth_extern_ep))"
    exit 0
fi

[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT INT TERM
rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

"$PROBE" -d "127.0.0.1:$PORT" txn-test || { cat "$DEVLOG"; fail "txn-test assertions failed"; }
