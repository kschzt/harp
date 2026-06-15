#!/bin/sh
# session-share-test — the P4 registry, proven on real hardware: N plugin/host
# instances that name the SAME serial must SHARE one runtime — ONE USB session,
# ONE claim — instead of fighting for an exclusive device (the second would
# lose). This is the prerequisite for multitimbral aliasing (P5: several shells,
# one device, one part each), so it has to hold on the bus, not just in theory.
#
# How: tsan-host obtains its runtimes from the SAME process-global registry the
# plugin uses (tools/tsan-host/main.cpp). With HARP_DEVICE_SERIAL pinned and
# --instances 2, both instances ask the registry for the SAME serial: the first
# is the OWNER (creates the runtime, connects, claims the device exactly once);
# the second ATTACHES to that shared runtime and drives nothing (dormant in P4).
# So the proof is one of UNIQUENESS under contention: two instances pinned to
# one exclusive unit yield exactly ONE USB claim and exactly ONE connected
# session. Without the registry the second instance would open its OWN runtime
# and either claim a second time (impossible on an exclusive device — the second
# loses) or stand up a second supervisor connecting on the same serial; either
# breaks the "exactly one" count this test asserts.
#
# We assert against tsan-host's stderr (it logs the instance/serial banner, and
# the runtime logs its per-session connect + the libusb claim) and corroborate
# with harp-probe (the board on the bus, claimable again when we are done).
#
# Mirrors multidevice-test.sh / tsan-shell.sh conventions. Needs ONE board on
# the bus and Live closed. Exit 0 pass / 2 N/A (board absent) / 3 device busy.
set -e
cd "$(dirname "$0")/.."

# Pin one board, exactly like hw-tests.sh (desk unit PI4B-0001; the Linux rig
# overrides to PI4B-0002 via the env). This serial is the shared key under test.
SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
export HARP_DEVICE_SERIAL="$SERIAL"
PROBE="${PROBE:-./build/harp-probe}"
BUILD="${BUILD:-build-mt-host}"      # the multi-instance harness, built WITHOUT TSan
INSTANCES="${INSTANCES:-2}"
SECONDS_RUN="${SECONDS_RUN:-6}"
# X's must be the template TAIL: BSD mktemp does not expand them when a suffix
# follows (it would create a literal "...XXXXXX.log" and collide run-to-run).
LOG=$(mktemp /tmp/session-share.XXXXXX)

# claim guard: a DAW holding the device makes this test lie (it would steal the
# claim we are trying to count) — a hard FAIL, never a silent skip.
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "SESSION-SHARE FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

# need the pinned board on the bus; without it the owner never connects and the
# share is unobservable — legitimately N/A on this rig.
if [ -x "$PROBE" ]; then
    if ! "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL"; then
        echo "SESSION-SHARE SKIP: board $SERIAL not on the bus"
        exit 2
    fi
else
    echo "SESSION-SHARE SKIP: $PROBE not built (need it to confirm the board is present)"
    exit 2
fi
echo "── session-share: $INSTANCES instances pinned to $SERIAL — expect ONE shared claim"

# Build the multi-instance harness: the same tsan-host main.cpp + the shell
# runtime + the registry, but WITHOUT ThreadSanitizer (-DHARP_TSAN_SANITIZE=OFF).
# This test asserts session SHARING (one claim for N instances), NOT race-
# freedom — that is tsan-shell.sh's job — and a TSan binary aborts at startup on
# recent Linux kernels (the vm.mmap_rnd_bits ASLR mapping issue), which would
# fail this hardware test for a reason that has nothing to do with sharing.
echo "── building multi-instance harness (-DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF)"
cmake -B "$BUILD" -S tools/vst3-host -DHARP_TSAN=ON -DHARP_TSAN_SANITIZE=OFF >/dev/null
cmake --build "$BUILD" --target tsan-host -j >/dev/null
[ -x "$BUILD/tsan-host" ] || { echo "SESSION-SHARE FAIL: harness build produced no binary"; exit 1; }

