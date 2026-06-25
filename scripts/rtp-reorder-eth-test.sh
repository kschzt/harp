#!/bin/bash
# rtp-reorder-eth-test — §8.7: the host tolerates RTP REORDER on the free-running audio plane WITHOUT
# over-counting loss. The device (--reorder-rtp-pct N) holds ~N% of its outgoing datagrams and releases
# each after 3 later ones, so the host sees seq N+1..N+3 then N — a genuine out-of-order arrival, not a
# drop. The shipped plugin path (shell/eth_transport.h::recvAudio) must NOT rewind its high-water seq on
# the reordered packet — rewinding makes the next in-order packet compute a huge spurious gap and
# multiply the §8.7/§14.2 rtp_loss counter (the bug #68's fix missed on this path). Reorder is best-effort
# (no audio is lost — the jitter buffer reorders), so the tone must stay intact AND loss stays bounded.
#
# Co-existence: unique port + kills ONLY its own device by pid; hard perl-alarm watchdog.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47977}"
REORDER="${REORDER:-20}"
DEVDIR=rtpreorder-eth-state
DEVLOG=/tmp/rtpreorder-eth-dev.log
HOSTLOG=/tmp/rtpreorder-eth-host.log
BUNDLE=/tmp/rtpreorder-eth.cbor
fail() { echo "RTP-REORDER FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"; rm -f "$BUNDLE"

echo "── device REORDERS ${REORDER}% of outgoing RTP (440Hz tone, deep swap); host renders 4s over §8.7 loopback"
"$DEVICED" --port "$PORT" --tone 440 --reorder-rtp-pct "$REORDER" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001" \
  perl -e 'alarm 20; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 4 --realtime --diag-bundle "$BUNDLE" >"$HOSTLOG" 2>&1 & HP=$!
wait "$HP"; rc=$?; HP=""
kill -9 "$DP" 2>/dev/null; DP=""

nconn=$(grep -c "connected:" "$HOSTLOG")
rms=$(grep -oE "rms=[0-9.]+" "$HOSTLOG" | tail -1 | cut -d= -f2)
echo "── result: connects=$nconn  rms=${rms:-?}  host-exit=$rc  (reorder=${REORDER}%)"
grep -iE "AddressSanitizer|SEGV|abort trap|terminating due to" "$HOSTLOG" && fail "host CRASHED under ${REORDER}% RTP reorder"
[ "$rc" -eq 142 ] && fail "host HUNG under ${REORDER}% RTP reorder (perl-alarm watchdog fired)"
grep -q "connected:" "$HOSTLOG" || fail "host never connected (no oracle)"
# Reorder loses NO audio (the buffer reorders) — the 440Hz tone must read ~ideal (0.3536), well above 0.2.
awk "BEGIN{exit !(${rms:-0} > 0.2)}" \
  || fail "440Hz tone did not survive ${REORDER}% reorder (rms=${rms:-0} <= 0.2) — the buffer mishandled out-of-order arrival"
# §8.7: the host MUST NOT rewind on a reordered packet. Forward-only loss accounting still counts ~1
# transient gap per reorder (the higher seq arrives before the held one), but the FIX keeps it bounded
# at ~1/reorder; the rewind BUG multiplies it to ~4/reorder (the 3-deep hold). Assert rtp_loss is
# bounded (< 200; the fix reads ~83 here, the bug would read ~330) and counted (>0, never concealed).
# cbor2 is installed on ALL three OSes (eth.yml, Windows included — HIGH #6), so its absence is a hard
# CI fail on EVERY OS; the old Windows skip carve-out (§8.7 MUST untested on one OS) is gone. A bare
# dev box without cbor2 still skip-logs.
if [ -s "$BUNDLE" ] && python3 -c "import cbor2" >/dev/null 2>&1; then
  python3 - "$BUNDLE" <<'PY' || fail "RTP reorder mis-counted as loss (§8.7 host-counters key 8 — rewind over-count?)"
import sys, cbor2
b = cbor2.load(open(sys.argv[1], "rb"))
lost = (b.get(5) or {}).get(8)
if lost is None: sys.exit("host-counters key 8 (rtp_loss) absent")
if lost <= 0:    sys.exit("host-counters key 8 = %r — reorder should still surface its transient gaps (>0)" % (lost,))
if lost >= 200:  sys.exit("host-counters key 8 = %d — reorder OVER-counted (>=200); the high-water seq rewound" % lost)
print("   ✓ host-counters key 8 (rtp_loss) = %d — bounded (~1/reorder, no rewind amplification)" % lost)
PY
elif [ -n "${CI:-}" ]; then
  fail "cbor2 unavailable on CI — the §8.7 reorder loss-count assertion cannot be skipped on any OS (install cbor2; see eth.yml)"
else
  echo "   (cbor2 or bundle absent — skipped the host-counters key-8 assertion on this dev box)"
fi
echo "RTP-REORDER PASS (440Hz tone intact through ${REORDER}% reorder: rms=$rms; loss bounded, not rewound; $nconn connect(s), rc=$rc)"
