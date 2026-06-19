#!/bin/bash
# corrupt-cbor-eth-test — §8.7 fault injection: the host survives a HOSTILE/buggy device that
# emits corrupt CBOR. The device (--corrupt-ctl-pct N) flips one byte in ~N% of its outgoing
# framed CBOR (ctl responses + echoes + events, all via harp_link_send). The host decodes
# device-trusted bytes (the ctl-response decode AND the pollEcho echo decode) and MUST NOT
# crash, MUST NOT trip a sanitizer, MUST NOT hang — it may reject frames / reconnect, but it
# must survive. Best run UNDER ASan/UBSan (the eth-asan job) so a corruption-triggered
# over-read or UB in the CBOR decoder is CAUGHT, not silently tolerated.
#
# Co-existence: unique port + kills ONLY its own device by pid; hard perl-alarm watchdog.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build-asan build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47975}"
CORRUPT="${CORRUPT:-8}"
DEVDIR=/tmp/corrupt-eth-state
DEVLOG=/tmp/corrupt-eth-dev.log
HOSTLOG=/tmp/corrupt-eth-host.log
fail() { echo "CORRUPT-CBOR FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"

echo "── device corrupts ~${CORRUPT}% of framed CBOR; host decodes for 5s over the §8.7 loopback"
"$DEVICED" --port "$PORT" --corrupt-ctl-pct "$CORRUPT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001" \
  perl -e 'alarm 25; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 5 --realtime >"$HOSTLOG" 2>&1 & HP=$!
wait "$HP"; rc=$?; HP=""
kill -9 "$DP" 2>/dev/null; DP=""

conn=$(grep -c "connected:" "$HOSTLOG")
echo "── result: host-exit=$rc  connects=$conn  (corrupt=${CORRUPT}%)"
# SURVIVAL gates — a hostile device must never crash / UB / hang the host:
grep -iE "AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|heap-buffer-overflow|stack-buffer-overflow|global-buffer|SEGV|SIGSEGV|abort trap" "$HOSTLOG" \
    && fail "host crashed / sanitizer trap decoding corrupt CBOR"
[ "$rc" -eq 142 ] && fail "host HUNG on corrupt CBOR (perl-alarm watchdog fired)"
[ "$rc" -eq 139 ] && fail "host SEGV on corrupt CBOR (exit 139)"
[ "$rc" -eq 134 ] && fail "host aborted on corrupt CBOR (exit 134)"
echo "CORRUPT-CBOR PASS (host survived ~${CORRUPT}% framed-CBOR corruption: exit rc=$rc, $conn connect(s), no crash/UB/hang)"