# Run N instances on the one serial. All but the first attach to the owner's
# shared runtime; only the owner claims/connects.
"$BUILD/tsan-host" --instances "$INSTANCES" --seconds "$SECONDS_RUN" --block 256 \
    >"$LOG" 2>&1 || true

# 1. the harness banner confirms it really ran N instances on OUR serial — if
#    the serial weren't pinned, every instance would auto-select its own fresh
#    runtime (the multi-device path) and the share would not be exercised.
grep -q "tsan-host: $INSTANCES instance(s).*serial=\"$SERIAL\"" "$LOG" || {
    echo "SESSION-SHARE FAIL: harness did not run $INSTANCES instances on serial $SERIAL"
    grep -E "^tsan-host:" "$LOG" | head -2; exit 1; }

# 2. exactly ONE USB claim, and it is OUR board. Two independent runtimes on one
#    exclusive unit could never both claim it — sharing is the only way one claim
#    serves N instances. (claimed-line format matches multidevice-test.sh.)
NCLAIM=$(grep -cE "harp-usb: claimed 1209:[0-9a-f]+ serial $SERIAL([^0-9]|$)" "$LOG" || true)
NCLAIM_OTHER=$(grep -E "harp-usb: claimed 1209:[0-9a-f]+ serial PI4B-[0-9]+" "$LOG" \
    | grep -cvE "serial $SERIAL([^0-9]|$)" || true)
if [ "$NCLAIM" != "1" ]; then
    echo "SESSION-SHARE FAIL: expected exactly 1 claim of $SERIAL, got $NCLAIM"
    grep -E "harp-usb: claimed|no HARP device" "$LOG"; exit 1
fi
[ "$NCLAIM_OTHER" = "0" ] || {
    echo "SESSION-SHARE FAIL: an instance claimed a different unit than $SERIAL"
    grep -E "harp-usb: claimed" "$LOG"; exit 1; }
echo "   exactly ONE USB claim, on $SERIAL (the shared session) ✓"

# 3. exactly ONE session connected, to OUR board: only the owner runtime runs a
#    supervisor and connects; the attached instance(s) never open a session of
#    their own. Without sharing each instance would drive its own runtime, so
#    the device-less ones would keep a SECOND supervisor connecting on the same
#    serial — there would be more than one connect, or a permanently-failing
#    sibling. (Note: a SINGLE owner legitimately logs transient "no HARP device
#    with serial / cannot claim interface" lines while its supervisor retries a
#    momentarily-busy bus at startup — those are the owner's own retries, NOT a
#    second instance, so we count CONNECTS, the positive signal, not misses.)
NCONN=$(grep -cE "harp-shell: connected:.*serial $SERIAL([^0-9]|$)" "$LOG" || true)
NCONN_OTHER=$(grep -E "harp-shell: connected:.*serial PI4B-[0-9]+" "$LOG" \
    | grep -cvE "serial $SERIAL([^0-9]|$)" || true)
if [ "$NCONN" != "1" ]; then
    echo "SESSION-SHARE FAIL: expected exactly 1 session connected to $SERIAL, got $NCONN"
    grep -E "harp-shell: connected:" "$LOG"; exit 1
fi
[ "$NCONN_OTHER" = "0" ] || {
    echo "SESSION-SHARE FAIL: a session connected to a unit other than $SERIAL"
    grep -E "harp-shell: connected:" "$LOG"; exit 1; }
echo "   exactly ONE connected session, on $SERIAL — the other instance(s) shared it ✓"

# 4. clean exit corroboration: the harness tore the shared runtime down (last
#    release stops+destroys it) and the board is claimable again afterwards.
grep -q "tsan-host: done" "$LOG" || {
    echo "SESSION-SHARE FAIL: harness did not exit cleanly"; tail -5 "$LOG"; exit 1; }
if ! "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL"; then
    echo "SESSION-SHARE FAIL: $SERIAL not back on the bus after teardown (leaked claim?)"
    exit 1
fi
echo "   shared runtime torn down on last release; $SERIAL claimable again ✓"

echo "SESSION-SHARE PASS ($INSTANCES instances on $SERIAL shared ONE claim / ONE session)"
