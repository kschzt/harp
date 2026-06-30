#!/bin/sh
# reconcile-relay-test — the §11.4 reconcile relay over the PROTOCOL (Phase-2B layer
# 1): a shell POSTs a conflict offer (x.harp.reconcile.offer), a panel frontend SEES
# it (panel verb reconcile-get) and records the user's pick (reconcile-choose), and
# the shell READs the pick back (x.harp.reconcile.poll). Proves the device relays
# both directions between the protocol and the front panel — the plumbing the shell's
# recall flow (the four §11.4 actions) sits on. Simulated device over TCP + a panel
# socket, no hardware. Exit 0 pass / 1 fail / 2 N/A (binaries absent).
set -e
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-build/harp-deviced}"
PROBE="${PROBE:-build/harp-probe}"
PORT="${PORT:-17811}"
SOCK="/tmp/recrelay-panel.sock"
STATE="/tmp/recrelay-dev"
HOST="/tmp/recrelay-host"

[ -x "$DEVICED" ] && [ -x "$PROBE" ] || { echo "RECONCILE-RELAY SKIP: build $DEVICED + $PROBE first"; exit 2; }

pkill -f "harp-deviced.*$PORT" 2>/dev/null || true
sleep 0.3
rm -rf "$STATE" "$HOST" "$SOCK"
mkdir -p "$HOST"
"$DEVICED" --state-dir "$STATE" --panel-sock "$SOCK" --port "$PORT" >/tmp/recrelay-dev.log 2>&1 &
DPID=$!
trap 'kill $DPID 2>/dev/null' EXIT
sleep 1.2

panel() {  # send one panel verb, echo the JSON reply
    python3 -c "
import socket,sys
s=socket.socket(socket.AF_UNIX); s.connect('$SOCK')
s.sendall((sys.argv[1]+'\n').encode()); sys.stdout.write(s.recv(4096).decode())" "$1"
}

# 1. the shell posts a conflict offer over the protocol
"$PROBE" -d 127.0.0.1:$PORT -s "$HOST" reconcile-offer abc123 def456 1 >/dev/null
# 2. the panel must SEE it (protocol -> shared mailbox -> panel)
got=$(panel "reconcile-get")
{ echo "$got" | grep -q '"pending":true' && echo "$got" | grep -q '"expect":"abc123"' \
  && echo "$got" | grep -q '"live":"def456"' && echo "$got" | grep -q '"dirty":true'; } \
    || { echo "RECONCILE-RELAY FAIL: panel did not see the posted offer: $got"; exit 1; }
# 3. the shell polls: an offer is pending, the user has not chosen yet
p1=$("$PROBE" -d 127.0.0.1:$PORT -s "$HOST" reconcile-poll)
{ echo "$p1" | grep -q "pending=true" && echo "$p1" | grep -q "choice=-1"; } \
    || { echo "RECONCILE-RELAY FAIL: poll before the pick is wrong: $p1"; exit 1; }
# 4. the panel records the user's pick — 2 (read-only), a non-default so we prove the
#    actual value carries, not just "something non-negative"
panel "reconcile-choose 2" >/dev/null
# 5. the shell reads the pick back over the protocol
p2=$("$PROBE" -d 127.0.0.1:$PORT -s "$HOST" reconcile-poll)
{ echo "$p2" | grep -q "pending=false" && echo "$p2" | grep -q "choice=2"; } \
    || { echo "RECONCILE-RELAY FAIL: poll after the pick is wrong: $p2"; exit 1; }

echo "RECONCILE-RELAY PASS: offer posted over protocol -> seen on panel -> pick"
echo "   recorded -> read back over protocol (choice 2 = read-only carried intact)"
