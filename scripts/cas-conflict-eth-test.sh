#!/bin/bash
# cas-conflict-eth-test — §11.3 CAS conflict detection over the §8.7 loopback.
#
# Drives harp-probe `cas-test` against a fresh harp-deviced (fake hardware on a unique
# port). cas-test exercises the recall-negotiation REJECTION paths the happy path never
# hits, and asserts the device's §11.3 CAS error map:
#   (a) wrong expect (flipped digest)            -> error 'conflict'
#   (b) the same wrong expect + force            -> ACCEPTED (the §11.4 override)
#   (c) target whose closure was never pushed    -> error 'not-found'
#   (d) expect carrying an unknown hash-alg byte -> error 'malformed' (§10.2)
#   (e) NEW — DIRTY-REF: a CAS whose expect MATCHES the head is STILL rejected 'conflict'
#       with the distinct message "ref is dirty" because a front-panel knob edit left live
#       DIRTY. Proves the device distinguishes the two conflict causes (key 1 of the error
#       map: "ref is dirty" vs "expect mismatch") and refuses to overwrite unsaved edits.
#
# A fresh --state-dir device boots with a factory live/project snapshot, so cas-test needs
# no `demo` to populate the live ref. HARP_RECONCILE_TIMEOUT_MS=0 keeps it headless (no panel
# answers the loopback). Co-existence: unique port (17997), own state-dir, kills only its pid.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17997}"
SERIAL="${SERIAL:-SIM-0001}"
DEVDIR=cas-conflict-eth-state   # workspace-RELATIVE (Git Bash /tmp -> C:\ trips the MinGW device mkdir)
DEVLOG=/tmp/cas-conflict-eth-dev.log
OUT=/tmp/cas-conflict-eth.out
fail() { echo "CAS-CONFLICT-ETH FAIL: $1"; exit 1; }
. "$(dirname "$0")/eth-extern-lib.sh"

# EXTERNAL-ENDPOINT MODE (§8.7 over a real network hop): the probe drives cas-test against the
# already-running external deviced directly — no local spawn. cas-test's expects/digests are
# self-contained (it relies only on the daemon's factory live/project snapshot, which a running
# daemon boots). Default loopback path below is untouched when HARP_ETH_EXTERN is unset.
if eth_extern_active; then
    eth_extern_banner cas-conflict
    [ -x "$PROBE" ] || fail "$PROBE not built (needed as the external client)"
    HARP_RECONCILE_TIMEOUT_MS=0 "$PROBE" -d "$(eth_extern_ep)" cas-test >"$OUT" 2>&1 \
      || { cat "$OUT"; fail "cas-test assertions failed against $(eth_extern_ep)"; }
    cat "$OUT"
    grep -q "dirty-ref -> conflict: OK" "$OUT" || { cat "$OUT"; fail "the DIRTY-REF case (e) did not pass — 'dirty-ref -> conflict: OK' absent"; }
    grep -q "CAS-TEST PASS" "$OUT"            || { cat "$OUT"; fail "cas-test did not report PASS"; }
    echo "CAS-CONFLICT-ETH PASS (external §11.3 over the real link: stale-expect AND dirty-ref -> conflict against $(eth_extern_ep))"
    exit 0
fi

[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf "$DEVDIR"' EXIT INT TERM
rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

HARP_RECONCILE_TIMEOUT_MS=0 "$PROBE" -d "127.0.0.1:$PORT" cas-test >"$OUT" 2>&1 || { cat "$OUT"; cat "$DEVLOG"; fail "cas-test assertions failed"; }
cat "$OUT"
grep -q "dirty-ref -> conflict: OK" "$OUT" || { cat "$OUT"; fail "the DIRTY-REF case (e) did not pass — 'dirty-ref -> conflict: OK' absent"; }
grep -q "CAS-TEST PASS" "$OUT"            || { cat "$OUT"; fail "cas-test did not report PASS"; }
echo "CAS-CONFLICT-ETH PASS (§11.3: stale-expect AND dirty-ref both -> conflict)"
