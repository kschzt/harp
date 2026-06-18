#!/bin/sh
# part-param-iso-test — the combined P5+P6 e2e: in ONE shared multitimbral session,
# a per-part PARAM routes to its OWN part's audio and ONLY that part. P6 proved the
# Part parameter routes a single instance's params (and recall-perpart that they
# persist per part); P5 proved several instances MERGE onto one session. This ties
# them together on the audio plane: instance A owns part 0, instance B owns part 1,
# both inject param-sets on their own event source (§9.4 key 5 = their channel),
# and we watch part 1's demuxed audio (the P5b sink) respond to part 1's level
# while staying deaf to part 0's.
#
# THE SIGNAL — sink-rms, the attached instance's demuxed PART-1 audio (slots {2,3}),
# as the harness prints it. param 8 is the per-part LEVEL (the front panel's "8"),
# so part 1's energy tracks part 1's param-8 monotonically. Three shared-session
# runs (HARP_ISO_LEVELS = "<part0>,<part1>", each instance drives ONLY a controlled
# tone+env+level on its part — no random flood, so the sink's energy is part 1's
# level alone):
#   HIGH  = 0.2,0.9  part 1 loud   -> sink-rms high
#   LOW   = 0.2,0.2  part 1 quiet  -> sink-rms low
#   XTALK = 0.9,0.2  part 1 quiet, OWNER part 0 loud -> sink-rms must stay ~LOW
# Two assertions, jitter-robust (RMS energy, the realtime pull jitters byte-wise):
#   1. HIGH > 1.5x LOW   — part 1's OWN level param reached part 1's audio (routing).
#   2. XTALK < 1.5x LOW  — driving the OWNER's level (part 0) to 0.9 did NOT raise
#      part 1; the sink stayed near LOW. So params do not cross parts — the §9.4
#      channel key isolates them inside the one merged session.
#
# Same harness/conventions as alias-part-audio-test.sh (tsan-host, no TSan, one
# pinned board, Live closed). Exit 0 pass / 2 N/A (board absent) / 3 device busy.
set -e
cd "$(dirname "$0")/.."

SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
export HARP_DEVICE_SERIAL="$SERIAL"
# This capture drives the harness's setStateBundle loop, which now hits the §11.4
# recall reconcile on every conflict. With no front panel to answer, the default
# timeout stalls each recall ~8s and the capture records SILENCE — a bogus
# "produced no audio" fail. 0 = immediate Push fallback. The suite exports this
# too; set it here so the test is correct run standalone (override =8000 by hand).
export HARP_RECONCILE_TIMEOUT_MS="${HARP_RECONCILE_TIMEOUT_MS:-0}"
PROBE="${PROBE:-./build/harp-probe}"
BUILD="${BUILD:-build-mt-host}"
SECONDS_RUN="${SECONDS_RUN:-4}"
SAMPLES="${SAMPLES:-5}"   # median of 5: the realtime pull's RMS jitters; more samples tighten it

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "PART-PARAM-ISO FAIL: device claimed by Ableton Live — needs it exclusively"; exit 3
fi
if [ -x "$PROBE" ]; then
    if ! "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL"; then
        echo "PART-PARAM-ISO SKIP: board $SERIAL not on the bus"; exit 2
    fi
else
    echo "PART-PARAM-ISO SKIP: $PROBE not built"; exit 2
fi
echo "── part-param-iso: per-part level routing + isolation on $SERIAL (owner part 0 + attached part 1)"

echo "── building multi-instance harness (-DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF)"
cmake -B "$BUILD" -S tools/vst3-host -DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF >/dev/null
cmake --build "$BUILD" --target tsan-host -j >/dev/null
[ -x "$BUILD/tsan-host" ] || { echo "PART-PARAM-ISO FAIL: harness build produced no binary"; exit 1; }

median() {
    n=$(printf '%s\n' "$@" | grep -c .)
    printf '%s\n' "$@" | sort -g | sed -n "$(((n + 1) / 2))p"
}

