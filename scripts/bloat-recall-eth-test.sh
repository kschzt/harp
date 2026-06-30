#!/bin/bash
# bloat-recall-eth-test — regression for debt #22 (the recall-breaker). A device whose state
# store has accumulated many §11.4 archive refs MUST still resolve the live ref — the path
# refsLocked / getState / recall take on every save. The host filters state.refs to the single
# named ref instead of pulling the WHOLE list, which past ~1,300 refs exceeds the §4.2.1 ctl
# message bound and fails the recv (which is exactly how the hw rig's recall/session-share
# broke). We fabricate the bloat on disk (valid copies of the live ref) so it is fast and
# deterministic — no thousands of real recalls needed.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17923}"
DEVDIR=bloat-state   # workspace-RELATIVE
DEVLOG=/tmp/bloat-dev.log
N="${BLOAT_N:-1500}" # comfortably past the ~1,300-ref ctl-bound threshold
fail() { echo "BLOAT-RECALL FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT INT TERM
launch() {
    "$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
    for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done
    cat "$DEVLOG"; fail "device didn't start on $PORT"
}
stopdev() { [ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; DP=""; sleep 0.3; }

rm -rf "$DEVDIR"; : > "$DEVLOG"
launch
# create the live ref: dirty a param, then snapshot it
"$PROBE" -d "127.0.0.1:$PORT" knob 3 0.5 >/dev/null 2>&1 || fail "knob failed"
"$PROBE" -d "127.0.0.1:$PORT" save     >/dev/null 2>&1 || fail "save (snapshot) failed"
stopdev
[ -f "$DEVDIR/refs/live/project" ] || fail "no live ref was created"

# fabricate N archive refs (each a valid copy of the live ref record)
mkdir -p "$DEVDIR/refs/archive"
for i in $(seq 1 "$N"); do cp "$DEVDIR/refs/live/project" "$DEVDIR/refs/archive/a$i"; done
total=$(find "$DEVDIR/refs" -type f | wc -l | tr -d ' ')

launch
# THE regression: resolve the live ref on the bloated store via the filtered read. An
# UNFILTERED full-list read fails here (response > 64 KiB ctl bound); the filtered single-ref
# read must succeed.
"$PROBE" -d "127.0.0.1:$PORT" find-ref live/project >/tmp/bloat-find.log 2>&1 \
    || { cat /tmp/bloat-find.log; fail "find-ref live/project failed on a $total-ref store (the debt #22 recall-breaker)"; }
echo "BLOAT-RECALL PASS: live ref resolves on a $total-ref store ($(grep -oE '@ [0-9a-f]+' /tmp/bloat-find.log | head -1))"
stopdev
rm -rf "$DEVDIR"
