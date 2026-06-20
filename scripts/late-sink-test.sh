#!/bin/sh
# late-sink-test — P5b RE-NEGOTIATION proven on real hardware: a sink that
# registers AFTER the owner is already streaming RELIABLY hears its part. This is
# the inverse of the alias-part-audio-test "fixed-at-start" union: there every
# sink is in the audio.start union BEFORE the stream begins (a DAW activating all
# tracks at project load); here the sink arrives MID-SESSION — a track added to a
# live transport — so its slots are NOT in the live union and the feeder must
# RE-STREAM the wider union (audio.stop -> new union -> audio.start) without
# tearing the session down. The bug class this guards:
#   - a stale per-sink pad debt (silence padded while waiting for the re-neg) that
#     would otherwise eat the first real demuxed frames -> a SILENT late sink
#     (~1 in 8 before the epoch-reset fix);
#   - the re-neg tripping a reconnect / RT-state reset (the TSan B1 race) — here
#     it must just re-stream and keep playing.
#
# THE SIGNAL is the harness's sink-rms (the demuxed late part's energy), captured
# only AFTER the re-negotiation has streamed the wider union (tsan-host polls the
# runtime's renegCount() then settles, so the capture never accumulates the
# pre-re-neg silence — the proof is DETERMINISTIC, not racing the re-neg).
#
# THE ASSERTION: across N>=5 runs the late sink is non-silent EVERY time
# (sink-rms > FLOOR each run). One silent run fails — the point is reliability,
# not "usually works". Builds the harness WITHOUT TSan (a TSan binary aborts at
# startup on recent Linux); pins HARP_DEVICE_SERIAL; needs ONE board, Live closed.
# Exit 0 pass / 2 N/A (board absent) / 3 device busy.
set -e
cd "$(dirname "$0")/.."

SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
export HARP_DEVICE_SERIAL="$SERIAL"
PROBE="${PROBE:-./build/harp-probe}"
BUILD="${BUILD:-build-mt-host}"      # the multi-instance harness, built WITHOUT TSan
INSTANCES="${INSTANCES:-4}"          # owner + 3 attached parts; the first late sink captures
SECONDS_RUN="${SECONDS_RUN:-8}"
RUNS="${RUNS:-5}"                    # >=5: reliability, not a single lucky capture
FLOOR="${FLOOR:-0.002}"             # sink-rms floor: clearly non-silent (obs ~0.013–0.05 on PI4B-0001)

# claim guard: a DAW holding the device steals the claim and every render is
# silence (a bogus fail) — a hard error, never a silent skip.
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "LATE-SINK FAIL: device claimed by Ableton Live — needs it exclusively"
    exit 3
fi

# need the pinned board; without it nothing connects and the re-neg is unobservable.
if [ -x "$PROBE" ]; then
    if ! "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL"; then
        echo "LATE-SINK SKIP: board $SERIAL not on the bus"; exit 2
    fi
else
    echo "LATE-SINK SKIP: $PROBE not built (need it to confirm the board)"; exit 2
fi
echo "── late-sink: a sink that registers MID-SESSION on $SERIAL reliably hears its part"

# Build the multi-instance harness WITHOUT ThreadSanitizer (same reasoning as
# alias-part-audio-test: this asserts the FUNCTIONAL re-neg, not race-freedom —
# tsan-shell.sh's --late-sink config covers the races — and a TSan binary aborts
# at startup on recent Linux kernels).
echo "── building multi-instance harness (-DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF)"
cmake -B "$BUILD" -S tools/vst3-host -DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF >/dev/null
cmake --build "$BUILD" --target tsan-host -j >/dev/null
[ -x "$BUILD/tsan-host" ] || { echo "LATE-SINK FAIL: harness build produced no binary"; exit 1; }

# one capture: --instances N --late-sink (the sinks register after start ->
# re-negotiation). The harness arms the designated sink's capture only after the
# re-neg lands, so a clean connected run yields its post-re-neg part energy. Echo
# the sink-rms (or -1 on a never-connected / device-busy run, retried a few times
# exactly as alias-part-audio-test does for the post-teardown claim race).
capture() {
    for try in 1 2 3 4; do
        out=$(mktemp /tmp/late-sink.XXXXXX)
        # --no-state-stress (as alias-part-audio-test.sh does): the owner's periodic
        # getState/setState CAS-to-store Push starves the stream into underrun, and the
        # SINK is captured only over the armed (post-re-neg) window — so a setState
        # starvation landing in that window reads the late sink as FULL silence while
        # the whole-run main mix stays audible (the residual ~1/13 flake). State-churn
        # resilience is a SEPARATE concern (tsan-shell.sh's race configs); this test
        # isolates the re-neg DELIVERING the part, so it opts out of the churn.
        "$BUILD/tsan-host" --instances "$INSTANCES" --late-sink --no-state-stress \
            --seconds "$SECONDS_RUN" --block 256 --out "/tmp/late-sink-$1.wav" >"$out" 2>&1 || true
        s=$(grep '^sink-rms:' "$out" | awk '{print $2}')
        m=$(grep '^main-rms:' "$out" | awk '{print $2}')
        conn=$(grep -c "harp-shell: connected:.*serial $SERIAL" "$out" || true)
        reneg=$(grep -c "re-negotiated audio stream" "$out" || true)
        rm -f "$out"
        # A VALID sample is a connected run that actually RE-NEGOTIATED and whose
        # OWNER MAIN MIX is non-silent (the session is producing audio). A silent
        # main mix means a transient device/claim hiccup, NOT a late-sink verdict —
        # retry it (mirrors alias-part-audio-test retrying a silent capture). Once
        # the session is healthy, sink-rms is the post-re-neg part energy we assert
        # on (silent or not — a connected+reneg'd+audible-session silent sink is a
        # REAL late-sink failure, never retried away).
        if [ "$conn" -gt 0 ] && [ "$reneg" -gt 0 ] && [ -n "$s" ] && [ -n "$m" ] \
           && [ "$(python3 -c "print(1 if $m > $FLOOR else 0)")" = 1 ]; then
            echo "$s"; return
        fi
        sleep 1
    done
    echo "-1"
}

echo "── sampling the late sink's post-re-neg part energy ×$RUNS (each must be > $FLOOR)"
fails=0; i=0
while [ "$i" -lt "$RUNS" ]; do
    s=$(capture "$i")
    case "$s" in -1) echo "LATE-SINK FAIL: never connected / never re-negotiated (device busy?)"; exit 3 ;; esac
    nonsilent=$(python3 -c "print(1 if $s > $FLOOR else 0)")
    if [ "$nonsilent" = 1 ]; then
        echo "   run $i: sink-rms=$s  (non-silent ✓)"
    else
        echo "   run $i: sink-rms=$s  (SILENT — late sink heard nothing ✗)"
        fails=$((fails + 1))
    fi
    i=$((i + 1)); sleep 1
done

if [ "$fails" -eq 0 ]; then
    echo "LATE-SINK PASS (a mid-session sink on $SERIAL reliably hears its part across"
    echo "   $RUNS/$RUNS runs — the feeder re-streamed the wider union and the late sink's"
    echo "   pad debt did not eat the first real frames)"
    exit 0
else
    echo "LATE-SINK FAIL: $fails/$RUNS runs left the late sink SILENT after a successful"
    echo "   re-negotiation — the demux did not reliably deliver its part (stale pad debt"
    echo "   / ring, or the capture raced the re-neg)"
    exit 1
fi
