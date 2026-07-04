#!/bin/bash
# offline-fence-eth — §8.3.1 event-fence INTEGRATION over the §8.7 host-paced OFFLINE
# bounce. Companion to offline-golden-eth.sh: that proves the offline bounce is
# DETERMINISTIC; this proves the §8.3.1 EVENT FENCE is actually REACHED and COUNTED on
# that path (the offline barrier is unbounded — it must NEVER time out).
#
# The fence-decision PREDICATE (harp_fence_keep_waiting) is already exhaustively unit-
# tested (tests/harp_engine_logic_tests.c). The gap this closes is INTEGRATION: today the
# normal host (harp-probe render_host_paced) sends pacing with NO fence flag and ZERO
# events, so the device's fence branch (engine.c:873) is never exercised end-to-end. The
# harp-eth-fence-test tool adds a host path that sets HARP_AUDIO_FENCE + the 4-byte `want`
# on a pacing frame and then sends a NOTE on the EVT stream AFTER it, so the render blocks
# on the fence and the device counts the wait.
#
# ASSERTION (Part A — the high-value, low-fragility deliverable):
#   - the tool returns 0 on each run: fence_waits MOVED (>0 delta) AND fence_timeouts==0.
#   - hash(run1) == hash(run2): the late-fenced offline bounce is run-to-run DETERMINISTIC
#     (the fence HELD the render until the event landed; a non-deterministic / racing
#     fence — the bug the barrier exists to prevent — would diverge the two hashes).
# Each run is a FRESH daemon (clean fence counters + clean factory state), so the per-run
# fence_waits-moved check sees a 0 -> >0 transition, not an accumulated count.
#
# POSIX-only: harp-eth-fence-test needs a raw connect-back server socket and isn't built on
# Windows (same gate as latefr; eth-suite.sh enforces it). Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
FENCE="${FENCE:-./build/harp-eth-fence-test}"
PORT="${PORT:-17996}"
DEVLOG=/tmp/offline-fence-eth-dev.log
fail() { echo "OFFLINE-FENCE-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$FENCE" ]   || fail "$FENCE not built (POSIX-only host-paced fence tool)"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT

# run NAME [MODE]: start a fresh-state daemon, run the fence tool once, echo its fence-hash.
# MODE (default empty = release with a note) is passed through to the fence tool: "param"
# releases the fence with a STAGED untagged param-set (the consume-side batching path), proving
# a staged event's deferred g_evt_consumed reaches the fence want via the readable-triggered
# flush — a broken accounting would never reach `want` and the offline barrier would WEDGE.
# Returns the tool's exit code; prints the hash on stdout (last line scraped by caller).
run_one() {
    local name="$1"
    local mode="${2:-}"
    local devdir="offline-fence-eth-$name.$$"
    local out rc
    rm -rf "$devdir"; : > "$DEVLOG"
    "$DEVICED" --port "$PORT" --state-dir "$devdir" --panel-sock "" >>"$DEVLOG" 2>&1 &
    DP=$!
    for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
    grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null || { cat "$DEVLOG" >&2; echo "DEVFAIL"; return 1; }
    out=$("$FENCE" "127.0.0.1:$PORT" $mode)
    rc=$?
    echo "$out" >&2          # the FENCE PASS/FAIL + counter line, to the log
    kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""
    PORT=$((PORT + 1))       # fresh port per run (TIME_WAIT-safe)
    rm -rf "$devdir"
    [ "$rc" = 0 ] || return "$rc"
    printf '%s\n' "$out" | sed -nE 's/^fence-hash: //p'
}

echo "── offline-fence-eth: §8.3.1 event fence reached + counted on the OFFLINE host-paced bounce"

echo "──── run #1"
h1=$(run_one a) || fail "run #1: fence not reached/counted, or offline barrier timed out (see log above)"
echo "──── run #2"
h2=$(run_one b) || fail "run #2: fence not reached/counted, or offline barrier timed out (see log above)"

echo "──── fence-hash #1=${h1:-<none>}  #2=${h2:-<none>}"
[ -n "$h1" ] || fail "run #1 produced no fence-hash"
[ "$h1" = "$h2" ] || fail "late-fenced offline bounce NON-DETERMINISTIC (#1=$h1 #2=$h2) — fence did not hold"

# Consume-side batching: release the fence with a STAGED param-set (not a note). The device
# stages the untagged param, and its deferred g_evt_consumed is published only when the recv
# would block (readable==false) — so this reaching `want` proves the staging + readable-flush +
# deferred consume-accounting release the fence end-to-end (broken -> the barrier WEDGES here).
echo "──── run #3 (param-set release: consume-side batching / staging path)"
h3=$(run_one c param) || fail "run #3: a STAGED param-set did not release the fence — consume-side batching consume-accounting is broken (barrier wedged)"
echo "──── run #4 (param-set release, determinism)"
h4=$(run_one d param) || fail "run #4: staged param-set release failed"
echo "──── param fence-hash #3=${h3:-<none>}  #4=${h4:-<none>}"
[ -n "$h3" ] || fail "run #3 produced no fence-hash"
[ "$h3" = "$h4" ] || fail "staged-param-release offline bounce NON-DETERMINISTIC (#3=$h3 #4=$h4)"

echo "OFFLINE-FENCE-ETH PASS: fence reached + counted (note $h1; staged-param $h3), fence_timeouts==0, both deterministic"
exit 0
