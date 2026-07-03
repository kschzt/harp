#!/bin/bash
# fence-l10-repro — OFF-HARDWARE reproduction + the DECISIVE measurement for the §8.3.1
# fence_timeouts-under-L10-flood. NOT a cloud CI gate: a contention-dependent, on-workstation driver.
#
# The established root cause: a fenced pacing frame's render (audio_loop.c host_paced_loop) blocks
# until the CONSUME thread bumps g_evt_consumed to `want` inside HARP_FENCE_RT_BOUND_NS (5 ms). The
# bump happens only AFTER evq_push wins g_evq_mu (engine.c) — the SAME coarse lock the render thread
# holds across the WHOLE O(S*N) evq_apply_due drain (S segments/block, N up to DEV_EVQ_CAP=512). Under
# the L10 flood (driver.py: n_lyo up to 6/voice, ppb up to 128, dense --set-at) the queue runs deep
# and the block splits into many segments, so the render's lock-hold starves the bump past the bound.
#
# The stock eth-fence-test `load` mode releases ONE event/fenced frame in strict ping-pong: N stays
# shallow, S~1, and render/consume never overlap -> it reads 0. This driver flips BOTH knobs:
#   HARP_FENCE_K       events per fenced frame (sized toward DEV_EVQ_CAP=512): deep N
#   HARP_FENCE_SPREAD  distinct SSI samples/frame: many render segments S
#   HARP_FENCE_PIPE    frames in flight (>=2): the render's deep-queue drain OVERLAPS the consume push
# and turns on HARP_FENCE_INSTRUMENT so the daemon dumps, at each stream teardown:
#   consume-acquire-wait(g_evq_mu)   — how long the bump-releasing push waited for the lock
#   render-lock-hold(evq_apply_due)  — the per-call hold + the queue depth N it scanned
#
# TWO parts:
#   1. SWEEP (no burners): climb K (=> depth) and watch acquire-wait track the lock-hold. If it grows
#      with S*N -> LOCK-DOMINANT (decouple reaches ~statistical-0). If acquire stays ~0 -> a FLOOR.
#   2. BURNERS: inject runqueue pressure to drive fence_timeouts NONZERO off-hardware (the stock
#      one-event load cannot), A/B on the device SCHED_FIFO promotion (HARP_DEVICE_RT).
#
# NOTE (macOS): device SCHED_FIFO is Linux-only (session.c harp_device_thread_set_realtime is a no-op
# off Linux), so on a Mac BOTH arms are time-share and the burner run also carries scheduling jitter.
# The clean lock measurement is therefore the SWEEP (part 1); the burner A/B (part 2) is the raw
# repro. On the Pi, SCHED_FIFO removes the scheduling residual, leaving the lock the sweep isolates.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build-fence/harp-deviced}"
FENCE="${FENCE:-./build-fence/harp-eth-fence-test}"
PORT="${PORT:-17994}"
DEVLOG="${DEVLOG:-/tmp/fence-l10-repro-dev.log}"
FRAMES="${FRAMES:-400}"
PIPE="${PIPE:-4}"
SPREAD="${SPREAD:-256}"
BURNERS="${BURNERS:-6}"          # bounded: leave cores for anything else on the box
BFRAMES="${BFRAMES:-600}"        # burner-arm frame count

