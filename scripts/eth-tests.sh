#!/bin/bash
# eth-tests — §8.7 Ethernet/IP conformance against FAKE HARDWARE.
#
# A localhost harp-deviced (the REAL synth + protocol, just not plugged in over USB)
# serves the framed control link over TCP and the audio plane over RTP/UDP; the shell
# connects via HARP_ETH_DEVICE. No USB, no physical device — so this runs on any cloud
# runner. It is the ci-side complement to hw.yml's real-device USB suite, running in
# PARALLEL: hw.yml is the authoritative device + USB-binding oracle; eth-tests covers
# the Ethernet TRANSPORT (bit-exact play, the host-locked rate loop, the RTP audio
# plane, per-part demux) plus transport-agnostic device behavior that the USB rig
# can't run reliably (e.g. the multi-instance per-part tests).
#
# Assumes harp-deviced + harp-vst3-host + the harp-shell.vst3 bundle are already
# built (eth.yml builds them). Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47900}"
export HARP_RECONCILE_TIMEOUT_MS=0   # headless: no front panel to answer a recall pick

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo "   ✓ $1"; }
bad() { FAIL=$((FAIL + 1)); echo "   ✗ FAIL: $1"; }

DEVPID=""
stop_dev() { [ -n "$DEVPID" ] && kill "$DEVPID" 2>/dev/null; wait "$DEVPID" 2>/dev/null; DEVPID=""; }
trap stop_dev EXIT
# start the fake-hardware synth on 127.0.0.1:$PORT; $* = extra daemon args (e.g. --tone)
start_dev() {
    stop_dev
    rm -rf /tmp/eth-dev-state; : > /tmp/eth-dev.log
    "$DEVICED" --port "$PORT" --state-dir /tmp/eth-dev-state "$@" >/tmp/eth-dev.log 2>&1 &
    DEVPID=$!
    for _ in $(seq 1 25); do
        grep -q "listening on $PORT" /tmp/eth-dev.log 2>/dev/null && return 0
        sleep 0.2
    done
    echo "eth-tests: device failed to start on $PORT"; cat /tmp/eth-dev.log; exit 1
}

[ -x "$DEVICED" ] || { echo "ETH-TESTS FAIL: $DEVICED not built"; exit 1; }
[ -x "$HOSTBIN" ] || { echo "ETH-TESTS FAIL: $HOSTBIN not built"; exit 1; }
[ -n "$PLUG" ] && [ -d "$PLUG" ] || { echo "ETH-TESTS FAIL: harp-shell.vst3 bundle not found"; exit 1; }
echo "── eth conformance: fake hardware (harp-deviced) on 127.0.0.1:$PORT, shell over §8.7 Ethernet"
echo "   device=$DEVICED  host=$HOSTBIN  plug=$PLUG"

# ── bit-exact fidelity ──────────────────────────────────────────────────────────
# The device free-runs a 440 Hz tone over RTP; the host plays it 1:1 (no resampling
# = bit-exact) and the feeder's audio.trim loop slaves the device's emit rate to the
# host's consumption. Asserts a connected session and SUBSTANTIAL audio (the RTP
# plane carries the synth's tone, not silence); the device-side reanchor count (the
# rate loop's discontinuous corrections) and host underruns are reported. rms of a
# 0.5-amplitude tone is 0.3536; we gate on >0.30 to tolerate cloud-runner scheduling
# jitter (padding lowers rms), and report the exact rms so drift is visible.
echo "──── bit-exact: device free-runs 440 Hz, host plays 1:1 + rate-trims"
start_dev --tone 440
out=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" "$HOSTBIN" "$PLUG" --seconds 8 --realtime --json 2>/tmp/eth-host.err || true)
rms=$(printf '%s' "$out" | sed -nE 's/.*"rms":([0-9.]+).*/\1/p')
conn=$(grep -c 'connected:' /tmp/eth-host.err 2>/dev/null || true)
under=$(grep -oE 'underruns: [0-9]+' /tmp/eth-host.err 2>/dev/null | grep -oE '[0-9]+' | tail -1)
stop_dev
reanch=$(grep -oE 'stopped \(([0-9]+) reanchors\)' /tmp/eth-dev.log 2>/dev/null | grep -oE '[0-9]+' | tail -1)
echo "   rms=${rms:-?}  reanchors=${reanch:-?}  underruns=${under:-?}  (ideal tone rms 0.3536)"
if [ "${conn:-0}" -gt 0 ] && [ -n "$rms" ] \
   && [ "$(python3 -c "print(1 if $rms > 0.30 else 0)")" = 1 ]; then
    ok "bit-exact: connected, RTP audio flowing 1:1 (rms $rms)"
else
    bad "bit-exact: conn=$conn rms=${rms:-none} (want connected + rms>0.30)"
fi

# ── multi-output demux + per-part routing over RTP ──────────────────────────────
# The §8.7 coverage the USB rig can't run reliably (it wedges on the repeated
# multi-instance --part-audio re-claims): the device streams the slot UNION over RTP
# and the owner's reader() demuxes each part into its sink. Reuse the conformance
# tests verbatim — they detect HARP_ETH_DEVICE and dial the localhost fake hardware:
#   alias-part-audio = the main mix is RICHER than a demuxed part (a strict subset),
#   part-param-iso   = a part's level routes to ITS audio and stays out of the others.
# A normal-synth device (no --tone) so the aliases' injected per-part notes render.
echo "──── multi-output: per-part demux + routing over RTP"
start_dev
export HARP_ETH_DEVICE="127.0.0.1:$PORT"
export HARP_DEVICE_SERIAL="SIM-0001"   # the --port daemon's serial (the connect-check matches it)
for t in scripts/alias-part-audio-test.sh scripts/part-param-iso-test.sh; do
    echo "──── $t (over §8.7 Ethernet)"
    if sh "$t"; then ok "$(basename "$t" .sh): per-part over RTP"
    else bad "$(basename "$t" .sh) over RTP (exit $?)"; fi
done
unset HARP_ETH_DEVICE
stop_dev

echo "════ eth-tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
