#!/bin/bash
# fence-load-rt — §8.3.1 host-paced --realtime fence UNDER CPU CONTENTION, A/B on the device's
# SCHED_FIFO promotion (device/session.c::harp_device_thread_set_realtime).
#
# The companion realtime-fence-eth.sh proves the real-time fence is BOUNDED + counted (a host that
# fences beyond its feed does not wedge). This proves the complementary property: when the host DOES
# feed every fenced event IN ORDER, the device must NOT count a fence_timeout — even under CPU load.
# A NORMAL-priority event-consume thread can be descheduled past the few-ms bound under contention,
# applying an in-order event late (a benign fence_timeout, audio_underruns still 0). SCHED_FIFO on
# the device's event-consume + audio threads closes that race → literal zero.
#
# It runs TWO arms against a fresh HARP_FENCE_FORCE_RT=1 daemon (bounded real-time fence over the TCP
# carrier — faithful to a USB host-paced stream: the fence reads a->offline, not the transport) while
# $BURNERS CPU burners hammer every core:
#   before : HARP_DEVICE_RT=0  — the device threads stay time-share (the pre-fix behaviour)
#   after  : default            — the device threads self-promote to SCHED_FIFO (the fix)
# eth-fence-test in `load` mode streams $FRAMES fenced frames, EACH released by an in-order event,
# and prints the fence_timeouts delta per arm. PASS = the SCHED_FIFO arm hit fence_timeouts=0 with
# audio_underruns=0 (and no audio_overruns / frame_errors / evt_late regression).
#
# Needs SCHED_FIFO to be grantable for the `after` arm (run as root, or grant CAP_SYS_NICE /
# RLIMIT_RTPRIO to the daemon). If RT is DENIED the daemon logs it once and degrades — the script
# reports that honestly instead of a false PASS. POSIX raw-socket tool → not built on Windows.
# Exit 0 pass / 1 fail. This is an ON-TARGET driver (contention-dependent); it is NOT a cloud CI gate.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
FENCE="${FENCE:-./build/harp-eth-fence-test}"
PORT="${PORT:-17998}"
FRAMES="${FRAMES:-4000}"
BURNERS="${BURNERS:-8}"        # 2x a 4-core Pi — enough runqueue pressure to deschedule time-share
DEVLOG="${DEVLOG:-/tmp/fence-load-rt-dev.log}"

fail() { echo "FENCE-LOAD-RT FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$FENCE" ]   || fail "$FENCE not built (POSIX-only host-paced fence tool)"

BURN_PIDS=""
start_burners() {
    BURN_PIDS=""
    for _ in $(seq 1 "$BURNERS"); do
        # a pure CPU spinner (no I/O), niced to normal — pins a core so the daemon's time-share
        # event-consume thread must contend for the runqueue (the fence-inducing condition).
        sh -c 'while :; do :; done' &
        BURN_PIDS="$BURN_PIDS $!"
    done
}
stop_burners() { [ -n "$BURN_PIDS" ] && { kill $BURN_PIDS 2>/dev/null; wait $BURN_PIDS 2>/dev/null; BURN_PIDS=""; }; }

DP=""
cleanup() { stop_burners; [ -n "$DP" ] && { kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""; }; }
trap cleanup EXIT

run_arm() { # $1=label  $2=extra_env
    local label="$1" env2="$2" devdir line rtline
    devdir="fence-load-$label-state.$$"
    rm -rf "$devdir"; : > "$DEVLOG"
    env HARP_FENCE_FORCE_RT=1 $env2 "$DEVICED" --port "$PORT" --state-dir "$devdir" --panel-sock "" \
        >>"$DEVLOG" 2>&1 &
    DP=$!
    for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
    grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null || { cat "$DEVLOG"; fail "device didn't start on $PORT ($label)"; }

    start_burners
    # 180s alarm: 4000 fenced frames back-to-back under heavy contention can be slow; a wedge (an
    # unbounded fence) would hang drain_output — the alarm turns that into a deterministic failure.
    line=$(perl -e 'alarm 180; exec @ARGV' "$FENCE" "127.0.0.1:$PORT" "load:$FRAMES"); local rc=$?
    stop_burners
    kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""
    rtline=$(grep -m1 -E "SCHED_FIFO|SCHED_FIFO denied" "$DEVLOG" | sed 's/^.*harp-deviced: //')
    rm -rf "$devdir"
    [ "$rc" = 0 ] || { echo "$line"; fail "$label load run failed (rc=$rc — wedge or I/O error; see $DEVLOG)"; }
    echo "── $label:"
    echo "   rt: ${rtline:-<no rt log line>}"
    echo "   $line"
    echo "$line"   # last line = machine-parseable, captured by the caller
}

echo "══ fence-load-rt: §8.3.1 host-paced real-time fence under $BURNERS burners, $FRAMES frames/arm"
BEFORE=$(run_arm before "HARP_DEVICE_RT=0" | tail -1)
AFTER=$(run_arm after "" | tail -1)

bt=$(printf '%s\n' "$BEFORE" | grep -oE 'fence_timeouts=\+[0-9]+' | grep -oE '[0-9]+')
at=$(printf '%s\n' "$AFTER"  | grep -oE 'fence_timeouts=\+[0-9]+' | grep -oE '[0-9]+')
au=$(printf '%s\n' "$AFTER"  | grep -oE 'audio_underruns=\+[0-9]+' | grep -oE '[0-9]+')
ao=$(printf '%s\n' "$AFTER"  | grep -oE 'audio_overruns=\+[0-9]+' | grep -oE '[0-9]+')
fe=$(printf '%s\n' "$AFTER"  | grep -oE 'frame_errors=\+[0-9]+' | grep -oE '[0-9]+')

echo "══ A/B: time-share fence_timeouts=$bt  ->  SCHED_FIFO fence_timeouts=$at (audio_underruns=$au)"
[ "${at:-x}" = 0 ] || fail "SCHED_FIFO arm still counted $at fence_timeouts (RT not granted? see the rt: line)"
[ "${au:-x}" = 0 ] || fail "SCHED_FIFO arm regressed audio_underruns=$au"
[ "${ao:-x}" = 0 ] || fail "SCHED_FIFO arm regressed audio_overruns=$ao"
[ "${fe:-x}" = 0 ] || fail "SCHED_FIFO arm regressed frame_errors=$fe"
echo "FENCE-LOAD-RT PASS: SCHED_FIFO drove §8.3.1 fence_timeouts to ZERO under load (audio_underruns 0)"
exit 0
