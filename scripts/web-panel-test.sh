#!/bin/bash
# web-panel-test — smoke the §11 web-panel sidecar (web/harp-panel.py): launch the
# REAL panel against a sim harp-deviced and assert its HTTP/JSON endpoints respond
# with the expected shape. The reconcile RELAY is covered elsewhere (reconcile-relay-
# test.sh, a python STUB) — this is the only end-to-end check of harp-panel.py itself
# + the device's panel API. POSIX only (AF_UNIX panel; the Windows daemon stubs it).
# Needs python3 + curl. Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PORT="${PORT:-18099}"   # below every OS ephemeral floor (<32768) — see the eth-suite port renumber (#122)
SOCK="/tmp/web-panel-$$.sock"
DEVLOG=/tmp/web-panel-dev.log
PANLOG=/tmp/web-panel-panel.log
fail() { echo "WEB-PANEL FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "harp-deviced not built"
command -v python3 >/dev/null 2>&1 || { echo "WEB-PANEL SKIP: no python3"; exit 0; }
command -v curl    >/dev/null 2>&1 || { echo "WEB-PANEL SKIP: no curl"; exit 0; }

rm -f "$SOCK"; rm -rf /tmp/web-panel-state
"$DEVICED" --port 18098 --state-dir /tmp/web-panel-state --panel-sock "$SOCK" >"$DEVLOG" 2>&1 & DP=$!
PP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$PP" ] && kill -9 "$PP" 2>/dev/null; rm -f "$SOCK"' EXIT
for _ in $(seq 1 25); do [ -S "$SOCK" ] && break; sleep 0.2; done
[ -S "$SOCK" ] || { cat "$DEVLOG"; fail "device didn't create panel socket"; }

python3 web/harp-panel.py "$SOCK" "$PORT" >"$PANLOG" 2>&1 & PP=$!
for _ in $(seq 1 30); do curl -fsS "http://127.0.0.1:$PORT/api/params" >/dev/null 2>&1 && break; sleep 0.2; done

# check ENDPOINT PYTHON-SHAPE-EXPR : the endpoint must return valid JSON satisfying the expr
check() {
    body=$(curl -fsS "http://127.0.0.1:$PORT/api/$1" 2>/dev/null) \
        || { cat "$PANLOG"; fail "/api/$1 did not respond"; }
    printf '%s' "$body" | python3 -c "import sys,json; d=json.load(sys.stdin); assert ($2)" 2>/dev/null \
        || { printf '%s' "$body" | head -c 200; echo; fail "/api/$1 wrong shape"; }
    echo "   ✓ /api/$1"
}
check params   "isinstance(d['params'], list) and len(d['params']) > 0 and 'id' in d['params'][0]"
check refs     "isinstance(d['refs'], list)"
check counters "'frame_errors' in d and 'evq_drops' in d"
check snapshot "d['ok'] is True"   # the panel's snapshot action -> {ok, hash, gen}

# the HTML page itself
curl -fsS "http://127.0.0.1:$PORT/" 2>/dev/null | grep -qi "<!doctype html" \
    && echo "   ✓ / (panel.html) served" || fail "/ (panel.html) not served"

echo "WEB-PANEL PASS (sidecar serves params/refs/counters/snapshot + the page)"
