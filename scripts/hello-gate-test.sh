#!/bin/bash
# hello-gate-test — §5.4: core.hello MUST complete before any other method. A WELL-FORMED
# non-hello request sent BEFORE core.hello is rejected with error "denied" ("core.hello
# required first"), not processed. Guards device/session.c (the !hello_done gate). The
# abuse-test only sends a hello-with-noise (which the gate permits), so this pre-hello
# rejection was the missing assertion (2026-06-23 audit, §5.4 test gap).
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PORT="${PORT:-47823}"
fail() { echo "HELLO-GATE FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
command -v python3 >/dev/null 2>&1 || { echo "HELLO-GATE SKIP: python3 not available"; exit 0; }

rm -rf hello-gate-state; : > /tmp/hello-gate-dev.log
"$DEVICED" --port "$PORT" --serial SIM-HELLOGATE --state-dir hello-gate-state --panel-sock "" >/tmp/hello-gate-dev.log 2>&1 & DP=$!
trap 'kill -9 "$DP" 2>/dev/null; rm -rf hello-gate-state' EXIT INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/hello-gate-dev.log 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" /tmp/hello-gate-dev.log || { cat /tmp/hello-gate-dev.log; fail "device didn't start on $PORT"; }

python3 - "$PORT" <<'EOF'
import socket, struct, sys
port = int(sys.argv[1])
def frame(stream, payload):  # framed link: fver=1, stream, flags(FIN=1), reserved, len LE32
    return bytes([1, stream, 1, 0]) + struct.pack("<I", len(payload)) + payload
# a WELL-FORMED request for a NON-hello method, with NO core.hello first:
# envelope {0: msgtype=0 (request), 1: rid=1, 2: method="core.ping", 3: {}}
env = b"\xa4\x00\x00\x01\x01\x02" + bytes([0x60 + 9]) + b"core.ping" + b"\x03\xa0"
s = socket.create_connection(("127.0.0.1", port), timeout=3); s.settimeout(2)
s.sendall(frame(0, env))     # stream 0 = ctl
# accumulate: the framed error reply can arrive in a LATER TCP segment (seen on Linux CI —
# a single recv caught only the 8-byte frame header), so read until the code shows, the peer
# closes, or we time out.
resp = b""
while b"denied" not in resp:
    try:
        chunk = s.recv(4096)
    except socket.timeout:
        break
    if not chunk:
        break
    resp += chunk
s.close()
if b"denied" not in resp:
    sys.stderr.write("pre-hello response (no 'denied'): %r\n" % resp[:160])
    sys.exit("a pre-hello core.ping was NOT rejected with 'denied'")
print("   pre-hello core.ping -> rejected with 'denied' (the gate holds)")
EOF
[ $? -eq 0 ] || fail "pre-hello request was not denied (§5.4 gate)"
echo "HELLO-GATE PASS (§5.4: a well-formed pre-hello request is rejected with 'denied')"
