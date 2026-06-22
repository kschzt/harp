#!/bin/bash
# mpe-eth-test — MPE / §9.5 per-voice + cross-format conformance over the §8.7 loopback.
#
# Moved OFF the passthrough USB hw rig: mpe-test's rapid VRLOAD claim/release (save-state +
# reload across many offline renders) wedges the libvirt PCI-USB-passthrough controller,
# cascading into recall/session-share failures (see scripts/hw-tests-linux.sh + the
# hw-rig-runner note). The MPE renders are OFFLINE/byte-exact, so the per-voice, neutral,
# determinism and cross-format (VST3 == CLAP == AU) hash RELATIONS are transport-agnostic;
# over eth the absolute "plain chord" anchor is the run's own plain render (the §8.7 offline
# golden is per-OS), so coverage is preserved without a pinned-hash dependency.
#
# Launches a rate-locked normal-synth harp-deviced; mpe-test.sh dials it via HARP_ETH_DEVICE.
# Needs clap-host + the CLAP plugin (and harp-vst3-host / au-host extend the cross-format
# proof where present) — mpe-test self-skips (exit 2) if they are absent, which we treat as
# a clean skip so the suite is not failed for an unbuilt optional host.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PORT="${PORT:-47925}"
DEVDIR=mpe-eth-state   # workspace-RELATIVE
DEVLOG=/tmp/mpe-eth-dev.log
fail() { echo "MPE-ETH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT INT TERM
rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

HARP_DEVICE_SERIAL="SIM-0001" HARP_ETH_DEVICE="127.0.0.1:$PORT" sh scripts/mpe-test.sh
rc=$?
# mpe-test: 0 pass / 2 N/A (clap host or plugin absent — a clean skip) / else fail.
if [ "$rc" = 2 ]; then echo "MPE-ETH: mpe-test skipped (clap host / plugin absent) — OK"; exit 0; fi
[ "$rc" = 0 ] || { cat "$DEVLOG"; fail "mpe-test over loopback failed (rc=$rc)"; }
