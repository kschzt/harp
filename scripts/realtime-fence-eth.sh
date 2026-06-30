#!/bin/bash
# realtime-fence-eth — §8.3.1 event-fence INTEGRATION for the REAL-TIME host-paced path.
# Companion to offline-fence-eth.sh: that proves the OFFLINE bounce's fence is an UNBOUNDED
# barrier (never times out); this proves the REAL-TIME fence is BOUNDED and COUNTED — a host
# that fences beyond what it feeds must NOT wedge the stream (§8.3.1 line 522).
#
# Why this exists: the bounded/offline decision PREDICATES (harp_fence_keep_waiting /
# harp_fence_count_timeout, device/fence_wait.h) are unit-tested, but for a long time the
# production host_paced_loop did NOT call them — it ran an UNBOUNDED wait unconditionally, so a
# real-time host-paced stream that fenced ahead of its feed wedged until session teardown and
# fence_timeouts never counted a real deadline. The unit test passed (it exercised the helper,
# not the loop). This integration test closes that test/production gap on the real-time side.
#
# Mechanism: the daemon runs with HARP_FENCE_FORCE_RT=1 so its host-paced stream is
# offline=false (the fence reads a->offline, not the transport, so this faithfully selects the
# bounded regime over the TCP test carrier a USB host-paced stream would use). eth-fence-test
# in `realtime` mode fences want=1 and then sends NO event, so the bound (a few ms) is the only
# thing that can release the render. PASS = the fenced range still rendered (no wedge) AND
# fence_timeouts moved (>0). A regression to an unbounded fence would hang drain_output → the
# tool's read blocks → the daemon never returns the frame → FAIL (caught by the perl alarm).
#
# POSIX-only: eth-fence-test needs a raw connect-back server socket, not built on Windows
# (same gate as offline-fence-eth / latefr; eth-suite.sh enforces it). Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
FENCE="${FENCE:-./build/harp-eth-fence-test}"
PORT="${PORT:-47997}"
DEVLOG=/tmp/realtime-fence-eth-dev.log
fail() { echo "REALTIME-FENCE-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$FENCE" ]   || fail "$FENCE not built (POSIX-only host-paced fence tool)"

DP=""
# graceful stop (SIGTERM + reap) so the listen socket is released cleanly (no TIME_WAIT churn)
trap '[ -n "$DP" ] && { kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""; }' EXIT

devdir="realtime-fence-eth-state.$$"
rm -rf "$devdir"; : > "$DEVLOG"
# HARP_FENCE_FORCE_RT=1 -> the host-paced stream runs the REAL-TIME bounded fence path
HARP_FENCE_FORCE_RT=1 "$DEVICED" --port "$PORT" --state-dir "$devdir" --panel-sock "" >>"$DEVLOG" 2>&1 &
DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

echo "── realtime-fence-eth: §8.3.1 event fence BOUNDED + counted on the real-time host-paced path"
# 20s alarm: a regression to an unbounded fence wedges drain_output; the alarm turns that hang
# into a deterministic failure instead of a stuck CI job.
out=$(perl -e 'alarm 20; exec @ARGV' "$FENCE" "127.0.0.1:$PORT" realtime); rc=$?
echo "$out"
rm -rf "$devdir"
[ "$rc" = 0 ] || fail "real-time fence did not bound+count (rc=$rc — wedge or missing timeout; see log)"

echo "REALTIME-FENCE-ETH PASS: real-time fence bounded + counted, no wedge"
exit 0
