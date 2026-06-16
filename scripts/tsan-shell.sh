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
# Two devices on the bus exercises the richest interleaving (two live USB
# sessions, claim contention); one device still covers the full runtime +
# the device-less retry path.
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
run multidev --instances 2 --seconds 15 --block 256
[ "$NDEV" -lt 2 ] && echo "  (multidev ran with 1 device: one live + one in device-less retry;" \
    && echo "   connect a second board for two live USB sessions)"

# multitimbral MERGE (P5): PIN one serial so the N instances SHARE one runtime
# and the eventPump drains N per-instance event sources concurrently — this is
# the only config that TSan-covers the multi-source merge (without a pinned
# serial, --instances spins up N independent owners and never merges).
SERIAL=$(./build/harp-probe list 2>/dev/null | grep -oE 'PI4B-[0-9]+' | head -1)
if [ -n "$SERIAL" ]; then
    export HARP_DEVICE_SERIAL="$SERIAL"
    run merge --instances 4 --seconds 15 --block 256
    # P5b: --part-audio adds the per-part AUDIO demux to the merge — the owner's
    # reader() now ALSO splits each device frame into every attached instance's
    # sink ring (one producer, each sink one consumer), on top of the event
    # merge. With per-channel notes the attached parts SOUND, so the demux moves
    # real signal under TSan, not silence. The only config that covers reader()'s
    # multi-sink demux concurrently with the eventPump merge.
    run partaudio --instances 4 --part-audio --seconds 15 --block 256
    unset HARP_DEVICE_SERIAL
else
    echo "  merge: SKIP (no board on the bus to pin for the shared-runtime merge)"
fi

[ "$FAIL" = 0 ] && echo "TSAN-SHELL PASS (runtime threads race-free)" || { echo "TSAN-SHELL FAIL"; exit 1; }
