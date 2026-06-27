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

# §10.3 GC caps the retained archive count, so a NEW archive can evict an old one and leave the
# COUNT unchanged. Snapshot the archive ref NAMES (sorted) and assert a NEW name appears on reopen.
arch_names() { curl -s "http://$HOST:8080/api/refs" | python3 -c \
    "import json,sys; [print(r['name']) for r in json.load(sys.stdin).get('refs',[]) if r['name'].startswith('archive/')]" | sort; }
arch_names > /tmp/harp-arch0.txt

# 1. known state, saved
"$HOSTBIN" "$VST" --set 3=0.81 --set 6=0.31 --set 1=0.61 --seconds 0.6 \
    --save-state /tmp/harp-recall.state > /dev/null 2>&1 || fail "save render"

# 2. musician mutates
curl -s "http://$HOST:8080/api/knob?id=3&value=0.10" > /dev/null
curl -s "http://$HOST:8080/api/knob?id=6&value=0.95" > /dev/null

# 3. DAW reopen. The render MUST outlast the reconcile window: with HARP_RECONCILE_TIMEOUT_MS
# above (1000 ms) the device holds divergent state, so pushStateLocked posts a panel offer and
# POLLS up to that timeout before the no-pick Push fallback fires (runtime.cpp:2977-3001). The
# reconcile runs async on the supervisor thread while this host renders; a 0.6 s render could
# begin shutdown mid-poll, before the Push applied params 3/6 — a real race (lost ~2/3 on a
# loaded rig, won otherwise). 2.0 s comfortably exceeds the 1.0 s poll + the archive-then-CAS
# push + the device reflecting it, so the restore has always completed before step 4 reads it.
"$HOSTBIN" "$VST" --load-state /tmp/harp-recall.state --seconds 2.0 > /tmp/harp-recall.log 2>&1 \
    || fail "load render"
grep -q "restored\|SYNCED" /tmp/harp-recall.log || fail "no recall action logged"

# 4. params back + archive grew
curl -s "http://$HOST:8080/api/params" | python3 -c "
import json, sys
p = {x['id']: x['value'] for x in json.load(sys.stdin)['params']}
ok = abs(p[3]-0.81) < 0.02 and abs(p[6]-0.31) < 0.02 and abs(p[1]-0.61) < 0.02
sys.exit(0 if ok else (print(f'RECALL FAIL: params not restored: {p}') or 1))" || exit 1
arch_names > /tmp/harp-arch1.txt
# A NEW archive name (not in the before-set) = the displaced state was archived. Robust to the §10.3
# GC cap, which can evict an old archive on the same push so the bare COUNT stays flat (the old bug).
NEW=$(comm -13 /tmp/harp-arch0.txt /tmp/harp-arch1.txt)
[ -n "$NEW" ] || fail "mutation was not archived before push (no new archive ref appeared)"
echo "RECALL PASS (params restored; mutation archived as $(echo "$NEW" | head -1))"
