#!/bin/sh
# conn-flood-test — §16 DoS: a single-threaded daemon must NOT hang on a HALF-OPEN connection
# (connect, never send core.hello) and must survive a connect storm, still serving the next
# well-behaved client. The pre-hello SO_RCVTIMEO drops a half-open (~5s) and a 10s total
# deadline bounds a slow trickle; a connect storm is handled (each connection accepted and
# closed fast, bounded per-connection) without tying up the single session slot.
#
# Why this exists: scripts/abuse-test.sh connects SEQUENTIALLY and its "hostile body" attack
# sends a core.hello (which the gate permits), so it never exercised a held half-open — before
# the fix a single one blocked the single-threaded accept loop FOREVER (no other client could
# connect). The empty half-opens below would hang the recv() indefinitely on an unfixed daemon.
set -e
cd "$(dirname "$0")/.."

DEVICED=${DEVICED:-build/harp-deviced}
PROBE=${PROBE:-build/harp-probe}
PORT=${PORT:-47833}
export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
STATE=conn-flood-state    # workspace-RELATIVE: Git Bash /tmp->C:\ trips the MinGW device's --state-dir
STORE=conn-flood-store    # mkdir (and harp-probe's -s store) on Windows (see eth-tests.sh). A shell
DEVLOG=/tmp/harp-flood-dev.log  # redirect to /tmp (DEVLOG) is fine; a device/probe mkdir there is not.

cleanup() {
    [ -n "$DPID" ] && { kill "$DPID" 2>/dev/null || true; wait "$DPID" 2>/dev/null || true; }
    rm -rf "$STATE" "$STORE"
}
trap cleanup EXIT

command -v python3 >/dev/null 2>&1 || { echo "CONN-FLOOD SKIP: python3 not available"; exit 0; }
[ -x "$DEVICED" ] || { echo "CONN-FLOOD FAIL: $DEVICED not built"; exit 1; }
[ -x "$PROBE" ]   || { echo "CONN-FLOOD FAIL: $PROBE not built"; exit 1; }

rm -rf "$STATE" "$STORE"; : > "$DEVLOG"
"$DEVICED" --state-dir "$STATE" --port "$PORT" --panel-sock "" >"$DEVLOG" 2>&1 & DPID=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; echo "CONN-FLOOD FAIL: device didn't start"; exit 1; }

echo "── §16: half-opens must be DROPPED (~5s) + a connect storm must not crash the daemon (port $PORT)"
python3 - "$PORT" <<'EOF' || { echo "CONN-FLOOD FAIL: a half-open was NOT dropped — daemon hung (no pre-hello timeout)"; tail -20 "$DEVLOG"; exit 1; }
import socket, sys, time, threading
port = int(sys.argv[1])

# (a) sequential half-opens — each MUST be dropped within ~the 5s pre-hello timeout, proving the
#     daemon reliably reclaims its single session slot instead of blocking forever. recv() returns
#     b'' (EOF) when the daemon closes its side; on an UNFIXED daemon it blocks until our 15s cap.
for i in range(3):
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    if i == 1:
        s.sendall(bytes([1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff]))  # full frame header claiming a payload, then stall
    t0 = time.time(); s.settimeout(15)
    try:
        got = s.recv(64)
    except socket.timeout:
        got = b'__TIMEOUT__'
    dt = time.time() - t0; s.close()
    if got == b'__TIMEOUT__':
        sys.exit("half-open %d NOT dropped within 15s — the daemon hung on it" % i)
    if dt > 8.0:
        sys.exit("half-open %d took %.1fs to drop (expected ~5s pre-hello timeout)" % (i, dt))
    print("   half-open %d dropped after %.1fs" % (i, dt))

# (b) a connect storm: ~300 concurrent connect/close — the daemon must survive a flood
#     (no crash, no spin) and still serve the next client (below).
def burst():
    for _ in range(60):
        try:
            c = socket.create_connection(("127.0.0.1", port), timeout=2); c.close()
        except OSError:
            pass
ts = [threading.Thread(target=burst) for _ in range(5)]
for t in ts: t.start()
for t in ts: t.join()
print("   connect storm: ~300 connect/close completed")
EOF

kill -0 "$DPID" 2>/dev/null || { echo "CONN-FLOOD FAIL: daemon died under the flood"; tail -20 "$DEVLOG"; exit 1; }

echo "── post-flood: a well-behaved client must still be served end-to-end"
perl -e 'alarm 30; exec @ARGV' "$PROBE" -d 127.0.0.1:"$PORT" -s "$STORE" demo >/tmp/harp-flood-demo.log 2>&1 \
  || { echo "CONN-FLOOD FAIL: daemon unresponsive / hung after the flood"; tail -20 /tmp/harp-flood-demo.log; exit 1; }
kill -0 "$DPID" 2>/dev/null || { echo "CONN-FLOOD FAIL: daemon died serving the demo"; exit 1; }

RESETS=$("$PROBE" -d 127.0.0.1:"$PORT" -s "$STORE" counters 2>/dev/null | grep -E 'session_resets' | tr -d ' ' || true)
echo "   post-flood recall demo: PASS   ${RESETS:-}"
echo "CONN-FLOOD PASS (§16: half-opens dropped via the pre-hello timeout; survived a ~300-connect storm; served the next client)"
