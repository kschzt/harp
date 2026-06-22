#!/bin/bash
# admission-eth-test — §8.4 admission control (bandwidth budget + refuse-with-budget), §8.7 loopback.
#
# Before audio.start the runtime reserves the session's audio bandwidth in a process-global
# per-path ledger, and MUST refuse explicitly WITH the computed budget if the path is full —
# never degrade silently. We force both outcomes with the HARP_ADMISSION_BUDGET seam, the
# device + notes held constant so ONLY the budget differs:
#   OVER-BUDGET  (100000 < the stereo free-running need 2*4*48000 = 384000 B/s): audio.start is
#     REFUSED; the host surfaces requested/available/capacity and renders SILENCE (no stream).
#   UNDER-BUDGET (10000000 >> need): admitted; the host streams + renders the real device audio.
# The over-budget vs under-budget render hashes therefore DIFFER — proof admission actually
# gated the stream, not just logged. Cross-session aggregation, exact release, re-negotiation
# idempotency and the >=8-session proof live in registry-tests (the only way to share the
# process-global ledger in one image); this is the e2e refuse/admit-with-budget proof.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47913}"
STATEDIR=admission-eth-state   # workspace-RELATIVE
DEVLOG=/tmp/admission-eth-dev.log
HOSTLOG=/tmp/admission-eth-host.log
fail() { echo "ADMISSION FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 bundle not found"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT INT TERM
rm -rf "$STATEDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$STATEDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }
export HARP_ETH_DEVICE="127.0.0.1:$PORT"
export HARP_DEVICE_SERIAL="SIM-0001"

# run the host under a given admission budget. $1 = budget. Hash -> stdout; log -> $HOSTLOG.
run_host() {
  : > "$HOSTLOG"
  HARP_ADMISSION_BUDGET="$1" perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" \
    --notes 62,69,74,65 --seconds 1 --realtime --hash >"$HOSTLOG" 2>&1
  local rc=$?
  [ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "host HUNG under budget $1 (watchdog)"; }
  sed -n 's/^output-hash: //p' "$HOSTLOG" | head -1
}

echo "── OVER-BUDGET: HARP_ADMISSION_BUDGET=100000 (< 384000 B/s stereo need) -> REFUSE"
HOVER=$(run_host 100000)
grep -q "§8.4 admission: REFUSED" "$HOSTLOG" || { cat "$HOSTLOG"; fail "over-budget audio.start was NOT refused"; }
grep -q "requested 384000 B/s" "$HOSTLOG" || { cat "$HOSTLOG"; fail "refusal did not report the requested figure (the formula)"; }
grep -q "available 100000 B/s" "$HOSTLOG" || { cat "$HOSTLOG"; fail "refusal did not report available bandwidth (computed budget)"; }
grep -q "capacity 100000 B/s" "$HOSTLOG" || { cat "$HOSTLOG"; fail "refusal did not report path capacity (computed budget)"; }
echo "   ✓ refused WITH the computed budget (requested 384000 / available 100000 / capacity 100000)"

echo "── UNDER-BUDGET: HARP_ADMISSION_BUDGET=10000000 (>> need) -> ADMIT"
HUNDER=$(run_host 10000000)
grep -q "§8.4 admission: REFUSED" "$HOSTLOG" && { cat "$HOSTLOG"; fail "under-budget audio.start was wrongly refused"; }
[ -n "$HUNDER" ] || { cat "$HOSTLOG"; fail "under-budget host produced no render hash"; }
echo "   ✓ admitted; streamed (render $HUNDER)"

# admission ACTUALLY gated the stream: same notes/device, only the budget differs, yet the
# refused render (silence) != the admitted render (real device audio).
[ -n "$HOVER" ] && [ "$HOVER" != "$HUNDER" ] \
  || { fail "over-budget render ($HOVER) == under-budget render ($HUNDER) — admission did not gate the stream"; }

echo "ADMISSION PASS (§8.4: audio.start refused WITH the computed budget over-budget [render $HOVER=silence], admitted under-budget [render $HUNDER]; the gate changed the stream)"
