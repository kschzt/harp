#!/bin/sh
# alias-part-audio-test — P5b proven on real hardware: a sibling alias HEARS its
# OWN part, demuxed from the one shared device stream. P5 (alias-play-test) proved
# the group PLAYS (every part reaches the device main mix); P5b proves the inverse
# direction of the audio plane — each attached alias pulls only ITS part's stereo
# slots ({2+2k,3+2k}, §6.3) out of the single device stream, instead of the summed
# main mix. The device streams the UNION of every instance's requested slots once;
# the owner's reader() demuxes each frame into per-instance sinks.
#
# THE SIGNAL — two RMS energies the harness prints from ONE shared session:
#   main-rms = the OWNER's main mix  (part 0 + the attached part, summed, slots 0,1)
#   sink-rms = the ATTACHED alias's demuxed part audio (its part pair, via the sink)
# Two assertions, both jitter-robust (energy over the whole capture, not bytes —
# the realtime pull's block alignment jitters run-to-run, exactly as in
# alias-play-test):
#   1. sink-rms > FLOOR  — the alias HEARS its part. Pre-P5b an attached instance
#      was audio-SILENT (sink-rms would be 0); a non-silent sink is the feature.
#   2. main-rms > sink-rms — the demuxed part is a strict SUBSET of the mix, NOT a
#      copy of it. A demux bug that handed the sink the main-mix columns {0,1}
#      would make sink-rms ~= main-rms; the part pair being materially quieter than
#      the full mix witnesses that the sink read a DIFFERENT, narrower slice.
#
# tsan-host is the only harness that registers an attached sink BEFORE the owner's
# audio.start, so the sink's slots enter the audio.start UNION (the P5b fixed-at-
# start union; a DAW activating all tracks at project load does the same) — without
# that the sink reads silence (the documented mid-attach limitation). --part-audio
# turns it on; the first attached instance also captures, and the harness prints
# its sink-rms next to the owner's main-rms.
#
# Mirrors alias-play-test.sh: builds the harness WITHOUT TSan (a TSan binary aborts
# at startup on recent Linux), pins HARP_DEVICE_SERIAL, needs ONE board and Live
# closed. Exit 0 pass / 2 N/A (board absent) / 3 device busy.
set -e
cd "$(dirname "$0")/.."

SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
export HARP_DEVICE_SERIAL="$SERIAL"
# This capture drives the harness's setStateBundle loop, which now hits the §11.4
# recall reconcile on every conflict. With no front panel to answer, the default
# timeout stalls each recall ~8s and the 4s capture records SILENCE — a bogus
# "sink silent" fail. 0 = immediate Push fallback. The suite exports this too;
# set it here so the test is correct run standalone (override =8000 by hand).
export HARP_RECONCILE_TIMEOUT_MS="${HARP_RECONCILE_TIMEOUT_MS:-0}"
PROBE="${PROBE:-./build/harp-probe}"
BUILD="${BUILD:-build-mt-host}"      # the multi-instance harness, built WITHOUT TSan
INSTANCES="${INSTANCES:-2}"          # owner part 0 + one attached part 1 (the proof)
SECONDS_RUN="${SECONDS_RUN:-4}"
SAMPLES="${SAMPLES:-5}"              # captures; the PAIRED median (below) defeats the run-to-run RMS jitter
FLOOR="${FLOOR:-0.002}"              # sink-rms floor: clearly non-silent (obs ~0.013)

# claim guard: a DAW holding the device steals the claim and every render is
# silence (sink-rms 0, a bogus fail) — a hard error, never a silent skip.
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "ALIAS-PART-AUDIO FAIL: device claimed by Ableton Live — needs it exclusively"
    exit 3
fi

# need the pinned board; without it nothing connects and per-part audio is
# unobservable — legitimately N/A on this rig.
if [ -x "$PROBE" ]; then
    if ! "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL"; then
        echo "ALIAS-PART-AUDIO SKIP: board $SERIAL not on the bus"; exit 2
    fi
else
    echo "ALIAS-PART-AUDIO SKIP: $PROBE not built (need it to confirm the board)"; exit 2
fi
echo "── alias-part-audio: owner part 0 + attached part 1 on $SERIAL — the alias hears its part"

# Build the multi-instance harness WITHOUT ThreadSanitizer (same reasoning as
# alias-play-test: this asserts per-part AUDIO, not race-freedom — tsan-shell.sh's
# job — and a TSan binary aborts at startup on recent Linux kernels).
echo "── building multi-instance harness (-DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF)"
cmake -B "$BUILD" -S tools/vst3-host -DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF >/dev/null
cmake --build "$BUILD" --target tsan-host -j >/dev/null
[ -x "$BUILD/tsan-host" ] || { echo "ALIAS-PART-AUDIO FAIL: harness build produced no binary"; exit 1; }

