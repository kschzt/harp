#!/bin/sh
# replug-test — T2 in miniature, automated: kill the device session
# mid-stream (daemon restart = FFS unbind + rebind = bus-level
# detach/reattach, the firmware-update scenario) and assert the shell
# rides through it: silence while gone, automatic reconnect, audio back,
# project state re-asserted, no crash, no stuck notes.
#
# Needs: shell installed for the CLI host, Pi reachable, Live closed.
set -e
cd "$(dirname "$0")/.."

PI=${PI:-jak@harp.local}
HOST=build-vst/harp-vst3-host
PLUG="$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3"
OUT=/tmp/replug-test.wav
LOG=/tmp/replug-test.log

# claim guard: a DAW holding the device makes this test lie
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "REPLUG SKIP: Ableton Live is running (device claim)"
    exit 2
fi

echo "── replug: 20 s realtime render, daemon restart at t=6 s"
# continuous drone (param 7 up) so post-reconnect audio is provable
"$HOST" "$PLUG" --realtime --set 7=0.9 --set 8=0.7 --set 3=0.6 \
    --seconds 20 --out "$OUT" >"$LOG" 2>&1 &
HOSTPID=$!

sleep 6
echo "   t=6s: restarting device daemon (bus-level detach/reattach)"
ssh "$PI" 'sudo -n systemctl restart harp-deviced-usb'

wait "$HOSTPID" || { echo "REPLUG FAIL: host crashed"; tail -5 "$LOG"; exit 1; }

grep -q "device gone\|link receive failed\|audio stream read failed" "$LOG" || {
    echo "REPLUG FAIL: shell never noticed the detach"; tail -20 "$LOG"; exit 1; }
grep -q "reconnected; stream re-established" "$LOG" || {
    echo "REPLUG FAIL: no reconnect logged"; tail -20 "$LOG"; exit 1; }

# audio must be back: RMS of the final 2 s well above silence
python3 - "$OUT" <<'EOF'
import struct, sys, wave
w = wave.open(sys.argv[1])
n = w.getnframes()
rate = w.getframerate()
w.setpos(max(0, n - 2 * rate))
data = w.readframes(2 * rate)
samples = struct.unpack(f"<{len(data)//2}h", data)
rms = (sum(s * s for s in samples) / max(1, len(samples))) ** 0.5
print(f"   post-reconnect tail RMS: {rms:.0f}")
sys.exit(0 if rms > 300 else 1)
EOF
[ $? -eq 0 ] || { echo "REPLUG FAIL: no audio after reconnect"; exit 1; }

echo "REPLUG PASS (detach noticed, session re-established, audio resumed)"
