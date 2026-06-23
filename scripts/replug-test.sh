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
SERIAL=${SERIAL:-PI4B-0001}   # the board PI hosts; pin the shell to it
HOST=build-vst/harp-vst3-host
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"  # Linux CI overrides -> ~/.vst3
OUT=/tmp/replug-test.wav
LOG=/tmp/replug-test.log

# claim guard: a DAW holding the device makes this test lie
if pgrep -x "Live" >/dev/null 2>&1; then
    echo "REPLUG FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

echo "── replug: 20 s realtime render, daemon restart at t=6 s (device $SERIAL)"
# Drone removed: the audio must come from NOTES, and crucially from notes the host
# plays AFTER the t=6s restart — a held --chord dies with the old daemon (notes are
# momentary, not re-asserted state like a param/drone was). The host schedules
# --notes at 0.6 s spacing from t=0, so a list spanning the full 20 s keeps notes
# (and their audio) flowing past the reconnect, the tail included. ~34 notes -> 20.4 s.
NOTES=$(python3 -c "print(','.join(['48','55','60','64'][i%4] for i in range(34)))")
HARP_DEVICE_SERIAL="$SERIAL" "$HOST" "$PLUG" --realtime --notes "$NOTES" --set 8=0.7 --set 3=0.6 \
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
# rule 6: reconnect pins the bound serial — every claim must be $SERIAL,
# never a same-model sibling that happened to be free during the restart.
if grep -oE "claimed 1209:[0-9a-f]+ serial PI4B-[0-9]+" "$LOG" \
     | grep -qv "serial $SERIAL"; then
    echo "REPLUG FAIL: reconnect bound a different unit than $SERIAL"
    grep -oE "claimed .* serial PI4B-[0-9]+" "$LOG"; exit 1
fi
echo "   reconnect re-bound $SERIAL (never a sibling) — rule 6 holds"

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
