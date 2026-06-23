#!/bin/bash
# gc-test — §10.3/§11.4 store hygiene end-to-end (debt #22a). Proves the WIRED device path
# over the real wire (no fabricated on-disk refs):
#   (1) archive retention prunes the archive/ namespace to the newest HARP_ARCHIVE_KEEP=32,
#       oldest-first lexicographically — driven automatically by each archive-class refset;
#   (2) the mark-sweep reclaims now-unreachable objects — crucially the PARENT history of a
#       superseded snapshot (GC marks include_parents=false, matching the recall closure); and
#   (3) the live ref + its closure survive and still resolve after the sweep.
# The GC mechanism itself is exhaustively unit-tested (tests/harp_tests.c test_store_gc: incl.
# fail-closed, bounded-sweep, crash-resume); this asserts the DEVICE actually invokes it.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-47924}"
DEVDIR=gc-state          # workspace-RELATIVE
DEVLOG=/tmp/gc-dev.log
KEEP=32                  # must match HARP_ARCHIVE_KEEP
N=40                     # archives to create (> KEEP so retention prunes N-KEEP=8)
D="127.0.0.1:$PORT"
fail() { echo "GC FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT INT TERM
rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

objs() { find "$DEVDIR/objects" -type f 2>/dev/null | wc -l | tr -d ' '; }
arcs() { find "$DEVDIR/refs/archive" -type f 2>/dev/null | wc -l | tr -d ' '; }

# Two distinct saves: the FIRST snapshot becomes superseded history. snapB references snapA as
# its parent, but GC's closure excludes parents — so snapA's unique objects are reclaimable.
"$PROBE" -d "$D" knob 3 0.1 >/dev/null 2>&1 || fail "knob A failed"
"$PROBE" -d "$D" save        >/dev/null 2>&1 || fail "save A failed"
"$PROBE" -d "$D" knob 3 0.9 >/dev/null 2>&1 || fail "knob B failed"
"$PROBE" -d "$D" save        >/dev/null 2>&1 || fail "save B failed"
"$PROBE" -d "$D" find-ref live/project >/dev/null 2>&1 || fail "live ref never resolved"
objs_baseline=$(objs)   # snapA + snapB closures, no archives yet

# Create N real archive refs over the wire (correct stored names). Each is an archive-class
# refset, so the device prunes to KEEP as they accrue and ticks the GC interval automatically.
"$PROBE" -d "$D" archive aa "$N" >/tmp/gc-arch.log 2>&1 || { cat /tmp/gc-arch.log; fail "archive failed"; }

# (1) retention happened automatically (no forced GC yet): newest KEEP kept, oldest pruned
[ "$(arcs)" -eq "$KEEP" ] || fail "auto-retention: expected $KEEP archives, have $(arcs)"
[ -f "$DEVDIR/refs/archive/aa0040" ] || fail "retention: newest archive aa0040 was pruned"
[ -f "$DEVDIR/refs/archive/aa0009" ] || fail "retention: boundary archive aa0009 was pruned"
[ ! -f "$DEVDIR/refs/archive/aa0008" ] || fail "retention: stale archive aa0008 survived"
[ ! -f "$DEVDIR/refs/archive/aa0001" ] || fail "retention: stale archive aa0001 survived"

# Force a full retention+GC cycle and read back what it reclaimed / what remains.
"$PROBE" -d "$D" gc >/tmp/gc.log 2>&1 || { cat /tmp/gc.log; fail "gc command failed"; }
remaining=$(grep -oE '[0-9]+ remaining' /tmp/gc.log | grep -oE '[0-9]+')
[ -n "$remaining" ] || { cat /tmp/gc.log; fail "could not parse gc output"; }

# (2) reclamation: snapA's orphaned parent history is gone — fewer objects than the pre-archive
#     baseline — and the device's reported count matches what's on disk.
[ "$remaining" -lt "$objs_baseline" ] || fail "orphan parent history not reclaimed ($objs_baseline -> $remaining)"
[ "$(objs)" -eq "$remaining" ] || fail "reported remaining ($remaining) != on-disk objects ($(objs))"
[ "$(arcs)" -eq "$KEEP" ] || fail "retention drifted after forced GC: $(arcs) archives"

# (3) survival: the live closure is intact — live still resolves after the sweep
"$PROBE" -d "$D" find-ref live/project >/tmp/gc-find2.log 2>&1 \
    || { cat /tmp/gc-find2.log; fail "find-ref live/project failed AFTER GC (closure was swept!)"; }

echo "GC PASS: retention $N->$KEEP archives, orphan reclaimed ($objs_baseline->$remaining objects), live resolves"
kill -9 "$DP" 2>/dev/null; DP=""
rm -rf "$DEVDIR"