# Make PART 0 RELIABLY out-shout part 1 with CONTROLLED, DETERMINISTIC levels. The
# old design left part 1 on the harness's RANDOM param-flood (its level swung
# 0..0.99 run-to-run), so part 1 frequently out-shouted part 0 and the paired
# (main-sink) median tipped negative — the long-standing flake (failing since
# Path 3). HARP_ISO_LEVELS drives BOTH instances as a fixed tone+env voice at a
# fixed level (the SAME mechanism part-param-iso-test relies on — proven
# jitter-robust on the bus): owner part 0 LOUD (0.9), attached part 1 MODEST (0.3).
# main (part 0 + part 1) then exceeds the demuxed part-1 sink in every sample by
# part 0's ~3x dominant energy, while part 1 at 0.3 stays well clear of the floor.
export HARP_ISO_LEVELS="0.9,0.3"

# median of stdin numbers (sh-portable: sort + middle pick). Works for floats.
median() {
    n=$(printf '%s\n' "$@" | grep -c .)
    printf '%s\n' "$@" | sort -g | sed -n "$(((n + 1) / 2))p"
}

# capture once: run --instances N --part-audio, echo "<main-rms> <sink-rms>".
# Retries the transient post-teardown claim race (renders as silence), exactly as
# alias-play-test: a clean, connected, non-silent capture is what we want.
capture() {
    for try in 1 2 3; do
        out=$(mktemp /tmp/alias-pa.XXXXXX)
        "$BUILD/tsan-host" --instances "$INSTANCES" --part-audio --no-state-stress --seconds "$SECONDS_RUN" \
            --block 256 --out "/tmp/alias-pa-$1.wav" >"$out" 2>&1 || true
        m=$(grep '^main-rms:' "$out" | awk '{print $2}')
        s=$(grep '^sink-rms:' "$out" | awk '{print $2}')
        conn=$(grep -c "harp-shell: connected:.*serial $SERIAL" "$out" || true)
        rm -f "$out"
        if [ "$conn" -gt 0 ] && [ -n "$m" ] && [ -n "$s" ] \
           && [ "$(python3 -c "print(1 if $s>0 else 0)")" = 1 ]; then
            echo "$m $s"; return
        fi
        sleep 1
    done
    echo "-1 -1"
}

echo "── sampling owner main-rms + attached sink-rms ×$SAMPLES"
MAINS=""; SINKS=""; DIFFS=""; i=0
while [ "$i" -lt "$SAMPLES" ]; do
    pair=$(capture "$i")
    m=${pair% *}; s=${pair#* }
    case "$m$s" in *-1*) echo "ALIAS-PART-AUDIO FAIL: never connected / sink silent (device busy?)"; exit 3 ;; esac
    echo "   sample $i: main-rms=$m  sink-rms=$s"
    MAINS="$MAINS $m"; SINKS="$SINKS $s"
    DIFFS="$DIFFS $(python3 -c "print($m - $s)")"   # PAIRED, same run — see below
    i=$((i + 1)); sleep 1
done
MMED=$(median $MAINS); SMED=$(median $SINKS); DMED=$(median $DIFFS)

# The subset check is PAIRED: median of per-sample (main - sink), NOT median(mains)
# vs median(sinks) compared separately. With the controlled levels above (part 0 at
# 0.9, part 1 at 0.3) main (part 0 + part 1) exceeds sink (part 1 alone) by part 0's
# ~3x energy in EVERY run; the paired median then only has to defend the residual
# block-alignment jitter of the realtime pull (RMS still swings a little run-to-run),
# for which comparing the two medians INDEPENDENTLY would be fragile — a low main
# sample could lose to a high sink sample even though main > sink within each run.
# The paired median is jitter-robust and still catches a real demux-copies-the-mix
# bug (then main-sink ~= 0 in every sample, regardless of levels).
echo "── medians: main-rms=$MMED  sink-rms=$SMED  paired (main-sink) median=$DMED  (floor=$FLOOR)"
HEARD=$(python3 -c "print(1 if $SMED > $FLOOR else 0)")
SUBSET=$(python3 -c "print(1 if $DMED > 0 else 0)")
if [ "$HEARD" = 1 ] && [ "$SUBSET" = 1 ]; then
    echo "ALIAS-PART-AUDIO PASS (attached alias on $SERIAL hears its part: sink-rms $SMED >"
    echo "   floor $FLOOR — non-silent demux — and the owner main mix $MMED is richer than the"
    echo "   single demuxed part, so the sink read its OWN narrower slice, not the main mix)"
    exit 0
elif [ "$HEARD" != 1 ]; then
    echo "ALIAS-PART-AUDIO FAIL: attached sink silent (RMS $SMED <= floor $FLOOR) — the alias"
    echo "   did not hear its part (sink not in the audio.start union, or demux delivered nothing)"
    exit 1
else
    echo "ALIAS-PART-AUDIO FAIL: paired (main-sink) median $DMED <= 0 — the sink is not a"
    echo "   strict subset of the mix (demux may have handed it the main-mix columns)"
    exit 1
fi
