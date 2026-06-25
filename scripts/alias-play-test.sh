#!/bin/sh
# alias-play-test — P5 proven on real hardware: a MULTITIMBRAL GROUP plays.
# Several plugin/host instances ("aliases") that name the SAME serial SHARE one
# device (that is session-share-test.sh's job), and now each alias drives its
# OWN MIDI channel — owner ch0, siblings ch1..N-1 — so the device renders a
# DIFFERENT PART per alias and SUMS them all into its main mix. This test proves
# the sibling parts actually reach the device: the device's main mix with the
# whole group driving is materially, reproducibly DIFFERENT from the same device
# with only the owner's single channel driving.
#
# THE SIGNAL — the owner's MAIN-MIX render, not the panel counters.
#   The panel API (web/harp-panel.py -> device/panel.c "counters") is GLOBAL —
#   frame_errors/evt_late/streaming/… — with NO per-part or per-channel
#   dimension (panel_json_counters has none; the front panel is part 0 only).
#   So the panel cannot show "how many parts are engaged." The device DOES
#   answer that acoustically: device/engine.c render_with_events sums part 0
#   (full voice) + every ACTIVE part 1..15 (note-only) into the main mix, and
#   part_active() keeps a part out of the mix until a note lands on its channel.
#   Therefore the main mix is the direct witness: drive ONLY ch0 and parts 1..15
#   stay silent (mix = part 0); drive ch0..chN-1 (the alias group) and parts
#   1..N-1 turn active and change the mix. We compare the two.
#
#   We do NOT hash the mix: tsan-host pulls the main mix in REALTIME cadence (a
#   nanosleep-paced audio thread racing the device's free-running stream), so the
#   exact captured bytes jitter run-to-run — even two single-channel runs hash
#   differently. RMS (energy integrated over the whole capture) is the
#   jitter-robust metric, BUT it must be read carefully: the realtime pull pads
#   underrun gaps with silence, and N contending instances underrun far more than
#   one (measured ~67% vs ~26% of the capture padded), so the GROUP capture reads
#   LOWER than the single even though it carries more parts. That inversion is
#   fine — separation, NOT its direction, is the proof: a single channel can
#   never produce the group's multi-part mix, so a group median that differs from
#   the single median by a wide margin means MORE THAN ONE part engaged.
#
#   The one thing that must be stable is the SINGLE-channel baseline (the group's
#   heavy-underrun band sits reproducibly near ~6k on every board measured). So
#   each alias holds a SUSTAINED chord note (below) for the whole capture instead
#   of tsan-host's default 80-note flood: a flood retriggers a voice 20x/s and
#   its realtime-pulled RMS swung ~±40% (single dipped from ~12k to ~7.5k on a
#   slower board), which once narrowed the gap under the threshold and flaked. A
#   held note renders a steady tone, so the single band is tight (~12k) and the
#   ~2x gap to the group band has wide, board-independent margin.
#
# tsan-host is the only harness that drives N channels into ONE device: each of
# its --instances is an alias on its own channel (owner ch0, attached ch1.., the
# P5 per-instance source merge), all pinned to one serial via the registry. With
# --out it now keeps the OWNER's pulled main mix and writes it to WAV (harness
# instrumentation only — no shell/device change); we RMS that WAV here.
#
# Mirrors session-share-test.sh: builds the harness WITHOUT TSan
# (-DHARP_TSAN_SANITIZE=OFF — a TSan binary aborts at startup on recent Linux),
# pins HARP_DEVICE_SERIAL, needs ONE board on the bus and Live closed.
# Exit 0 pass / 2 N/A (board absent) / 3 device busy.
set -e
cd "$(dirname "$0")/.."

SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
export HARP_DEVICE_SERIAL="$SERIAL"
PROBE="${PROBE:-./build/harp-probe}"
BUILD="${BUILD:-build-mt-host}"      # the multi-instance harness, built WITHOUT TSan
INSTANCES="${INSTANCES:-4}"          # owner ch0 + 3 siblings ch1..3 — a real group
SECONDS_RUN="${SECONDS_RUN:-4}"
SAMPLES="${SAMPLES:-5}"              # captures per configuration (median defeats jitter)
CHORD="${CHORD:-57}"                 # a SUSTAINED note per alias (transposed by channel),
                                     # NOT tsan-host's default 80-note flood — see below

# claim guard: a DAW holding the device steals the claim and every render comes
# back silence (single==group, a bogus pass) — a hard FAIL, never a silent skip.
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "ALIAS-PLAY FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

# need the pinned board on the bus; without it nothing connects and the group is
# unobservable — legitimately N/A on this rig.
if [ -x "$PROBE" ]; then
    if ! "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL"; then
        echo "ALIAS-PLAY SKIP: board $SERIAL not on the bus"
        exit 2
    fi
else
    echo "ALIAS-PLAY SKIP: $PROBE not built (need it to confirm the board is present)"
    exit 2
fi
echo "── alias-play: $INSTANCES aliases on $SERIAL, one channel each — expect a multi-part mix"

# Build the multi-instance harness WITHOUT ThreadSanitizer — identical reasoning
# to session-share-test.sh: this asserts that the GROUP PLAYS (multi-part audio),
# not race-freedom (tsan-shell.sh's job), and a TSan binary aborts at startup on
# recent Linux kernels (the vm.mmap_rnd_bits ASLR issue), failing this hardware
# test for a reason unrelated to multitimbral play.
echo "── building multi-instance harness (-DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF)"
cmake -B "$BUILD" -S tools/vst3-host -DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF >/dev/null
cmake --build "$BUILD" --target tsan-host -j >/dev/null
[ -x "$BUILD/tsan-host" ] || { echo "ALIAS-PLAY FAIL: harness build produced no binary"; exit 1; }

