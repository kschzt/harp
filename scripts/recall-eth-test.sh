#!/bin/bash
# recall-eth-test — the shell recall round-trip + §11.4 archive against FAKE HARDWARE
# (harp-deviced --port), the §8.7-Ethernet complement to recall-test.sh.
#
# recall-test.sh needs the real rig's web panel (curl :8080/api/*) for the musician
# mutation + verification, so it only runs on the hardware rigs. This variant drives
# the SAME semantic against the localhost loopback device — no web panel, no USB:
#   1. set known params via the shell (HARP_ETH_DEVICE), save plugin state (DAW save)
#   2. mutate the device front-panel side via harp-probe knob (the "musician")
#   3. load plugin state (DAW reopen) -> auto-Push-with-archive
#   4. assert params restored AND an archive ref captured the mutation
# Runs on any cloud runner (eth.yml). Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."

# Interactive recall path so the displaced device state is ARCHIVED before the Push
# (the headless TIMEOUT_MS=0 path skips the archive). No panel answers the loopback, so
# it falls back to Push after a brief wait — fast, and it archives. (Same as recall-test.)
export HARP_RECONCILE_TIMEOUT_MS=1000

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PROBE="${PROBE:-./build/harp-probe}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47902}"
# State dir + file are passed as ARGS to the native device/host exes; keep them
# workspace-RELATIVE so MSYS doesn't path-convert an absolute /tmp arg into a
# C:\...\ drive path the MinGW device's recursive mkdir can't create on Windows
# (logs stay in /tmp — those are bash/python redirections, not exe args, and
# recall-eth-dev.log is grepped by eth-asan.yml).
STATEDIR=recall-eth-state
STATEFILE=recall-eth.state
# Also workspace-relative: written by bash sed, then read by python3 — which on the
# Windows runner is NATIVE python, so a '/tmp/...' string inside the python code is
# resolved against C:\ (no MSYS conversion of in-code path strings), not Git Bash's
# /tmp. A relative path resolves to the same CWD on both sides.
PARAMS=recall-eth.params
fail() { echo "RECALL-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 bundle not found"

# start the fake-hardware loopback device on 127.0.0.1:$PORT
rm -rf "$STATEDIR" "$STATEFILE"; : > /tmp/recall-eth-dev.log
"$DEVICED" --port "$PORT" --state-dir "$STATEDIR" >/tmp/recall-eth-dev.log 2>&1 &
DEVPID=$!
trap 'kill -9 "$DEVPID" 2>/dev/null' EXIT INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/recall-eth-dev.log 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" /tmp/recall-eth-dev.log || { cat /tmp/recall-eth-dev.log; fail "device didn't start on $PORT"; }

export HARP_ETH_DEVICE="127.0.0.1:$PORT"
export HARP_DEVICE_SERIAL="SIM-0001"   # the --port daemon's serial (the connect-check matches it)
PD="-d $HARP_ETH_DEVICE"
arch_count() { "$PROBE" $PD refs 2>/dev/null | grep -c "archive/"; }

echo "── recall over §8.7 loopback: device $DEVICED on 127.0.0.1:$PORT, shell over Ethernet"
"$HOSTBIN" "$PLUG" --seconds 0.05 2>&1 | grep -q "connected:" \
    || fail "cannot connect to the loopback device (HARP_ETH_DEVICE=$HARP_ETH_DEVICE)"

ARCH0=$(arch_count)
# EXACT-HASH RECALL GATE (audit gap #4): the param check below is a 2% FLOAT tolerance — a
# recall codec that rounds/quantizes the low bits restores 0.81->0.815 and sails through it.
# So ALSO assert the RESTORED device renders the golden note line BYTE-IDENTICALLY to the
# pre-mutation device. HPRE = a DIRECT --set render (ground truth, no codec); HPOST = a
# render AFTER --load-state (through the full save->restore codec). Lossless recall =>
# HPRE==HPOST; a lossy/rounding restore shifts the audio and fails HERE even within 2%.
# Renders are kOffline => §8.7 host-paced => byte-exact run-to-run; the hashes live in shell
# vars off stdout, so the Windows native-python /tmp rule doesn't apply to them.
# Warm-up: the FIRST full note-render after a fresh device start is a one-time cold transient
# (metering/echo settle) — discard one so HPRE (the save render) is in the stable regime
# HPOST will also be in.
"$HOSTBIN" "$PLUG" --set 3=0.81 --set 7=0.31 --set 1=0.61 --notes 62,69,74,65 \
    --seconds 0.6 --hash >/dev/null 2>&1   # warm: settle the device, discard
# 1. known state, RENDERED (HPRE = ground-truth render) + saved (DAW save)
HPRE=$("$HOSTBIN" "$PLUG" --set 3=0.81 --set 7=0.31 --set 1=0.61 --notes 62,69,74,65 \
       --seconds 0.6 --hash --save-state "$STATEFILE" 2>/dev/null | sed -n 's/^output-hash: //p')
[ -n "$HPRE" ] || fail "no pre-mutation render-hash (save render produced no output-hash)"
# 2. musician mutates the device (front-panel path, via the probe)
"$PROBE" $PD knob 3 0.10 >/dev/null 2>&1 || fail "knob 3 mutate"
"$PROBE" $PD knob 7 0.95 >/dev/null 2>&1 || fail "knob 7 mutate"
# 3. DAW reopen -> recall (auto-Push-with-archive); HPOST = render of the RESTORED device
"$HOSTBIN" "$PLUG" --load-state "$STATEFILE" --notes 62,69,74,65 --seconds 0.6 --hash \
    >/tmp/recall-eth.log 2>&1 || fail "load render"
grep -q "restored\|SYNCED" /tmp/recall-eth.log || fail "no recall action logged"
HPOST=$(sed -n 's/^output-hash: //p' /tmp/recall-eth.log)
[ -n "$HPOST" ] || fail "no post-restore render-hash (load render produced no output-hash)"
# 3'. the EXACT gate: the restored render must equal the ground-truth render byte-for-byte
[ "$HPRE" = "$HPOST" ] || fail "EXACT-HASH mismatch: restored $HPOST != ground-truth $HPRE (params within 2% but the bounce changed — a lossy/rounding recall)"
# 4. params restored AND the mutation was archived
"$PROBE" $PD params 2>/dev/null | sed -nE 's/^ *\[([0-9]+)\].*[[:space:]]([0-9.]+)$/\1 \2/p' > "$PARAMS"
PARAMS="$PARAMS" python3 -c "
import os
v = {}
for ln in open(os.environ['PARAMS']):
    a = ln.split()
    if len(a) == 2:
        try: v[int(a[0])] = float(a[1])
        except ValueError: pass
ok = abs(v.get(3,9)-0.81) < 0.02 and abs(v.get(7,9)-0.31) < 0.02 and abs(v.get(1,9)-0.61) < 0.02
import sys
sys.exit(0 if ok else (print(f'RECALL-ETH FAIL: params not restored: {v}') or 1))" || exit 1
ARCH1=$(arch_count)
[ "$ARCH1" -gt "$ARCH0" ] || fail "mutation was not archived before push ($ARCH0 -> $ARCH1 archives)"
echo "RECALL-ETH PASS (params restored over the §8.7 loopback, mutation archived: $ARCH0 -> $ARCH1 archives)"
