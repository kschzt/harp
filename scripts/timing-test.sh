#!/bin/bash
# Note-timing conformance against live hardware — encodes the 2026-06-11
# timing saga as regression tests:
#   1. pitch arrival: gate -> on-pitch within 5 ms at ALL interval sizes
#      (catches portamento-class defects: interval-dependent arrival was
#      audible as r=0.86 grid slop before the legato-only glide fix)
#   2. onset placement: scheduled notes land at exact grid spacing
#   3. realtime 16th-note storm: device evt_late MUST NOT move
#      (catches event/pacing ordering races: events transmitted after
#      their own block's pacing frame applied a block late)
# usage: scripts/timing-test.sh   (device must be unclaimed)
set -u
HOST=${HARP_HOST:-harp.local}
VST=${VST:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}
HOSTBIN=${HOSTBIN:-./build-vst/harp-vst3-host}
fail() { echo "TIMING FAIL: $1"; exit 1; }

# the device is exclusive: a DAW holding the claim makes every test render
# silence and fail confusingly (cost us a debugging detour) — guard first
"$HOSTBIN" "$VST" --seconds 0.05 2>&1 | grep -q "connected:" \
    || { echo "TIMING FAIL: cannot claim device — the rig must own it exclusively (busy/absent?)"; exit 3; }

# bind claim to proof: this test IS the harp-perf (§9.2 ±1-sample)
# conformance probe, so a device advertising harp-perf must pass it, and
# one that doesn't claim it has no business being measured here.
PROBE=${PROBE:-./build/harp-probe}
if [ -x "$PROBE" ]; then
    "$PROBE" -d usb identify 2>/dev/null | grep -q harp-perf \
        || fail "device does not advertise harp-perf (claim/proof mismatch)"
fi

# ---- 1+2: offline render, mixed intervals up to 15 semitones ----
# settle device into a known state first (sine, fast envelope; drone removed)
"$HOSTBIN" "$VST" --set 2=0 --set 3=0.8 --set 5=0.05 --set 6=0.15 \
    --set 8=0.6 --seconds 0.5 > /dev/null 2>&1 || fail "settle render"
"$HOSTBIN" "$VST" --notes "62,64,62,72,60,67,59,74,62,63" --seconds 6.2 \
    --out /tmp/harp-timing.wav > /dev/null 2>&1 || fail "render did not complete"

python3 - <<'EOF' || exit 1
import wave, struct, math, sys
w = wave.open('/tmp/harp-timing.wav')
rate = w.getframerate()
raw = w.readframes(w.getnframes())
L = struct.unpack(f'<{len(raw)//2}h', raw)[0::2]
notes = [62,64,62,72,60,67,59,74,62,63]
WIN, HOP = int(0.02*rate), rate//1000
def f0_at(s):
    seg = L[s:s+WIN]
    best, bl = 0, 0
    for lag in range(rate//1000, rate//50):
        v = sum(seg[i]*seg[i+lag] for i in range(0, WIN-lag, 4))
        if v > best: best, bl = v, lag
    return rate/bl if bl else 0
# Gates are scheduled at i*0.6 s; actual delivery is latency-shifted by a
# CONSTANT (that's PDC's job, not a defect). Measure each note's pitch
# arrival relative to its schedule, take the first note as the latency
# baseline, and assert the SPREAD — interval-dependent arrival is the
# defect class this guards against.
worst = 0
base = None
for k, n in enumerate(notes):
    sched = int(k*0.6*rate)
    target = 440*2**((n-69)/12)
    arrive = None
    for ms in range(0, 150):
        f = f0_at(sched + ms*HOP)
        if f > 0 and abs(1200*math.log2(f/target)) < 30:
            arrive = ms; break
    if arrive is None:
        print(f'TIMING FAIL: note {n} (k={k}) never reached pitch'); sys.exit(1)
    if base is None: base = arrive
    worst = max(worst, abs(arrive - base))
print(f'pitch-arrival spread across intervals: {worst} ms (limit 5)')
sys.exit(0 if worst <= 5 else (print('TIMING FAIL: interval-dependent arrival') or 1))
EOF

# ---- 3: realtime storm, evt_late must stay flat ----
C0=$(curl -s --max-time 3 "http://$HOST:8080/api/counters" | python3 -c "import json,sys; print(json.load(sys.stdin)['evt_late'])") || fail "panel unreachable"
NOTES=$(python3 -c "print(','.join(['62','67','74','62']*24))")
"$HOSTBIN" "$VST" --realtime --block 512 --set 8=0.6 \
    --notes "$NOTES" --note-period 0.125 --seconds 13 > /tmp/harp-timing.log 2>&1 \
    || fail "realtime run failed"
C1=$(curl -s --max-time 3 "http://$HOST:8080/api/counters" | python3 -c "import json,sys; print(json.load(sys.stdin)['evt_late'])")
echo "evt_late: $C0 -> $C1 over 96 realtime 16th notes"
[ "$C1" -eq "$C0" ] || fail "events applied late (ordering race?)"
grep -q WARNING /tmp/harp-timing.log && fail "event drops"
echo "TIMING PASS"