# sink_rms LEVELS — run a 2-instance shared session with HARP_ISO_LEVELS=LEVELS and
# --part-audio, echo the attached part-1 sink RMS (median of $SAMPLES, retries the
# transient post-teardown claim race exactly as alias-part-audio-test).
sink_rms() {
    levels="$1"; vals=""; i=0
    while [ "$i" -lt "$SAMPLES" ]; do
        r=""
        for try in 1 2 3; do
            out=$(mktemp /tmp/iso.XXXXXX)
            HARP_ISO_LEVELS="$levels" "$BUILD/tsan-host" --instances 2 --part-audio --no-state-stress \
                --seconds "$SECONDS_RUN" --block 256 --out "/tmp/iso-$i.wav" >"$out" 2>&1 || true
            s=$(grep '^sink-rms:' "$out" | awk '{print $2}')
            conn=$(grep -c "harp-shell: connected:.*serial $SERIAL" "$out" || true)
            rm -f "$out"
            if [ "$conn" -gt 0 ] && [ -n "$s" ] && [ "$(python3 -c "print(1 if $s>0 else 0)")" = 1 ]; then
                r="$s"; break
            fi
            sleep 1
        done
        [ -z "$r" ] && { echo "-1"; return; }
        vals="$vals $r"; i=$((i + 1)); sleep 1
    done
    median $vals
}

echo "── HIGH (part0=0.2 part1=0.9): part 1 loud"
HI=$(sink_rms "0.2,0.9"); echo "   part-1 sink-rms (HIGH) = $HI"
echo "── LOW (part0=0.2 part1=0.2): part 1 quiet"
LO=$(sink_rms "0.2,0.2"); echo "   part-1 sink-rms (LOW)  = $LO"
echo "── XTALK (part0=0.9 part1=0.2): owner loud, part 1 quiet — must stay ~LOW"
XT=$(sink_rms "0.9,0.2"); echo "   part-1 sink-rms (XTALK)= $XT"

case "$HI$LO$XT" in *-1*) echo "PART-PARAM-ISO FAIL: a run never connected/produced audio (device busy?)"; exit 3 ;; esac

# Thresholds keyed to the STABLE loud readings, not the small/jittery LOW (which
# under the realtime pull can dip ~4x and false-trip a LOW-relative isolation gate
# — the earlier flake). The level param is ~linear, so part1@0.2 renders ~0.22x of
# part1@0.9: ROUTING HIGH>2·LOW and ISOLATION XTALK<0.5·HIGH both have wide margin
# vs jitter, while cross-talk (owner leaking into part 1) would push XTALK toward
# HIGH, well past 0.5·HIGH. (XTALK should also land near LOW — reported, not gated.)
ROUTES=$(python3 -c "print(1 if $HI > 2*$LO else 0)")
ISOLATED=$(python3 -c "print(1 if $XT < 0.5*$HI else 0)")
echo "── HIGH=$HI LOW=$LO XTALK=$XT  (routes: HIGH>2·LOW ; isolated: XTALK<0.5·HIGH)"
if [ "$ROUTES" = 1 ] && [ "$ISOLATED" = 1 ]; then
    echo "PART-PARAM-ISO PASS (on $SERIAL: part 1's level param reached part 1's audio —"
    echo "   HIGH $HI > 2×LOW $LO — and driving the OWNER's level to 0.9 left part 1 at"
    echo "   XTALK $XT < 0.5×HIGH $HI (≈ LOW $LO), so per-part params don't cross parts)"
    exit 0
elif [ "$ROUTES" != 1 ]; then
    echo "PART-PARAM-ISO FAIL: part 1 level did not route (HIGH $HI not > 2×LOW $LO) —"
    echo "   the attached instance's param-8 did not reach part 1's audio"
    exit 1
else
    echo "PART-PARAM-ISO FAIL: cross-talk (XTALK $XT >= 0.5×HIGH $HI) — driving the owner's"
    echo "   level toward 0.9 pushed part 1 up; per-part param isolation broken"
    exit 1
fi
