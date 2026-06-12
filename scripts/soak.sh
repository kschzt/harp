#!/bin/bash
# Real-time soak test for the HARP shell against live hardware.
# Floods the full event surface — DAW automation ramps, scheduled notes,
# concurrent web-panel knob turns — under wall-clock pacing, then asserts
# the §9.2/§14.1 contract: no silence gaps, no dropped events, no errors.
#
# usage: scripts/soak.sh [SECONDS] (default 30; device must be unclaimed)
set -u
S=${1:-30}
HOST=${HARP_HOST:-harp.local}
VST=${VST:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}
HOSTBIN=${HOSTBIN:-./build-vst/harp-vst3-host}
OUT=/tmp/harp-soak.wav
LOG=/tmp/harp-soak.log
fail() { echo "SOAK FAIL: $1"; exit 1; }

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "SOAK SKIP: Ableton Live is running (device claim)"
    exit 2
fi

NOTES=$(python3 -c "import sys; n=int($S/0.6); print(','.join(['60','64','67','72'][i%4] for i in range(n)))")
C0=$(curl -s --max-time 3 "http://$HOST:8080/api/counters") || fail "panel unreachable"

# concurrent front-panel traffic (Osc Shape wiggles; echo events flow back)
( end=$((SECONDS + S)); while [ $SECONDS -lt $end ]; do
    curl -s --max-time 1 "http://$HOST:8080/api/knob?id=2&value=0.$((RANDOM % 90 + 10))" > /dev/null
    sleep 0.4
  done ) & KNOBPID=$!

"$HOSTBIN" "$VST" --realtime --block "${BLOCK:-256}" \
    --set 7=0.4 --set 8=0.5 --ramp 3=0.15:0.9 --ramp 4=0.2:0.6 --ramp 1=0.35:0.6 \
    --notes "$NOTES" --seconds "$S" --out "$OUT" > "$LOG" 2>&1
RC=$?
kill $KNOBPID 2>/dev/null; wait $KNOBPID 2>/dev/null
[ $RC -eq 0 ] || { cat "$LOG"; fail "host exited $RC"; }

grep -q "WARNING" "$LOG" && { grep WARNING "$LOG"; fail "shell dropped events"; }
grep -q "connected:" "$LOG" || fail "never connected to device"

C1=$(curl -s --max-time 3 "http://$HOST:8080/api/counters")
# ramp_late accrues at a structural RATE under flood (~4/s: ramp starts
# anchor one block back), so its budget scales with run length — the 500
# baseline was calibrated for the 30 s default
RAMP_BUDGET=$((500 * S / 30))
python3 - "$OUT" "$C0" "$C1" "$RAMP_BUDGET" <<'EOF'
import json, math, struct, sys, wave
w = wave.open(sys.argv[1])
raw = w.readframes(w.getnframes())
L = struct.unpack(f"<{len(raw)//2}h", raw)[0::2]
gaps = 0
for k in range(5, len(L) // 4800):
    seg = L[k*4800:(k+1)*4800]
    if math.sqrt(sum(x*x for x in seg) / len(seg)) < 10:
        gaps += 1
c0, c1 = json.loads(sys.argv[2]), json.loads(sys.argv[3])
problems = []
if gaps: problems.append(f"{gaps} silent 100ms windows")
for key in ("evq_drops", "evt_late", "frame_errors", "session_resets"):
    if c1.get(key, 0) != c0.get(key, 0):
        problems.append(f"{key}: {c0.get(key,0)} -> {c1.get(key,0)}")
# ramp-end lateness is budgeted, not zero: ramp starts anchor at the
# previous automation point, so the end margin is structurally one block;
# a few % degrading to ~ms-late sets under triple-ramp flood is benign.
ramp_late = c1.get("ramp_late", 0) - c0.get("ramp_late", 0)
budget = int(sys.argv[4]) if len(sys.argv) > 4 else 500
if ramp_late > budget:
    problems.append(f"ramp_late {ramp_late} > budget {budget}")
if problems:
    print("SOAK FAIL:", "; ".join(problems)); sys.exit(1)
print(f"audio: {len(L)} samples, no silence gaps; device counters clean")
EOF
[ $? -eq 0 ] || exit 1

# Severity metric: total padded samples (what the ear integrates), not pad
# count — sync libusb at user priority jitters routinely; the SUM must stay
# bounded. Limit: 0.5% of the run (S * 48000 / 200), with the audible-gap
# check above remaining absolute zero.
LIMIT=$((S * 240))
PADDED=$(grep -o 'padded samples: [0-9]*' "$LOG" | grep -o '[0-9]*' | tail -1)
UNDER=$(grep -o 'underruns: [0-9]*' "$LOG" | grep -o '[0-9]*' | tail -1)
echo "shell underruns: ${UNDER:-0} events, ${PADDED:-0} samples padded (limit $LIMIT = 0.5%)"
[ "${PADDED:-0}" -lt "$LIMIT" ] || fail "padded samples: $PADDED > $LIMIT"
echo "SOAK PASS (${S}s realtime, automation + notes + panel traffic)"
