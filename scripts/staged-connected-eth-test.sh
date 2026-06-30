#!/bin/bash
# staged-connected-eth-test — §11.4 staged-while-connected held READ-ONLY (HIGH #8), over the §8.7 loopback.
#
# The #73 production trigger: a DAW stages a project recall onto an ALREADY-LIVE device. The hooks
# agent's `--load-state-after-connect FILE` (tools/vst3-host) defers the setState until AFTER
# setActive(true), so the restore drives HarpRuntime::setStateBundle's connected() branch (vs the
# pre-activate --load-state path). Per §11.4/§12.2, a project staged while connected onto an
# ENGINE-MISMATCHED / DIFFERENT-SERIAL unit MUST be HELD READ-ONLY — recomputeReadOnlyHolds() runs
# against the LIVE identity and the bundle is NOT auto-pushed. The device can't self-protect a
# serial-differs case (same engine, so the §13.4 device gate never fires), so the runtime must.
#
# Flow:
#   0) --save-state a bundle on serial SIM-0001 (records the saved-on serial in the bundle).
#   1) MISMATCH: run the host against a DIFFERENT unit (serial SIM-ALT-0002, same engine) and stage
#      the SIM-0001 bundle via --load-state-after-connect. Assert the staged-while-connected read-only
#      hold fires ("held read-only ... not auto-applied") and the project is NOT re-asserted/pushed.
#   2) CONTROL: stage the SAME bundle onto the SAME unit it was saved on (SIM-0001). The hold must NOT
#      fire — same serial/engine, so the staged-while-connected push goes through. This proves the
#      hold keys on the identity mismatch, not merely on the connected() staging path.
#
# Co-existence: unique port; kills ONLY its own pids; perl-alarm watchdog; workspace-RELATIVE state
# dir (Git Bash /tmp->C:\ trips the MinGW device mkdir on Windows; see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17992}"
SAVED_SERIAL="${SAVED_SERIAL:-SIM-0001}"
ALT_SERIAL="${ALT_SERIAL:-SIM-ALT-0002}"
DEVLOG=/tmp/staged-connected-eth-dev.log
HOSTLOG=/tmp/staged-connected-eth-host.log
STATE=/tmp/staged-connected.state   # --save-state bundle, staged via --load-state-after-connect
fail() { echo "STAGED-CONNECTED FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

# run_host <serial> <label> <extra args...> — leaves output in $HOSTLOG; hang-proof (30s perl-alarm).
run_host() {
  local serial="$1" label="$2"; shift 2
  : > "$HOSTLOG"
  echo "── run the host (§8.7 Ethernet) [$label] device-serial=$serial $*"
  # §11.4 reconcile is INTERACTIVE: a same-serial/same-engine bundle whose hash differs from the live
  # device head posts a reconcile OFFER and waits HARP_RECONCILE_TIMEOUT_MS (default 30s) for a panel to
  # pick Push/Pull/Read-only. This headless test has NO panel, so that wait would block the deferred
  # setStateBundle for the full 30s and trip the 30s perl-alarm watchdog (the [control] "hang"). Set the
  # timeout to 0 so the headless reconcile takes its deterministic Push FALLBACK immediately — which is
  # exactly the control's expectation (same identity -> the staged-while-connected push goes through, NOT
  # held read-only). The MISMATCH run returns at the read-only gate BEFORE reconcile, so this is a no-op there.
  HARP_RECONCILE_TIMEOUT_MS=0 HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$serial" \
    perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 2 --realtime "$@" >"$HOSTLOG" 2>&1 & HP=$!
  wait "$HP"; local rc=$?; HP=""
  [ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "[$label] host HUNG (perl-alarm watchdog fired)"; }
  [ "$rc" -eq 0 ] || { cat "$HOSTLOG"; fail "[$label] host exited rc=$rc"; }
  grep -q "connected:" "$HOSTLOG" || { cat "$HOSTLOG"; fail "[$label] host never connected"; }
}

start_dev() { # $1 = serial, $2 = state-dir suffix
  rm -rf "staged-connected-$2"; : > "$DEVLOG"
  "$DEVICED" --serial "$1" --port "$PORT" --state-dir "staged-connected-$2" >>"$DEVLOG" 2>&1 & DP=$!
  wait_listen || { cat "$DEVLOG"; fail "device ($1) didn't start on $PORT"; }
}

# ── 0) save a project bundle on the SAVED_SERIAL unit (records the saved-on serial).
start_dev "$SAVED_SERIAL" save
rm -f "$STATE"
run_host "$SAVED_SERIAL" save-fixture --save-state "$STATE"
[ -s "$STATE" ] || { cat "$HOSTLOG"; fail "save-state produced no fixture at $STATE"; }
kill -9 "$DP" 2>/dev/null; DP=""
echo "   ✓ staged a project-state fixture on $SAVED_SERIAL ($(wc -c <"$STATE") bytes)"

# ── 1) MISMATCH: stage the SAVED_SERIAL bundle onto a DIFFERENT unit WHILE CONNECTED. Must be held
#       read-only (the §11.4 HIGH #8 staged-while-connected path), NOT auto-applied/pushed.
start_dev "$ALT_SERIAL" alt
run_host "$ALT_SERIAL" mismatch --load-state-after-connect "$STATE"
grep -q "held read-only" "$HOSTLOG" \
  || { cat "$HOSTLOG"; fail "staged-while-connected bundle on a DIFFERENT unit was NOT held read-only (§11.4/§12.2 HIGH #8 — setStateBundle connected() branch didn't gate)"; }
grep -q "not auto-applied" "$HOSTLOG" \
  || { cat "$HOSTLOG"; fail "staged-while-connected hold did not log the 'not auto-applied' enforcement (auto-push not skipped)"; }
grep -q "project state re-asserted" "$HOSTLOG" \
  && { cat "$HOSTLOG"; fail "staged-while-connected bundle was AUTO-APPLIED onto a different unit (read-only hold not taken — HIGH #8)"; }
kill -9 "$DP" 2>/dev/null; DP=""
echo "   ✓ mismatch: SIM-0001 project staged-while-connected onto $ALT_SERIAL HELD READ-ONLY (not auto-applied)"

# ── 2) CONTROL: stage the SAME bundle onto the SAME unit it was saved on. The hold must NOT fire —
#       same identity, so the connected() staging push goes through. Proves the gate keys on the
#       mismatch, not on the staged-while-connected path itself.
start_dev "$SAVED_SERIAL" ctl
run_host "$SAVED_SERIAL" control --load-state-after-connect "$STATE"
grep -q "held read-only" "$HOSTLOG" \
  && { cat "$HOSTLOG"; fail "control: staging the bundle onto its OWN unit wrongly held read-only (false positive on a matching identity)"; }
kill -9 "$DP" 2>/dev/null; DP=""
echo "   ✓ control: staged-while-connected onto its own unit ($SAVED_SERIAL) was NOT held read-only"

# ── 3) TRANSIENT (Windows-flake regression guard): stage onto the SAME unit but FORCE the
#       auto-push to hit ONE transient ctl error (HARP_TEST_PUSH_FAIL). A failed auto-push of a
#       staged-while-connected bundle is RECOVERABLE — "Live wins" re-asserts on reconnect, exactly
#       as the connect-time re-assert tolerates it — so the host MUST NOT die. Before the fix the
#       runtime propagated the push failure as false -> the VST3 host's component->setState returned
#       kResultFalse -> "component setState failed" -> rc=1: the intermittent `staged-connected`
#       failure that only surfaced under a loaded Windows CI runner. This reproduces it on demand.
start_dev "$SAVED_SERIAL" trans
export HARP_TEST_PUSH_FAIL=1
run_host "$SAVED_SERIAL" transient --load-state-after-connect "$STATE"   # run_host already asserts rc==0
unset HARP_TEST_PUSH_FAIL
grep -q "auto-push deferred (transient" "$HOSTLOG" \
  || { cat "$HOSTLOG"; fail "transient: a forced push failure did NOT take the non-fatal deferred path (the Windows-flake fix is missing)"; }
grep -q "held read-only" "$HOSTLOG" \
  && { cat "$HOSTLOG"; fail "transient: a transient push failure wrongly held read-only (must defer, not hold)"; }
kill -9 "$DP" 2>/dev/null; DP=""
echo "   ✓ transient: a forced transient auto-push failure was NON-FATAL (host survived rc=0, bundle staged) — Windows flake guarded"

echo "STAGED-CONNECTED PASS (§11.4 HIGH #8: --load-state-after-connect onto a mismatched/different-serial unit held READ-ONLY [not auto-applied]; same-unit staging applies; a transient push failure is non-fatal)"
