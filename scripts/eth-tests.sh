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

# ── small-target loop stability (target-invariant rate loop) ─────────────────────
# The §8.7 trim loop normalizes the fill error by the LIVE target AND scales the EMA
# with the loop bandwidth, so its DAMPING (not just gain) is target-invariant: the same
# well-damped loop holds at the 2048 consumer-LAN default AND at a much smaller buffer
# on a clean link. Here we drop the setpoint 4x to 512 frames (~10.6 ms — the floor the
# direct-cable sweep reached) and require the loop to stay AS CLEAN AS the 2048 baseline:
# connected, tone rms holds, and underruns DON'T meaningfully grow. The old fixed-Kp loop
# hunted on its clamp rail below 512 frames (it underran CONTINUOUSLY — hundreds over 8 s);
# the normalized loop tracks the baseline within a handful.
#   WHY 512 AND NOT LOWER — and why this is a STABILITY guard, not a latency proof: CI
#   runs over localhost LOOPBACK, not a cable. A sub-512 buffer (< ~11 ms) cannot absorb
#   localhost scheduling jitter — macOS's relative-nanosleep device sim alone throws ~500
#   false underruns at target=256 — so smaller targets are NOT honestly testable here. The
#   real sub-10 ms / sub-512 latency (127 dB, 0 glitch) is validated on the Mac<->kria
#   DIRECT CABLE, not in CI.
# --block 256 => the 2*maxDawBlock underrun-safe floor is exactly 512, so the request
# lands at 512 (not silently floored higher); HARP_ETH_NSAMPLES=128 = a smaller RTP packet.
base_under="${under:-0}"   # the 2048-default underruns from the bit-exact section above
echo "──── small-target stability: HARP_ETH_TARGET=512 (~10.6 ms, 4x below default; baseline underruns=$base_under)"
start_dev --tone 440
out=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_ETH_TARGET=512 HARP_ETH_NSAMPLES=128 \
      "$HOSTBIN" "$PLUG" --seconds 8 --realtime --block 256 --json 2>/tmp/eth-small.err || true)
rms=$(printf '%s' "$out" | sed -nE 's/.*"rms":([0-9.]+).*/\1/p')
conn=$(grep -c 'connected:' /tmp/eth-small.err 2>/dev/null || true)
under=$(grep -oE 'underruns: [0-9]+' /tmp/eth-small.err 2>/dev/null | grep -oE '[0-9]+' | tail -1)
stop_dev
echo "   target=512 nsamples=128  rms=${rms:-?}  underruns=${under:-?} (baseline $base_under)  conn=${conn:-0}"
# Gate on the DELTA vs the 2048 baseline (robust: loopback jitter hits both runs, and a
# 10.6 ms buffer legitimately absorbs a little less than 43 ms — so allow +64). A hunting
# loop underruns continuously (hundreds-to-thousands at block 256 over 8 s), far past +64.
if [ "${conn:-0}" -gt 0 ] && [ -n "$rms" ] \
   && [ "$(python3 -c "print(1 if $rms > 0.30 else 0)")" = 1 ] \
   && [ "$(python3 -c "print(1 if ${under:-99999} <= ${base_under:-0} + 64 else 0)")" = 1 ]; then
    ok "small-target (512/128): loop stable at 4x-smaller buffer (rms $rms, underruns ${under} vs baseline $base_under)"
else
    bad "small-target (512/128): conn=$conn rms=${rms:-none} underruns=${under:-?} vs baseline $base_under (want connected + rms>0.30 + underruns<=baseline+64)"
fi

# ── ASRC fallback: a non-rate-lock device → host resamples to its own clock ───────
# --no-rate-lock drops the audio.rate-lock capability from hello, so the host CANNOT
# host-lock (no audio.trim) and must take the §8.7 ASRC path (host/freerun): recover
# the device's rate from the RTP timestamps and varispeed-resample its stream to the
# host clock. Models a real converter whose crystal the host can't steer. Asserts the
# host actually CHOSE ASRC (the warning fires) AND still renders the tone faithfully
# through the resampler — the ~42 ms elastic-buffer warm-up silence is negligible over
# 8 s, so the tone's rms survives; no reanchors (ASRC sends no audio.trim).
echo "──── ASRC: non-rate-lock device, host clock-recovers + resamples"
start_dev --no-rate-lock --tone 440
out=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" "$HOSTBIN" "$PLUG" --seconds 8 --realtime --json 2>/tmp/eth-asrc.err || true)
rms=$(printf '%s' "$out" | sed -nE 's/.*"rms":([0-9.]+).*/\1/p')
conn=$(grep -c 'connected:' /tmp/eth-asrc.err 2>/dev/null || true)
asrc=$(grep -c 'ASRC resample' /tmp/eth-asrc.err 2>/dev/null || true)
stop_dev
echo "   rms=${rms:-?}  asrc-chosen=${asrc:-0}  conn=${conn:-0}  (ideal tone rms 0.3536)"
if [ "${conn:-0}" -gt 0 ] && [ "${asrc:-0}" -gt 0 ] && [ -n "$rms" ] \
   && [ "$(python3 -c "print(1 if $rms > 0.25 else 0)")" = 1 ]; then
    ok "ASRC: chose resample path, tone rendered through it (rms $rms)"
else
    bad "ASRC: conn=$conn asrc=$asrc rms=${rms:-none} (want connected + ASRC + rms>0.25)"
fi

# ── multi-output demux + per-part routing over RTP (bit-exact AND ASRC) ──────────
# The §8.7 coverage the USB rig can't run reliably (it wedges on the repeated
# multi-instance --part-audio re-claims): the device streams the slot UNION over RTP
# and the owner's reader() demuxes each part into its sink. Run the conformance tests
# BOTH ways — first a rate-lock device (1:1 union demux) and then a --no-rate-lock one
# (the union is varispeed-resampled THEN demuxed: MULTICHANNEL ASRC, the per-part path
# through host/freerun). Identical assertions both ways prove the resampled union
# demuxes to the same per-part routing as the bit-exact one:
#   alias-part-audio = the main mix is RICHER than a demuxed part (a strict subset),
#   part-param-iso   = a part's level routes to ITS audio and stays out of the others.
# A normal-synth device (no --tone) so the aliases' injected per-part notes render.
export HARP_DEVICE_SERIAL="SIM-0001"   # the --port daemon's serial (the connect-check matches it)
export HARP_ETH_DEVICE="127.0.0.1:$PORT"
for mode in "bit-exact:" "ASRC:--no-rate-lock"; do
    label="${mode%%:*}"; devarg="${mode#*:}"
    echo "──── multi-output ($label): per-part demux + routing over RTP"
    start_dev $devarg
    for t in scripts/alias-part-audio-test.sh scripts/part-param-iso-test.sh; do
        echo "────── $t ($label over §8.7 Ethernet)"
        if sh "$t"; then ok "$(basename "$t" .sh): per-part over RTP ($label)"
        else bad "$(basename "$t" .sh) over RTP ($label) (exit $?)"; fi
    done
    stop_dev
done
unset HARP_ETH_DEVICE

echo "════ eth-tests: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
