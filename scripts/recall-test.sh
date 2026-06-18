#!/bin/bash
# Shell recall regression: getState/setState round-trip through the plugin,
# with device-side mutation in between — the DAW-project-wins semantic.
#   1. set known params via the shell, save plugin state (DAW save)
#   2. mutate the device via the front panel (the "musician")
#   3. load plugin state (DAW reopen) -> auto-Push-with-archive
#   4. assert params restored AND an archive ref captured the mutation
# usage: scripts/recall-test.sh   (device must be unclaimed)
set -u
# This test asserts the INTERACTIVE recall path archives the displaced device state
# before the Push (step 4). The §11.4 archive is skipped in headless mode
# (HARP_RECONCILE_TIMEOUT_MS=0) — which the conformance suite sets globally to keep
# the real-time audio tests from stalling on a panel pick. So opt OUT of headless
# here: a small non-zero timeout takes the interactive archive-then-CAS path (no
# panel answers, so it falls back to Push after a brief wait — fast, and it archives).
export HARP_RECONCILE_TIMEOUT_MS=1000
HOST=${HARP_HOST:-harp.local}
VST=${VST:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}
HOSTBIN=${HOSTBIN:-./build-vst/harp-vst3-host}
fail() { echo "RECALL FAIL: $1"; exit 1; }

"$HOSTBIN" "$VST" --seconds 0.05 2>&1 | grep -q "connected:" \
    || { echo "RECALL FAIL: cannot claim device — the rig must own it exclusively (busy/absent?)"; exit 3; }

ARCH0=$(curl -s "http://$HOST:8080/api/refs" | python3 -c \
    "import json,sys; print(sum(1 for r in json.load(sys.stdin)['refs'] if r['name'].startswith('archive/')))")

# 1. known state, saved
"$HOSTBIN" "$VST" --set 3=0.81 --set 7=0.31 --set 1=0.61 --seconds 0.6 \
    --save-state /tmp/harp-recall.state > /dev/null 2>&1 || fail "save render"

# 2. musician mutates
curl -s "http://$HOST:8080/api/knob?id=3&value=0.10" > /dev/null
curl -s "http://$HOST:8080/api/knob?id=7&value=0.95" > /dev/null

# 3. DAW reopen
"$HOSTBIN" "$VST" --load-state /tmp/harp-recall.state --seconds 0.6 > /tmp/harp-recall.log 2>&1 \
    || fail "load render"
grep -q "restored\|SYNCED" /tmp/harp-recall.log || fail "no recall action logged"

# 4. params back + archive grew
curl -s "http://$HOST:8080/api/params" | python3 -c "
import json, sys
p = {x['id']: x['value'] for x in json.load(sys.stdin)['params']}
ok = abs(p[3]-0.81) < 0.02 and abs(p[7]-0.31) < 0.02 and abs(p[1]-0.61) < 0.02
sys.exit(0 if ok else (print(f'RECALL FAIL: params not restored: {p}') or 1))" || exit 1
ARCH1=$(curl -s "http://$HOST:8080/api/refs" | python3 -c \
    "import json,sys; print(sum(1 for r in json.load(sys.stdin)['refs'] if r['name'].startswith('archive/')))")
[ "$ARCH1" -gt "$ARCH0" ] || fail "mutation was not archived before push"
echo "RECALL PASS (params restored, mutation archived: $ARCH0 -> $ARCH1 archives)"