fail() { echo "FENCE-L10 FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built (cmake --build build-fence --target harp-deviced)"
[ -x "$FENCE" ]   || fail "$FENCE not built (cmake --build build-fence --target harp-eth-fence-test)"

DP=""; BP=""
start_burn() { BP=""; local n="$1"; for _ in $(seq 1 "$n"); do sh -c 'while :; do :; done' & BP="$BP $!"; done; }
stop_burn()  { [ -n "$BP" ] && { kill $BP 2>/dev/null; wait $BP 2>/dev/null; BP=""; }; }
kill_dev()   { [ -n "$DP" ] && { kill "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""; }; }
cleanup()    { stop_burn; kill_dev; }
trap cleanup EXIT

start_dev() { # $1 = extra env (e.g. HARP_DEVICE_RT=0)
    local devdir; devdir="fence-l10-state.$$"
    rm -rf "$devdir"; : > "$DEVLOG"
    env HARP_FENCE_FORCE_RT=1 HARP_FENCE_INSTRUMENT=1 $1 "$DEVICED" --port "$PORT" \
        --state-dir "$devdir" --panel-sock "" >>"$DEVLOG" 2>&1 &
    DP=$!
    for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
    grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }
    STATEDIR="$devdir"
}
# pull a field out of the last FENCE-INSTR block in the daemon log. The leading space anchors the
# field name so `max` does not also match `depth_max`, and no suffix is assumed (some fields lack ns).
instr() { grep "$1" "$DEVLOG" | tail -1 | grep -oE " $2=[0-9]+" | grep -oE '[0-9]+' | tail -1; }

echo "== fence-l10-repro: FORCE_RT bounded fence, PIPE=$PIPE SPREAD=$SPREAD, instrument ON"
echo "== PART 1 — DEPTH SWEEP (no burners): does acquire-wait track the render lock-hold (S*N)?"
echo "   (mean ns is the smooth trend; the coarse-log2 p99/max show the tail. depth_max -> DEV_EVQ_CAP)"
run_row() { # $1 = K
    local K="$1"
    start_dev ""
    HARP_FENCE_K=$K HARP_FENCE_PIPE=$PIPE HARP_FENCE_SPREAD=$SPREAD \
        "$FENCE" "127.0.0.1:$PORT" "load:$FRAMES" >/tmp/fence-l10-tool.out 2>&1
    sleep 0.2
    local dm hmean hm amean am fw ft
    dm=$(instr "render-lock-hold" "depth_max")
    hmean=$(instr "render-lock-hold" "mean")
    hm=$(instr "render-lock-hold" "max")
    amean=$(instr "consume-acquire-wait" "mean")
    am=$(instr "consume-acquire-wait" "max")
    fw=$(grep -oE 'fence_waits=\+[0-9]+' /tmp/fence-l10-tool.out | grep -oE '[0-9]+')
    ft=$(grep -oE 'fence_timeouts=\+[0-9]+' /tmp/fence-l10-tool.out | grep -oE '[0-9]+')
    printf "%6s %9s %10s %11s %11s %11s %8s %9s\n" \
           "$K" "${dm:-?}" "${hmean:-?}" "${hm:-?}" "${amean:-?}" "${am:-?}" "${fw:-?}" "${ft:-?}"
    kill_dev; rm -rf "$STATEDIR"
}
printf "%6s %9s %10s %11s %11s %11s %8s %9s\n" \
       K depth_max hold_mean hold_max acq_mean acq_max f_waits f_timeout
for K in ${SWEEP_K:-1 64 128 256 512}; do run_row "$K"; done

[ "${PART2:-1}" = 1 ] || { echo "== (PART2=0: burner A/B skipped)"; exit 0; }
echo
echo "== PART 2 — BURNERS ($BURNERS): drive fence_timeouts NONZERO (K=384 PIPE=$PIPE, $BFRAMES frames)"
echo "   A/B on the device SCHED_FIFO promotion (macOS: both time-share; the delta is the raw repro)"
for arm in "before HARP_DEVICE_RT=0" "after "; do
    set -- $arm; label="$1"; env2="${2:-}"
    start_dev "$env2"
    start_burn "$BURNERS"
    HARP_FENCE_K=384 HARP_FENCE_PIPE=$PIPE HARP_FENCE_SPREAD=$SPREAD \
        "$FENCE" "127.0.0.1:$PORT" "load:$BFRAMES" >/tmp/fence-l10-tool.out 2>&1
    stop_burn; sleep 0.2
    echo "── $label ($env2):"
    grep '^fence-load:' /tmp/fence-l10-tool.out
    echo "   acquire-wait max=$(instr consume-acquire-wait max)ns  hold max=$(instr render-lock-hold max)ns  depth_max=$(instr render-lock-hold depth_max)"
    kill_dev; rm -rf "$STATEDIR"
done
echo "== done. VERDICT rule: acquire-wait p99/max climbing with depth_max across the sweep => LOCK-DOMINANT."
