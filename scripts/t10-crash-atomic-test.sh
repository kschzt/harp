#!/bin/bash
# t10-crash-atomic-test — §17.3 T10 (SIMULATED power-loss-during-apply) over the §8.7 dev
# transport. Proves the §10.2 journaled atomic-apply: a crash mid-commit leaves the live ref
# resolving to EITHER the OLD state OR the fully-NEW state — never a torn/hybrid/partial graph.
#
# WHY A SIGKILL (and what stays rig-only). The physical T10 torture ("cut power 50× during
# state apply", device boots old-or-new) needs the rig. But the storage-atomicity hazard it
# exercises — process death BETWEEN the durable object writes and the atomic ref rename (§10.2)
# — is reproduced fully in the cloud by a SIGKILL -9 of harp-deviced during the state.snapshot
# commit window. SIGKILL models the atomicity/ordering half (did the atomic rename happen or
# not); it does NOT model lost-unfsynced-data (the kernel keeps the page cache across a process
# kill) — that durability half is the physical power-loss torture that remains rig-only.
#
# THE COMMIT WINDOW. state.snapshot writes the params blob, the tree, and the snapshot object
# (each tmp+fsync+rename), THEN advances live/project (§11.2 / state.c do_snapshot). Objects go
# durable BEFORE the ref is advanced, and the ref advance is a single atomic rename — so a crash
# at any instant leaves the ref pointing at a snapshot whose whole closure is already present
# (NEW), or still at the previous snapshot (OLD). To make an EXTERNAL kill reliably land inside
# that microsecond window, the daemon runs with HARP_STORE_COMMIT_DELAY_US set: write_atomic then
# sleeps between each tmp's durable flush and its rename, widening the torn window and logging a
# T10-window-enter/exit marker pair around it (core/src/store.c). A kill during the sleep leaves
# an enter with no exit — counted below as a PROVEN mid-commit kill.
#
# PER ITERATION (N times, the kill nudged across the window by a swept jitter):
#   1. dirty live/project (knob, a distinct value each iter so the snapshot content is new),
#   2. launch the snapshot (the commit) in the BACKGROUND,
#   3. sleep the swept jitter, then SIGKILL -9 the daemon,
#   4. RESTART the daemon on the SAME state-dir and assert recovery:
#        (a) the daemon comes back up (a wedged/invalid store would not),
#        (b) live/project DECODES        — a torn ref record would not (find-ref),
#        (c) its object closure is COMPLETE — a state.refset to the live head runs the device
#            closure walk (snapshot→tree→blob); 'not-found' ⇒ a dangling/hybrid ref ⇒ FAIL,
#        (d) EVERY object file is HASH-CONSISTENT — its bytes hash to its name; a torn object
#            (present at its final path but partial) ⇒ FAIL,
#        (e) the head is EITHER the previous head (OLD) or a new COMPLETE snapshot (NEW).
# A dangling/torn/partial write MUST fail the test. (b)+(c) catch a dangling ref (a referenced
# object missing); (d) catches a torn object (present but wrong bytes); together they reject any
# hybrid graph a broken atomic-apply would leave. See PROVE-IT-BITES at the foot of this file.
#
# Cloud-only, no hardware. Co-exists: unique port, own state-dir, kills only its own pids.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17994}"
SERIAL="${SERIAL:-SIM-T10}"
ITERS="${T10_ITERS:-24}"
DELAY_US="${T10_DELAY_US:-40000}"       # per-write torn-window widen (µs); ~4-5 writes/commit
DEVDIR=t10-crash-atomic-state           # workspace-RELATIVE (matches cas-conflict-eth-test.sh)
TLOG=/tmp/t10-crash-torture-dev.log     # the daemon we SIGKILL mid-commit (widened window)
VLOG=/tmp/t10-crash-verify-dev.log      # the restarted daemon that proves recovery
EP="127.0.0.1:$PORT"
fail() { echo "T10-CRASH-ATOMIC FAIL: $1"; exit 1; }

