#!/bin/bash
# mdns-discover-test — §4.4.3 `_harp._tcp` discovery round-trip (advertise -> discover -> dial).
#
# The device advertises `_harp._tcp` via the platform mDNS responder (--mdns); harp-probe
# `discover` browses the segment, resolves the instance to host:port, and reads the TXT
# `proto`. Asserts: the device IS discovered, the TXT carries proto but NO serial (§16
# privacy — identity comes via core.hello), and the discovered host:port actually dials to
# the live device. Mirrors the KR260 bench proof (avahi over the direct cable).
#
# mDNS needs a running responder (mDNSResponder/avahi) AND harp-probe built with dns_sd, so
# eth-suite runs this only where that holds (macOS, where mDNSResponder is always up) and
# SKIPS elsewhere; the script self-skips if `discover` is the no-dns_sd stub.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-47809}"
STATEDIR=mdns-disc-state   # workspace-RELATIVE
SERIAL=SIM-MDNS
fail() { echo "MDNS-DISC FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

# capability gate: `discover` returns the stub message when harp-probe lacks dns_sd
"$PROBE" discover >/tmp/mdns-cap.log 2>&1
if grep -q "without dns_sd" /tmp/mdns-cap.log 2>/dev/null; then
  echo "MDNS-DISC SKIP: harp-probe built without dns_sd — no mDNS on this host"; exit 0
fi

rm -rf "$STATEDIR"; : > /tmp/mdns-disc-dev.log
"$DEVICED" --port "$PORT" --mdns --serial "$SERIAL" --state-dir "$STATEDIR" >/tmp/mdns-disc-dev.log 2>&1 & DP=$!
trap 'kill "$DP" 2>/dev/null' EXIT INT TERM  # SIGTERM (not -9) so on_term kills the mDNS
                                             # advertiser child + emits the goodbye (no orphan)
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/mdns-disc-dev.log 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" /tmp/mdns-disc-dev.log || { cat /tmp/mdns-disc-dev.log; fail "device didn't start on $PORT"; }
sleep 2  # let the responder register the advertisement

echo "── discover _harp._tcp"
"$PROBE" discover >/tmp/mdns-disc.log 2>&1
cat /tmp/mdns-disc.log
# OUR advertisement (port $PORT) must appear WITH a proto TXT...
grep -qE ":$PORT[[:space:]].*proto=" /tmp/mdns-disc.log || { cat /tmp/mdns-disc-dev.log; fail "device not discovered on _harp._tcp (port $PORT with proto)"; }
# ...and MUST NOT leak a serial (the discover command flags a serial TXT key)
grep -qiE 'serial leaked' /tmp/mdns-disc.log && fail "device LEAKED its serial in the TXT record (§16 violation)"

# dial the DISCOVERED host:port -> prove discovery yields a usable address reaching the device
ADDR=$(sed -nE "s/^[[:space:]]*([A-Za-z0-9._-]+:$PORT)[[:space:]].*/\1/p" /tmp/mdns-disc.log | head -1)
[ -n "$ADDR" ] || fail "could not extract a discovered host:port for port $PORT"
echo "── dial the discovered address: $ADDR"
HARP_DEVICE_SERIAL="$SERIAL" "$PROBE" -d "$ADDR" identify >/tmp/mdns-dial.log 2>&1 \
  || { cat /tmp/mdns-dial.log; fail "dialing the discovered address $ADDR failed"; }
grep -qiE "$SERIAL|serial|vendor|product" /tmp/mdns-dial.log || { cat /tmp/mdns-dial.log; fail "discovered device did not identify"; }

echo "MDNS-DISC PASS (§4.4.3: advertised _harp._tcp w/ proto + NO serial; discovered + dialed $ADDR -> live device)"
