#!/bin/bash
# loopback-eth-test — §14.3 host LoopbackMeasurer, over the §8.7 host-paced loopback.
#
# The DEVICE-side §14.3 (device/session.c diag.loopback.start/stop + device/engine.c
# the same-frame in->out copy) is exercised here by the HOST runtime's measurement:
# HarpRuntime::measureLoopback() arms diag.loopback.start, injects a one-sample impulse
# on the declared H->D in-slot column in a host-paced pacing frame, locates the echo on
# the D->H out-slot column, and derives the round-trip in samples (RTT = the host's send
# frontier minus the echoed frame's SSI). The device echoes IN->OUT in the SAME rendered
# frame (device-internal loop latency = 0), so the measured RTT is PURE transport
# buffering and is compared to the §6.4 latency-profile expected (identity key 8:
# input-latency + output-latency + buffer-depth; refdev = 0 + 0 + 256 @ 48 kHz).
#
# TRIGGER: harp-vst3-host --loopback IN,OUT (mirrors --diag-bundle): sets HARP_LOOPBACK_IN
# /_OUT, the runtime arms at start() and runs the probe at session teardown — AFTER the
# render, while the live host-paced session is still up — printing:
#   loopback: in=.. out=.. rate=.. armed=1 echo=1 ok=1 rtt-samples=.. expected-samples=.. delta-ms=..
#
# OUT-SLOT CHOICE (per eth-agent): the device OVERWRITES the out-slot column with the echo
# (it does not mix), so we MUST drive an out-slot the synth is NOT generating notes on. The
# refdev streams the main mix on {0,1}; we play notes on part 0 and loopback on the UNUSED
# pair 10,11 (part 4's slots, which carry no notes here) — the echo never lands on the
# captured main mix, so the render stays untouched (the offline goldens, run alongside in
# offline-golden-eth.sh, stay byte-identical).
#
# ASSERTIONS:
#   - all platforms: connected + armed + echo-detected (the loop is wired end-to-end);
#   - LINUX (strict, TIMER_ABSTIME pacing): |measured - expected| <= 1 ms (T11). macOS /
#     Windows runners gate strict timing loosely (relative-nanosleep jitter), exactly as the
#     bit-exact / offline-golden tests do, so there we assert only the wiring, not the ±1ms.
#
# Co-existence: unique port + kills ONLY its own device/host by pid; perl-alarm watchdog;
# workspace-RELATIVE state dir (Git Bash /tmp->C:\ trips the device mkdir on Windows; see
# eth-tests.sh).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47989}"
SERIAL="${SERIAL:-SIM-0001}"
IN_SLOT="${IN_SLOT:-10}"   # H->D injection slot (no synth notes ride it)
OUT_SLOT="${OUT_SLOT:-11}" # D->H echo slot (UNUSED by notes — the echo overwrites it)
TOL_MS="${TOL_MS:-1.0}"    # §6.4 strict tolerance (Linux)
DEVDIR=loopback-eth-state  # workspace-RELATIVE (see header)
DEVLOG=/tmp/loopback-eth-dev.log
HOSTLOG=/tmp/loopback-eth-host.log
fail() { echo "LOOPBACK-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" --panel-sock "" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# A clean patch + a 4-note line on part 0 (main mix {0,1}); the loopback rides the UNUSED
# pair {IN_SLOT,OUT_SLOT}. host-paced (default kOffline) is what diag.loopback.digital needs.
SETTLE="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2"
echo "── run the host (§8.7 host-paced offline) with --loopback $IN_SLOT,$OUT_SLOT"
HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" \
  perl -e 'alarm 40; exec @ARGV' "$HOSTBIN" "$PLUG" $SETTLE \
    --notes 62,69,74,65 --seconds 2.6 --loopback "$IN_SLOT,$OUT_SLOT" >"$HOSTLOG" 2>&1 & HP=$!
wait "$HP"; rc=$?; HP=""
[ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "host HUNG (perl-alarm watchdog fired)"; }
[ "$rc" -eq 0 ] || { cat "$HOSTLOG"; fail "host exited rc=$rc"; }
grep -q "connected:" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host never connected"; }

# The runtime prints ONE machine-readable summary line at teardown:
#   loopback: in=.. out=.. rate=.. armed=1 echo=1 ok=1 rtt-samples=.. expected-samples=.. delta-ms=..
LINE=$(grep -E '^loopback: in=[0-9]+ out=' "$HOSTLOG" | tail -1)
[ -n "$LINE" ] || { cat "$HOSTLOG"; fail "no loopback summary line (measurement didn't run?)"; }
echo "   $LINE"

field() { echo "$LINE" | sed -nE "s/.*$1=([-0-9.]+).*/\1/p"; }
ARMED=$(field armed); ECHO=$(field echo); OK=$(field ok)
RTT=$(field rtt-samples); EXP=$(field expected-samples); DELTA=$(field delta-ms)

# ── all-platform wiring assertions ───────────────────────────────────────────────
[ "$ARMED" = "1" ] || { cat "$HOSTLOG"; fail "device did not ARM the loopback (armed=$ARMED)"; }
[ "$ECHO" = "1" ]  || { cat "$HOSTLOG"; fail "no ECHO detected on out-slot $OUT_SLOT (echo=$ECHO)"; }

# ── Linux-strict §6.4 ±1ms gate (T11). macOS/Windows: wiring only (runner jitter). ──
case "$(uname -s)" in
  Linux)
    [ "$OK" = "1" ] || { cat "$HOSTLOG"; fail "measurement not OK (ok=$OK) — no RTT derived"; }
    [ -n "$DELTA" ] || fail "no delta-ms in the summary"
    # |delta_ms| <= TOL_MS
    awk -v d="$DELTA" -v t="$TOL_MS" 'BEGIN{ if (d<0) d=-d; exit !(d<=t) }' \
      || { cat "$HOSTLOG"; fail "RTT out of §6.4 spec: measured=$RTT expected=$EXP delta=${DELTA}ms (> ±${TOL_MS}ms)"; }
    echo "LOOPBACK-ETH PASS (Linux strict): armed + echo on slot $OUT_SLOT; RTT=$RTT samples ≈ §6.4 expected=$EXP (delta=${DELTA}ms, within ±${TOL_MS}ms)"
    ;;
  *)
    echo "LOOPBACK-ETH PASS (non-Linux, wiring only): connected + armed + echo on slot $OUT_SLOT (RTT=$RTT expected=$EXP delta=${DELTA}ms — strict ±1ms gated to Linux for runner jitter)"
    ;;
esac

kill -9 "$DP" 2>/dev/null; DP=""