[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
cleanup() { [ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf "$DEVDIR"; }
trap 'cleanup' EXIT INT TERM
rm -rf "$DEVDIR"

# Start a daemon on the shared state-dir; $1 = per-write commit delay (µs, 0 = none), $2 = log.
# Sets DP to the pid; fails if it does not reach "listening".
start_daemon() {
    : > "$2"
    HARP_STORE_COMMIT_DELAY_US="$1" HARP_RECONCILE_TIMEOUT_MS=0 \
      "$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" --panel-sock "" \
      >>"$2" 2>&1 &
    DP=$!
    for _ in $(seq 1 50); do
        grep -q "listening on $PORT" "$2" 2>/dev/null && return 0
        kill -0 "$DP" 2>/dev/null || return 1   # daemon exited before listening
        sleep 0.1
    done
    return 1
}
stop_daemon() { [ -n "$DP" ] && { kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; }; DP=""; }

# Full head hash of live/project (66 hex), or empty. Parsed from the on-disk ref record so it is
# independent of any daemon and gives the whole hash (find-ref prints a 12-hex prefix only).
read_head() {
    python3 - "$DEVDIR/refs/live/project" <<'PY'
import sys
try:
    b = open(sys.argv[1], "rb").read()
except OSError:
    sys.exit(0)
# ref record = CBOR map {0:name, 1:hash-bytes|null, 2:gen, 3:dirty}. Scan for key 1 (0x01)
# followed by a 33-byte byte string (0x58 0x21 ...) and print it as hex. Minimal, dependency-free.
i = 0
while i < len(b):
    if b[i] == 0x01 and i + 2 < len(b) and b[i+1] == 0x58 and b[i+2] == 0x21:
        h = b[i+3:i+3+33]
        if len(h) == 33:
            print(h.hex())
        break
    i += 1
PY
}

# Assert every object file's bytes hash to its name (01 + sha256), skipping in-flight .tmp.* and
# non-object names. A torn object at its final path fails here. Exits nonzero + prints on the first
# inconsistency. Pure filesystem — needs no daemon.
hash_consistent() {
    python3 - "$DEVDIR/objects" <<'PY'
import sys, os, hashlib
d = sys.argv[1]
bad = 0
try:
    names = os.listdir(d)
except OSError:
    names = []
for name in names:
    if ".tmp." in name or len(name) != 66 or name[:2] != "01":
        continue  # in-flight temporary / not a content-addressed object
    try:
        int(name, 16)
    except ValueError:
        continue
    data = open(os.path.join(d, name), "rb").read()
    want = "01" + hashlib.sha256(data).hexdigest()
    if want != name:
        print("TORN OBJECT %s (bytes hash to %s)" % (name, want))
        bad += 1
sys.exit(1 if bad else 0)
PY
}

echo "── T10 crash-atomicity: $ITERS × SIGKILL mid state.snapshot commit, per-write delay=${DELAY_US}µs"

# ── baseline: born factory state, sane closure + hashes, capture the starting head ──
start_daemon 0 "$VLOG" || { cat "$VLOG"; fail "device did not start (baseline)"; }
"$PROBE" -d "$EP" find-ref live/project >/dev/null 2>&1 || { cat "$VLOG"; fail "baseline live/project did not resolve"; }
"$PROBE" -d "$EP" archive t10base 1 >/dev/null 2>&1     || { cat "$VLOG"; fail "baseline closure incomplete"; }
stop_daemon
hash_consistent || fail "baseline store has a torn object (pre-existing)"
PREV_HEAD="$(read_head)"
[ -n "$PREV_HEAD" ] || fail "could not read baseline head"
echo "   baseline head ${PREV_HEAD:0:12}… closure OK, hashes OK"

OLD=0; NEW=0; MIDCOMMIT=0; KILLED_INFLIGHT=0
for k in $(seq 1 "$ITERS"); do
    # start the daemon we will torture: commit window widened so the kill lands inside it
    start_daemon "$DELAY_US" "$TLOG" || { cat "$TLOG"; fail "torture daemon did not start (iter $k)"; }

    # a real state mutation: a distinct knob value dirties live so the snapshot is genuinely new
    val="0.$(printf '%02d' $((k % 100)))"
    "$PROBE" -d "$EP" knob 1 "$val" >/dev/null 2>&1   # completes (foreground) despite the delay

    # launch the commit (write objects → advance ref) in the background, then cut across it
    "$PROBE" -d "$EP" snapshot "t10 iter $k" >/tmp/t10-snap-$k.out 2>&1 &
    BG=$!

    # sweep the kill across the window: connect+hello then ~5 delayed writes ≈ 5×DELAY_US.
    # A deterministic pseudo-random ramp over [5,355) ms spreads landings pre/mid/post-commit.
    jit_ms=$(( 5 + (k * 37) % 350 ))
    python3 -c "import time;time.sleep($jit_ms/1000.0)"

    kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""
    # reap the backgrounded snapshot (its link died with the daemon — result is irrelevant)
    if ! wait "$BG" 2>/dev/null; then KILLED_INFLIGHT=$((KILLED_INFLIGHT + 1)); fi

    # proven mid-commit kill: a write_atomic entered its rename window and never left it
    en=$(grep -c 'T10-window-enter' "$TLOG" 2>/dev/null); en=${en:-0}
    ex=$(grep -c 'T10-window-exit'  "$TLOG" 2>/dev/null); ex=${ex:-0}
    [ "$en" -gt "$ex" ] && MIDCOMMIT=$((MIDCOMMIT + 1))

    # ── RESTART on the same state-dir and prove recovery (no widen: verify at full speed) ──
    start_daemon 0 "$VLOG" || { cat "$VLOG"; fail "iter $k: daemon did NOT restart on the state-dir after the kill"; }
    "$PROBE" -d "$EP" find-ref live/project >/tmp/t10-fr-$k.out 2>&1 \
        || { cat /tmp/t10-fr-$k.out; cat "$VLOG"; fail "iter $k: live/project did not resolve (torn ref record?)"; }
    "$PROBE" -d "$EP" archive "t10v$k" 1 >/tmp/t10-clo-$k.out 2>&1 \
        || { cat /tmp/t10-clo-$k.out; fail "iter $k: object closure INCOMPLETE — dangling/hybrid ref (head=$(read_head))"; }
    stop_daemon

    hash_consistent || fail "iter $k: a TORN object is present at its final path (hybrid graph)"

    head="$(read_head)"
    [ -n "$head" ] || fail "iter $k: could not read the recovered head"
    if [ "$head" = "$PREV_HEAD" ]; then
        OLD=$((OLD + 1)); tag="OLD"
    else
        NEW=$((NEW + 1)); tag="NEW ${head:0:12}…"; PREV_HEAD="$head"
    fi
    printf "   iter %2d  jit=%3dms  kill→ %-18s closure OK  hashes OK\n" "$k" "$jit_ms" "$tag"
done

rm -f /tmp/t10-snap-*.out /tmp/t10-fr-*.out /tmp/t10-clo-*.out
echo "── recovered: OLD=$OLD  NEW=$NEW  (every one closure-complete + hash-consistent, never hybrid)"
echo "── kills interrupting an in-flight commit: $KILLED_INFLIGHT/$ITERS ; proven mid-rename-window landings: $MIDCOMMIT/$ITERS"

# The torture must actually reach the commit window, or it proves nothing. Require at least one
# kill that landed inside a rename window (widened by the seam) — else the delay/jitter is off.
[ "$MIDCOMMIT" -ge 1 ] || fail "no kill landed in the commit window across $ITERS iters — widen T10_DELAY_US / jitter"

# ── positive control (the NEW half, deterministic): a FULLY-committed snapshot must survive a
# crash byte-for-byte. Let the snapshot complete (foreground → we hold its exact head), THEN
# crash post-ack and restart: the recovered head must equal that committed head EXACTLY, with a
# complete + hash-consistent closure. This is the counterpart to the OLD recoveries above and
# proves the apply is not silently dropped when it DID finish. ──
start_daemon "$DELAY_US" "$TLOG" || { cat "$TLOG"; fail "control: daemon did not start"; }
"$PROBE" -d "$EP" knob 1 0.99 >/dev/null 2>&1
committed=$("$PROBE" -d "$EP" snapshot "t10 committed control" 2>/dev/null | sed -n 's/^snapshot: live\/project @ //p')
[ -n "$committed" ] || { cat "$TLOG"; fail "control: snapshot did not report a committed head"; }
kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""   # crash immediately AFTER the commit acked
start_daemon 0 "$VLOG" || { cat "$VLOG"; fail "control: daemon did not restart after a committed snapshot"; }
"$PROBE" -d "$EP" archive t10ctl 1 >/dev/null 2>&1 || fail "control: committed head closure INCOMPLETE after crash"
stop_daemon
hash_consistent || fail "control: a torn object after a committed snapshot"
recovered="$(read_head)"
[ "$recovered" = "$committed" ] || fail "control: a COMMITTED snapshot was lost by the crash (want ${committed:0:12}… got ${recovered:0:12}…)"
echo "── positive control: a committed snapshot (${committed:0:12}…) survived the crash byte-for-byte as NEW"

echo "T10-CRASH-ATOMIC PASS (§10.2/§17.3-T10 simulated): $ITERS SIGKILLs across the commit window recovered OLD-or-NEW (closure-complete + hash-consistent, never torn/hybrid); a committed snapshot survives a crash byte-for-byte"

# ─────────────────────────────────────────────────────────────────────────────────────────────
# PROVE-IT-BITES (how this test was shown to catch a REGRESSION, not pass vacuously). Each break
# below is a temporary edit to core/src/store.c write_atomic; the UNMODIFIED test caught it, and
# the break was reverted (none ship). Reproduce by re-applying one, `cmake --build build`, re-run.
#
#   1. DANGLING REF — defeat the object rename-atomicity so a committed ref points at a snapshot
#      whose objects never landed. Before the rename, add for object paths:
#          if (strstr(path, "/objects/")) { remove(tmp); return 0; }
#      → the state.refset closure walk reports 'not-found' ⇒ FAIL "object closure INCOMPLETE".
#
#   2. TORN OBJECT — make the object write non-atomic (write straight to the final path, partial):
#          if (strstr(path, "/objects/")) { FILE *o=fopen(path,"wb");
#              if(o){ if(len>1) fwrite(data,1,len-1,o); fclose(o);} remove(tmp); return 0; }
#      → a truncated object fails the CBOR closure parse ('not-found') and, when it still parses,
#        the hash_consistent() check reports "TORN OBJECT …" ⇒ FAIL (both detectors fire).
#
#   The hash_consistent() detector was also verified in isolation: append one byte to any good
#   object file (parseable CBOR, wrong digest) and hash_consistent() reports "TORN OBJECT".
#   On the correct store — atomic rename + objects-durable-before-ref (§10.2) — a SIGKILL can only
#   ever leave OLD or fully-NEW, so all three detectors stay green across every kill above.
# ─────────────────────────────────────────────────────────────────────────────────────────────
