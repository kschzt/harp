#!/bin/sh
# tsan-shell — ThreadSanitize the embedded shell runtime against a real
# device (debt #17). The shell's threads (supervisor/feeder, reader, event
# pump, libusb's async event thread) plus a simulated DAW audio + control
# thread are driven under -fsanitize=thread by tools/tsan-host, which
# statically links runtime.cpp (TSan needs to be in the main image, not a
# dlopen'd plugin bundle).
#
# Not a cloud-CI gate — it needs a real board on the bus. Run on the rig or
# any machine with a HARP device. Clean = no "WARNING: ThreadSanitizer".
#
# M3 multi-out model: tools/tsan-host acquires ONE private owner runtime and
# --instances N drives N device PARTS on channels 0..N-1 (the share-by-serial
# registry is gone — there is no second in-process session to interleave). One
# board on the bus covers the full runtime (feeder/reader/eventPump/part-sinks)
# under load; with no board the device-less connect/retry path still gets
# sanitized.
set -e
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build-tsan-shell}
SUPP="$(cd "$(dirname "$0")" && pwd)/tsan-shell.supp"
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1${TSAN_OPTIONS:+ $TSAN_OPTIONS}"
[ -f "$SUPP" ] && export TSAN_OPTIONS="$TSAN_OPTIONS suppressions=$SUPP"

echo "── tsan-shell: building harness (-fsanitize=thread)"
cmake -B "$BUILD" -S tools/vst3-host -DHARP_TSAN=ON >/dev/null
cmake --build "$BUILD" --target tsan-host -j >/dev/null

NDEV=$(./build/harp-probe list 2>/dev/null | grep -c 'serial PI4B-' || echo 0)
echo "── $NDEV HARP device(s) on the bus"

run() {
    name=$1; shift
    # X's must be the template TAIL: BSD mktemp does not expand them when a
    # suffix (.log) follows — it would create a literal "...XXXX.log" that
    # collides run-to-run and exits the wrapper early under `set -e`.
    log=$(mktemp /tmp/tsan-$name.XXXXXX)
    "$BUILD/tsan-host" "$@" >"$log" 2>&1 || true
    n=$(grep -c 'WARNING: ThreadSanitizer' "$log" || true)
    if [ "$n" -gt 0 ]; then
        echo "  $name: $n ThreadSanitizer WARNING(s) — see $log"
        grep -E 'SUMMARY: ThreadSanitizer' "$log" | sort -u | sed 's/^/    /'
        FAIL=1
    else
        echo "  $name: clean"
    fi
}

FAIL=0
run single   --instances 1 --seconds 12 --block 256
run tight    --instances 1 --seconds 15 --block 64
run twoparts --instances 2 --seconds 15 --block 256   # one owner driving 2 parts

# multitimbral PARTS (P5): pin a board and drive ONE owner across 4 parts. The
# eventPump drains the single owner source while the feeder/reader service all 4
# parts' rings concurrently — the densest event+audio config the runtime
# sanitizes. (The old "N instances SHARE one runtime / merge N per-instance
# sources" is gone with the registry: there is exactly one owner now.)
SERIAL=$(./build/harp-probe list 2>/dev/null | grep -oE 'PI4B-[0-9]+' | head -1)
if [ -n "$SERIAL" ]; then
    export HARP_DEVICE_SERIAL="$SERIAL"
    run parts --instances 4 --seconds 15 --block 256
    # P5b: --part-audio adds the per-part AUDIO demux to the merge — the owner's
    # reader() now ALSO splits each device frame into every attached instance's
    # sink ring (one producer, each sink one consumer), on top of the event
    # merge. With per-channel notes the attached parts SOUND, so the demux moves
    # real signal under TSan, not silence. The only config that covers reader()'s
    # multi-sink demux concurrently with the eventPump merge.
    run partaudio --instances 4 --part-audio --seconds 15 --block 256
    # P5b RE-NEGOTIATION (--late-sink): the attached sinks register AFTER the owner
    # is already streaming, so each lands in the registry but NOT the live
    # audio.start union — the feeder must RE-STREAM the wider union mid-session
    # (audio.stop -> new union -> audio.start) on the CONTINUOUS SSI domain. This
    # is the only config that TSan-covers the re-neg's reader quiesce/respawn, the
    # monotonic-fence epoch baseline (no store(0) racing queue*), and the sink
    # epoch reset — concurrently with the live reader, eventPump and audio pulls.
    # HARP_LATE_SETTLE_MS=0: register the sink the INSTANT the session is up, so
    # the re-neg's reader quiesce/respawn + fence-baseline + sink-epoch transitions
    # run while the link is live. NOTE: under heavy instrumentation the device often
    # times the slow TSan host out DURING the re-neg's audio.stop/start, so the
    # audio.start may not COMPLETE (you'll see "re-negotiation: audio.start failed
    # / device gone"). That is fine for THIS config: what TSan must watch is the
    # concurrent STATE TRANSITIONS (the feeder quiescing+respawning the reader,
    # storing the epoch baseline, resetting sink epochs) racing the live reader /
    # eventPump / audio pulls — that race surface executes whether or not the wire
    # audio.start lands. The FUNCTIONAL "the late sink hears its part" reliability
    # is the non-TSan late-sink-test.sh's job. 8 s (vs 15) keeps the heavily-
    # instrumented run practical — the re-neg attempt happens once, right at start.
    export HARP_LATE_SETTLE_MS=0
    run latesink --instances 4 --late-sink --seconds 8 --block 256
    unset HARP_LATE_SETTLE_MS
    unset HARP_DEVICE_SERIAL
else
    echo "  parts: SKIP (no board on the bus to pin the owner runtime)"
fi

[ "$FAIL" = 0 ] && echo "TSAN-SHELL PASS (runtime threads race-free)" || { echo "TSAN-SHELL FAIL"; exit 1; }
