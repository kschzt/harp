#!/bin/bash
# param-map-recall-test — §13.4 / §9.3 recall-validity: a project saved against one
# engine param-map, loaded against a device whose param-map-hash has DRIFTED (an engine
# update changed the automatable param set), must WARN ("applying matching ids only" —
# warn-and-map-conservatively), NOT silently load a mismatched map onto the user's old
# song. The shell detects the drift by comparing the bundle's identity-expectation hash
# to the connected device's param-map-hash (shell/runtime.cpp); the warning goes to
# stderr ("harp-shell: recall: project's param map differs ...").
#
# The drift is forced deterministically with the device --param-map-hash-flip test seam
# (advertises a 1-bit-altered hash — no 2nd board, no CBOR byte-surgery). Probe-free,
# over the §8.7 loopback. A NEGATIVE CONTROL (same bundle vs the REAL map) asserts the
# gate stays silent on a match, so it can't pass by always-warning. Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17948}"
STATEDIR=pmh-eth-state   # workspace-RELATIVE (Windows path-converts an absolute /tmp arg)
STATEFILE=pmh-eth.state
DEVLOG=/tmp/pmh-eth-dev.log
fail() { echo "PARAM-MAP-RECALL FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"
export HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001"

DP=""
start_dev() {  # start_dev [extra device args]
    : > "$DEVLOG"
    "$DEVICED" --port "$PORT" --state-dir "$STATEDIR" "$@" >>"$DEVLOG" 2>&1 & DP=$!
    for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
    grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }
}
stop_dev() { [ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; DP=""; sleep 0.3; }
trap 'stop_dev' EXIT
load_check() { grep -q "connected:" "$1" || { cat "$1"; fail "host never connected ($1) — comparison didn't run"; }; }

rm -rf "$STATEDIR" "$STATEFILE"

# 1. device with the REAL param map: set known params + save a recall bundle (records the hash)
start_dev
"$HOSTBIN" "$PLUG" --set 3=0.81 --set 6=0.31 --seconds 0.6 --save-state "$STATEFILE" >/tmp/pmh-save.log 2>&1 \
    || { cat /tmp/pmh-save.log; fail "save render"; }
load_check /tmp/pmh-save.log
stop_dev

# 2. device with a DRIFTED param-map-hash (engine-update sim): loading the bundle MUST warn
start_dev --param-map-hash-flip
"$HOSTBIN" "$PLUG" --load-state "$STATEFILE" --seconds 0.6 >/tmp/pmh-load-drift.log 2>&1 \
    || { cat /tmp/pmh-load-drift.log; fail "load render (drift)"; }
load_check /tmp/pmh-load-drift.log
stop_dev
grep -qi "param map differs" /tmp/pmh-load-drift.log \
    && echo "   ✓ drift: recall WARNED on the param-map mismatch (§9.3 warn-and-map-conservatively)" \
    || { cat /tmp/pmh-load-drift.log; fail "no drift warning when the device's param map changed — §13.4 gate is SILENT (old projects load onto a mismatched map unnoticed)"; }

# 3. NEGATIVE CONTROL: the SAME bundle against the REAL (unchanged) map must NOT warn
start_dev
"$HOSTBIN" "$PLUG" --load-state "$STATEFILE" --seconds 0.6 >/tmp/pmh-load-match.log 2>&1 \
    || { cat /tmp/pmh-load-match.log; fail "load render (match)"; }
load_check /tmp/pmh-load-match.log
stop_dev
grep -qi "param map differs" /tmp/pmh-load-match.log \
    && { cat /tmp/pmh-load-match.log; fail "FALSE drift warning when the param map MATCHED — the gate cries wolf"; } \
    || echo "   ✓ match: no warning when the param map is unchanged (negative control)"

echo "PARAM-MAP-RECALL PASS (§13.4: warns on param-map drift, silent on match)"
