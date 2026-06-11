#!/bin/sh
# abuse-test — T9 in miniature: a live daemon must survive hostile link
# traffic with a session reset, never a crash or a hang, and serve the
# next well-behaved client normally.
#
# Runs against the simulator over TCP. Point it at an ASan build
# (build-fuzz/harp-deviced) for teeth:   DEVICED=build-fuzz/harp-deviced
set -e
cd "$(dirname "$0")/.."

DEVICED=${DEVICED:-build/harp-deviced}
PROBE=${PROBE:-build/harp-probe}
PORT=${PORT:-47821}
# ASan builds: this test is about crashes/UB under hostile input, not
# CLI-tool exit leaks (Linux ASan turns leak detection on by default and
# would fail the probe's demo for unrelated reasons)
export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
STATE=$(mktemp -d /tmp/harp-abuse.XXXXXX)
STORE=$(mktemp -d /tmp/harp-abuse-store.XXXXXX)

cleanup() {
    if [ -n "$DPID" ]; then
        kill "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true # reap quietly (no job-control noise)
    fi
    rm -rf "$STATE" "$STORE"
}
trap cleanup EXIT

"$DEVICED" --state-dir "$STATE" --port "$PORT" --panel-sock "" >/dev/null 2>&1 &
DPID=$!
sleep 0.5

echo "── abuse: hostile link traffic against a live daemon (port $PORT)"

python3 - "$PORT" <<'EOF'
import random, socket, struct, sys, time

port = int(sys.argv[1])
random.seed(9)

def connect():
    s = socket.create_connection(("127.0.0.1", port), timeout=2)
    s.settimeout(2)
    return s

def attack(name, payload_fn, rounds):
    for i in range(rounds):
        s = connect()
        try:
            s.sendall(payload_fn(i))
            # the daemon may close on us mid-write; that IS the spec
            # behavior (session reset, §12.4) — only hanging is a failure
            try:
                s.recv(4096)
            except socket.timeout:
                pass
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            s.close()
    print(f"   {name}: {rounds} rounds survived")

def frame(stream, fin, payload):
    # framed link header: fver=1, stream, flags, reserved, length LE32
    return bytes([1, stream, 1 if fin else 0, 0]) + struct.pack("<I", len(payload)) + payload

# 1. pure noise: random bytes, never a valid header
attack("random bytes", lambda i: bytes(random.randrange(256) for _ in range(random.randrange(1, 2048))), 50)

# 2. valid header, garbage CBOR payloads on every stream id
attack("garbage frames", lambda i: frame(i % 8, True,
       bytes(random.randrange(256) for _ in range(random.randrange(0, 512)))), 50)

# 3. length-field lies: huge declared lengths, then EOF (truncation)
attack("truncated oversize", lambda i: frame(0, True, b"")[:4] + struct.pack("<I", 0xFFFFFF) + b"\xa1\x00", 20)

# 4. FIN games: endless unfinished fragments on one stream
def fin_games(i):
    return b"".join(frame(0, False, b"\x00" * 64) for _ in range(64))
attack("never-FIN fragment flood", fin_games, 10)

# 5. valid envelope, hostile body: rid/method ok, body is noise
def evil_body(i):
    body = bytes(random.randrange(256) for _ in range(64))
    # envelope: {0: msgtype 0, 1: rid, 2: "core.hello", 3: <noise>} — hand-rolled
    env = b"\xa4\x00\x00\x01\x01\x02\x6a" + b"core.hello" + b"\x03" + body
    return frame(0, True, env)
attack("hostile bodies", evil_body, 50)

# 6. half-close mid-frame
def half(i):
    f = frame(0, True, b"\xa1" * 100)
    return f[: random.randrange(1, len(f))]
attack("mid-frame hangup", half, 50)
EOF

# the daemon must still be alive...
kill -0 "$DPID" || { echo "ABUSE FAIL: daemon died"; exit 1; }

# ...and still serve a well-behaved client end-to-end
if ! "$PROBE" -d 127.0.0.1:"$PORT" -s "$STORE" demo >/tmp/abuse-demo.log 2>&1; then
    echo "ABUSE FAIL: daemon unresponsive after abuse"
    tail -15 /tmp/abuse-demo.log
    exit 1
fi

RESETS=$("$PROBE" -d 127.0.0.1:"$PORT" -s "$STORE" counters 2>/dev/null \
    | grep -E 'session_resets|frame_errors' | tr -d ' ' || true)
echo "   post-abuse recall demo: PASS"
echo "   counters: $RESETS"
echo "ABUSE PASS (daemon survived; sessions reset, never crashed)"