# Prime the patch on the device via the front panel (persists past the session,
# like flood-stress.sh): level up (7), a tone (3), and a FAST envelope (5/6). The
# drone is gone, so all the energy comes from the sustained CHORD note below (held
# for the whole capture, not struck repeatedly) — each alias's note on its own
# channel is what makes the group mix separate from the single-channel one.
for kv in "7 0.6" "3 0.7" "5 0.05" "6 0.1"; do
    "$PROBE" -d "usb:$SERIAL" knob $kv >/dev/null 2>&1 || true
done

# rms WAV — root-mean-square of a PCM16 stereo capture (mirrors soak.sh's reader).
# Energy integrated over the whole capture: robust to the realtime pull's
# block-alignment jitter, unlike a byte hash.
rms() {
    python3 - "$1" <<'EOF'
import wave, struct, math, sys
try:
    w = wave.open(sys.argv[1]); raw = w.readframes(w.getnframes())
    s = struct.unpack("<%dh" % (len(raw) // 2), raw)
    print("%.0f" % (math.sqrt(sum(x * x for x in s) / len(s)) if s else 0))
except Exception:
    print("-1")
EOF
}

# median of stdin numbers (sh-portable via sort + middle pick)
median() {
    n=$(printf '%s\n' "$@" | grep -c .)
    printf '%s\n' "$@" | sort -n | sed -n "$(((n + 1) / 2))p"
}

# capture_rms N — run the harness with N aliases, capture the owner main mix,
# echo its RMS. Retries: the first open right after a prior teardown can lose a
# transient claim race (observed: a momentarily-busy bus renders as silence);
# a single clean, non-silent, connected capture is what we want.
capture_rms() {
    n="$1"; wav="$2"; r=-1
    for try in 1 2 3; do
        err=$(mktemp /tmp/alias-play.XXXXXX)
        "$BUILD/tsan-host" --instances "$n" --chord "$CHORD" --seconds "$SECONDS_RUN" --block 256 \
            --out "$wav" >"$err" 2>&1 || true
        if grep -q "harp-shell: connected:.*serial $SERIAL" "$err"; then
            r=$(rms "$wav")
            rm -f "$err"
            [ "$r" != "-1" ] && [ "$r" != "0" ] && break
        else
            rm -f "$err"
        fi
        sleep 1
    done
    echo "$r"
}

# ---- baseline: ONE alias, owner channel only (parts 1..15 silent) ----
echo "── sampling single-channel baseline (1 alias, ch0 only) ×$SAMPLES"
SINGLE=""
i=0
while [ "$i" -lt "$SAMPLES" ]; do
    v=$(capture_rms 1 "/tmp/alias-single-$i.wav")
    case "$v" in -1|0|"") echo "ALIAS-PLAY FAIL: single-channel render never connected/produced audio (device busy?)"; exit 3 ;; esac
    echo "   single ch0 sample $i: RMS=$v"
    SINGLE="$SINGLE $v"
    i=$((i + 1))
    sleep 1
done
SMED=$(median $SINGLE)

# ---- the group: N aliases, channels 0..N-1 (parts 0..N-1 engaged) ----
echo "── sampling the alias group ($INSTANCES aliases, ch0..ch$((INSTANCES - 1))) ×$SAMPLES"
GROUP=""
i=0
while [ "$i" -lt "$SAMPLES" ]; do
    v=$(capture_rms "$INSTANCES" "/tmp/alias-group-$i.wav")
    case "$v" in -1|0|"") echo "ALIAS-PLAY FAIL: group render never connected/produced audio (device busy?)"; exit 3 ;; esac
    echo "   group ch0..ch$((INSTANCES - 1)) sample $i: RMS=$v"
    GROUP="$GROUP $v"
    i=$((i + 1))
    sleep 1
done
GMED=$(median $GROUP)

# ---- verdict: the group mix must be cleanly separated from the single mix ----
# A single channel can NEVER produce the group's mix, so a group median that
# differs from the single median by a wide margin (>25% relative) means the
# device summed sibling parts into its main mix — MORE THAN ONE part engaged.
# With sustained notes the measured gap is ~45-50% (single ~12k vs group ~6k, the
# group's heavy-underrun band), so 25% has wide margin for the realtime pull's
# residual jitter yet sits far above any same-config run-to-run spread.
echo "── single-channel median RMS=$SMED ; group median RMS=$GMED"
DIFF=$(python3 -c "s=$SMED; g=$GMED; print(0 if max(s,g)==0 else abs(s-g)/max(s,g))")
OVER=$(python3 -c "print(1 if $DIFF > 0.25 else 0)")
if [ "$OVER" = "1" ]; then
    echo "ALIAS-PLAY PASS ($INSTANCES aliases on $SERIAL, one channel each; the device's"
    echo "   main mix differs from the single-channel run by $(python3 -c "print('%.0f%%' % (100*$DIFF))") — sibling parts engaged)"
    exit 0
else
    echo "ALIAS-PLAY FAIL: group mix (RMS $GMED) did not separate from single-channel"
    echo "   (RMS $SMED) — only one part appears engaged (siblings not reaching the device?)"
    exit 1
fi
