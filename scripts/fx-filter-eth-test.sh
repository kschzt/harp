#!/bin/bash
# fx-filter-eth-test — the §8.8 audio.fx EFFECT path end to end, over the §8.7 loopback:
# the examples/fx-filter device (harp-fx-filter) driven by the FX VST3 shell
# (harp-fx-shell) in harp-vst3-host, the way a DAW insert drives it. No hardware.
#
#   T1 identity      the device advertises audio.fx and its own engine (§6.2, §12.2)
#   T2 processing    the track audio goes H->D and comes back filtered: cutoff closed
#                    attenuates a 1 kHz tone, open passes it (§8.8)
#   T3 determinism   an automated offline bounce is byte-identical run to run (§8.3)
#   T4 automation    a DAW automation curve (--ramp, block-rate points like a DAW writes;
#                    the shell sends them as §9.4 ramps) reaches the device: the sound
#                    opens up over the render, the STORED value follows the curve (§9.4:
#                    automation moves state, so recall sees it), nothing applied late
#   T5 sample-exact  a mid-render automation point is applied on EXACTLY the sample of
#                    the input audio it was drawn against (0 samples off), at several
#                    DAW block sizes (§9.2)
#   T6 recall        save -> musician turns the device's knobs -> reopen: the device
#                    params AND the host-side Mix are restored, the displaced state is
#                    archived first, and the restored plugin renders BYTE-IDENTICALLY to
#                    the saved one (§11.4, §15.3); a pre-Mix (header-less) state still loads
#   T7 echo          a device front-panel knob echoes back to the plugin as automation
#                    (§9.4 echo; POSIX only — the MinGW device's panel is a stub)
#   T8 latency       the wet arrives EXACTLY the latency the plugin reports for PDC after
#                    its input — offline and live, at several DAW block sizes (§8.8)
#   T9 dry/wet       at Mix 50% the dry and the wet land on the same sample: the plugin
#                    delays its dry by the latency the wet carries (§8.8)
#
# Exit 0 pass / 1 fail. Kills only its OWN device (by pid) on a unique port.
set -u
cd "$(dirname "$0")/.."

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WIN=1; EXE=.exe ;; *) WIN=0; EXE= ;; esac
find1() { find "$1" -name "$2" 2>/dev/null | head -1; }
FXDEVICED="${FXDEVICED:-$( [ -x ./build/harp-fx-filter ] && echo ./build/harp-fx-filter || find1 . "harp-fx-filter$EXE")}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PROBE="${PROBE:-./build/harp-probe}"
FXPLUG="${FXPLUG:-$(find build-vst -maxdepth 5 -name harp-fx-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17931}"
# workspace-relative state (the Windows MinGW device can't mkdir an MSYS-converted /tmp path)
STATEDIR=fx-filter-state; STATEFILE=fx-filter.state; NOISE=fx-filter-noise.wav
OUT=fx-filter-out.wav; SOCK=/tmp/harp-fxf-panel.sock
DEVLOG=/tmp/fx-filter-dev.log; LOG=/tmp/fx-filter-host.log

fail() { echo "FX-FILTER FAIL: $1"; exit 1; }
pass() { echo "  ✓ $1"; }
[ -n "$FXDEVICED" ] && [ -x "$FXDEVICED" ] || fail "harp-fx-filter not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"
[ -n "$FXPLUG" ] && find "$FXPLUG/Contents" -type f -name 'harp-fx-shell*' 2>/dev/null | grep -q . \
    || fail "harp-fx-shell.vst3 not built (no module in ${FXPLUG:-<not found>})"

rm -rf "$STATEDIR" "$STATEFILE" "$NOISE" "$OUT" "$SOCK"; : > "$DEVLOG"
PANEL=(); [ "$WIN" = 0 ] && PANEL=(--panel-sock "$SOCK")
"$FXDEVICED" --port "$PORT" --state-dir "$STATEDIR" "${PANEL[@]}" >"$DEVLOG" 2>&1 &
DP=$!
trap 'kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; rm -rf "$STATEDIR" "$STATEFILE" "$STATEFILE.legacy" "$NOISE" "$OUT" fx-filter-ref.wav' EXIT INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

export HARP_ETH_DEVICE="127.0.0.1:$PORT"
export HARP_DEVICE_SERIAL="SIM-0001"
export HARP_RECONCILE_TIMEOUT_MS=1000 # interactive recall path: archive the displaced state (T6)
PD="-d $HARP_ETH_DEVICE"
# every render is hard-bounded: a no-connect would otherwise supervise for hot-plug forever
host() { perl -e 'alarm 60; exec @ARGV' "$HOSTBIN" "$FXPLUG" "$@"; }
hash_of() { sed -n 's/^output-hash: //p'; }
param() { "$PROBE" $PD params 2>/dev/null | sed -nE "s/^ *\[$1\].*[[:space:]]([0-9.]+)$/\1/p"; }
counter() { "$PROBE" $PD counters 2>/dev/null | sed -nE "s/^ *(x\.[a-z0-9.-]+\.)?$1 = ([0-9]+).*/\2/p" | head -1; }

# WAV analysis (stdlib only: runs on the Windows runner's python too).
#   rms FILE FROM TO          RMS of the left channel over [FROM, TO)
#   quarters FILE             brightness (first-difference / signal energy) per quarter
#   impulse FILE              first non-zero sample
#   loud FILE                 first sample with |x| > 0.01
#   firstdiff FILE OTHER      first sample where the two renders differ
wav() { python3 - "$@" <<'EOF'
import array, math, sys, wave
def left(p):
    w = wave.open(p); ch = w.getnchannels(); sw = w.getsampwidth()
    a = array.array('f' if sw == 4 else 'h'); a.frombytes(w.readframes(w.getnframes()))
    k = 1.0 if sw == 4 else 1 / 32768.0
    return [x * k for x in a[0::ch]]
op, x = sys.argv[1], left(sys.argv[2])
if op == 'rms':
    s = x[int(sys.argv[3]):int(sys.argv[4])]; print('%.6f' % math.sqrt(sum(v * v for v in s) / len(s)))
elif op == 'quarters':
    q = len(x) // 4
    print(' '.join('%.4f' % (sum((s[i] - s[i - 1]) ** 2 for i in range(1, len(s))) /
                             max(sum(v * v for v in s), 1e-12))
                   for s in (x[j * q:(j + 1) * q] for j in range(4))))
elif op == 'impulse':
    print(next((i for i, v in enumerate(x) if v != 0.0), -1))
elif op == 'loud':
    print(next((i for i, v in enumerate(x) if abs(v) > 0.01), -1))
elif op == 'firstdiff':
    y = left(sys.argv[3])
    print(next((i for i, (a, b) in enumerate(zip(x, y)) if a != b), -1))
EOF
}
# deterministic white noise (LCG) as the track for the automation tests
python3 - "$NOISE" <<'EOF'
import struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
s, fr = 12345, bytearray()
for _ in range(48000):
    s = (1103515245 * s + 12345) & 0x7fffffff; v = int(((s >> 8) / (1 << 23) - 0.5) * 16000)
    fr += struct.pack('<hh', v, v)
w.writeframes(bytes(fr))
EOF

echo "── fx-filter: $FXDEVICED on 127.0.0.1:$PORT, driven by $(basename "$FXPLUG")"

# ---- T1 identity ----
ID=$("$PROBE" $PD identify 2>&1) || { echo "$ID"; fail "T1 probe identify"; }
echo "$ID" | grep -q "audio.fx" || { echo "$ID"; fail "T1 device does not advertise audio.fx"; }
echo "$ID" | grep -q "engine: fx-filter 1.0.0" || { echo "$ID"; fail "T1 wrong engine identity"; }
pass "T1 identity: audio.fx, engine fx-filter 1.0.0"

# ---- T2 processing: the track audio is filtered by the device ----
host --set 1=1.0 --set 2=0.0 --input sine:1000 --seconds 0.3 --out "$OUT" >"$LOG" 2>&1 \
    || { cat "$LOG"; fail "T2 open render"; }
grep -q "connected:" "$LOG" || { cat "$LOG"; fail "T2 shell never connected"; }
OPEN=$(wav rms "$OUT" 4800 14400)
host --set 1=0.0 --set 2=0.0 --input sine:1000 --seconds 0.3 --out "$OUT" >"$LOG" 2>&1 \
    || { cat "$LOG"; fail "T2 closed render"; }
CLOSED=$(wav rms "$OUT" 4800 14400)
python3 -c "import sys; sys.exit(0 if $OPEN > 0.3 and $CLOSED < 0.003 else 1)" \
    || fail "T2 filter not applied: open rms=$OPEN (want >0.3), closed rms=$CLOSED (want <0.003)"
pass "T2 processing: 1 kHz tone rms $OPEN open -> $CLOSED at 20 Hz cutoff"

# ---- T3 determinism of an automated bounce ----
RAMP=(--input "wav:$NOISE" --set 2=0.3 --ramp 1=0.1:0.9 --seconds 1.0)
H1=$(host "${RAMP[@]}" --hash 2>/dev/null | hash_of)
H2=$(host "${RAMP[@]}" --hash --out "$OUT" 2>/dev/null | hash_of)
[ -n "$H1" ] || fail "T3 no output-hash"
[ "$H1" = "$H2" ] || fail "T3 automated bounce not deterministic: $H1 != $H2"
pass "T3 determinism: automated bounce byte-identical ($H1)"

# ---- T4 automation: the curve reaches the device, moves the sound and the stored value ----
Q=$(wav quarters "$OUT")
python3 -c "import sys; q=[float(v) for v in '$Q'.split()]; sys.exit(0 if all(b > a * 1.5 for a, b in zip(q, q[1:])) else 1)" \
    || fail "T4 automation did not open the filter over the render (brightness per quarter: $Q)"
V1=$(param 1)
python3 -c "import sys; sys.exit(0 if abs(float('${V1:-9}') - 0.9) < 0.02 else 1)" \
    || fail "T4 stored Cutoff did not follow the ramp to 0.9 (device has ${V1:-?})"
EL=$(counter evt_late); RL=$(counter ramp_late)
[ "${EL:-x}" = 0 ] && [ "${RL:-x}" = 0 ] || fail "T4 late application: evt_late=${EL:-?} ramp_late=${RL:-?}"
pass "T4 automation: brightness per quarter $Q, stored Cutoff $V1, evt_late=0 ramp_late=0"

# ---- T5 sample-accurate automation, relative to the audio it accompanies ----
# Two renders, identical but for ONE automation point at 0.25 s (DAW sample 12000): the
# first sample where they differ is where the device applied it. It must be the output
# sample carrying the INPUT's sample 12000 — the impulse render gives the wet's offset
# for this block size. Exact: 0 samples of tolerance.
REF=fx-filter-ref.wav
for BLK in 64 256 1024; do
    host --block "$BLK" --set 1=1.0 --set 2=0.0 --input impulse --seconds 0.2 --out "$OUT" >/dev/null 2>&1 \
        || fail "T5 impulse render (block $BLK)"
    OFF=$(wav impulse "$OUT")
    host --block "$BLK" --set 1=1.0 --set 2=0.0 --input sine:1000 --seconds 0.5 --out "$REF" >/dev/null 2>&1 \
        || fail "T5 reference render (block $BLK)"
    host --block "$BLK" --set 1=1.0 --set 2=0.0 --set-at 0.25:1=0.0 --input sine:1000 --seconds 0.5 \
        --out "$OUT" >/dev/null 2>&1 || fail "T5 automated render (block $BLK)"
    AT=$(wav firstdiff "$OUT" "$REF")
    [ "$OFF" -ge 0 ] && [ "$AT" -ge 0 ] || fail "T5 block $BLK: no impulse ($OFF) or no change ($AT) in the output"
    [ $((AT - OFF)) = 12000 ] \
        || fail "T5 block $BLK: automation applied at input sample $((AT - OFF)), want 12000 (wet offset $OFF)"
    echo "     block $BLK: applied at input sample $((AT - OFF)) (wet offset $OFF)"
done
rm -f "$REF"
pass "T5 sample-accurate: automation applied on its exact input sample at blocks 64/256/1024"

# ---- T6 recall ----
A0=$("$PROBE" $PD refs 2>/dev/null | grep -c "archive/")
SET=(--set 1=0.31 --set 2=0.64 --set 50=0.35) # 50 = the host-side dry/wet Mix
HPRE=$(host "${SET[@]}" --input "wav:$NOISE" --seconds 0.5 --hash --save-state "$STATEFILE" 2>/dev/null | hash_of)
[ -n "$HPRE" ] && [ -s "$STATEFILE" ] || fail "T6 save render (no hash or empty state)"
"$PROBE" $PD knob 1 0.90 >/dev/null 2>&1 || fail "T6 knob 1 mutate"
"$PROBE" $PD knob 2 0.05 >/dev/null 2>&1 || fail "T6 knob 2 mutate"
[ "$(param 1)" = "0.900" ] || fail "T6 the mutation did not reach the device (Cutoff $(param 1))"
host --load-state "$STATEFILE" --input "wav:$NOISE" --seconds 0.5 --hash >"$LOG" 2>&1 \
    || { cat "$LOG"; fail "T6 load render"; }
grep -q "restored\|SYNCED" "$LOG" || { cat "$LOG"; fail "T6 no recall action logged"; }
HPOST=$(hash_of <"$LOG")
[ "$HPRE" = "$HPOST" ] || fail "T6 restored render $HPOST != saved render $HPRE (lossy recall)"
C=$(param 1); R=$(param 2)
python3 -c "import sys; sys.exit(0 if abs(float('${C:-9}')-0.31)<0.001 and abs(float('${R:-9}')-0.64)<0.001 else 1)" \
    || fail "T6 params not restored: Cutoff=${C:-?} Resonance=${R:-?} (want 0.31 / 0.64)"
A1=$("$PROBE" $PD refs 2>/dev/null | grep -c "archive/")
[ "$A1" -gt "$A0" ] || fail "T6 the displaced device state was not archived ($A0 -> $A1)"
# an older build's state is the bare recall bundle (no 'HF1' + Mix header): it must still
# restore the device (Mix falls back to 100% wet)
"$PROBE" $PD knob 1 0.90 >/dev/null 2>&1 || fail "T6 knob 1 re-mutate"
# (harp-vst3-host's state file = u32 len + component state + u32 len + controller state)
python3 - "$STATEFILE" "$STATEFILE.legacy" <<'EOF' || fail "T6 saved component state has no HF1 header"
import struct, sys
d = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('<I', d, 0)[0]
comp, rest = d[4:4 + n], d[4 + n:]
assert comp[:3] == b'HF1', comp[:3]
comp = comp[7:]                      # drop 'HF1' + the float32 Mix: the bare bundle
open(sys.argv[2], 'wb').write(struct.pack('<I', len(comp)) + comp + rest)
EOF
host --load-state "$STATEFILE.legacy" --input "wav:$NOISE" --seconds 0.2 >"$LOG" 2>&1 \
    || { cat "$LOG"; fail "T6 legacy-state load render"; }
rm -f "$STATEFILE.legacy"
[ "$(param 1)" = "0.310" ] || fail "T6 legacy (header-less) state did not restore the device (Cutoff $(param 1))"
pass "T6 recall: Cutoff $C Resonance $R + Mix restored, render byte-identical ($HPOST), archives $A0 -> $A1; legacy state loads"

# ---- T7 front-panel echo -> DAW automation ----
if [ "$WIN" = 1 ]; then
    echo "  ⏭ T7 echo skipped: the MinGW device's front panel is a stub (no AF_UNIX panel socket)"
else
    panel() { python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect('$SOCK'); s.send(sys.argv[1].encode()+b'\n'); s.recv(256); s.close()" "$1"; }
    : >"$LOG"
    ( for _ in $(seq 1 60); do grep -q "connected:" "$LOG" 2>/dev/null && break; sleep 0.1; done
      sleep 0.3; panel "knob 0 1 0.42"; sleep 0.3; panel "knob 0 2 0.77" ) &
    INJ=$!
    host --input "wav:$NOISE" --seconds 2 --realtime >"$LOG" 2>&1 || true
    kill -9 "$INJ" 2>/dev/null; wait "$INJ" 2>/dev/null
    grep -q "echo: param 1 -> 0.4200" "$LOG" || { grep "echo:" "$LOG"; fail "T7 Cutoff knob not echoed to the plugin"; }
    grep -q "echo: param 2 -> 0.7700" "$LOG" || { grep "echo:" "$LOG"; fail "T7 Resonance knob not echoed to the plugin"; }
    grep -E "echo: param ([0-9]+)" "$LOG" | grep -vE "echo: param [12] " \
        && fail "T7 the shell forwarded echoes for ids that are not its params (e.g. meters)"
    pass "T7 echo: front-panel Cutoff/Resonance moves surfaced as plugin automation"
fi

# ---- T8 the wet arrives exactly the reported latency after its input ----
# An impulse in; the first non-zero wet sample must sit at the latency the plugin reports
# (harp-vst3-host prints it as reported-samples). Live renders run against the wall clock,
# so a scheduler hiccup on a loaded runner can underrun a block — retry those a few times.
latency_ok() { # latency_ok BLOCK [--realtime]
    local out rep off
    out=$(host --block "$1" ${2:-} --set 1=1.0 --set 2=0.0 --input impulse --seconds 0.3 --out "$OUT" 2>&1) \
        || { echo "$out"; return 2; }
    rep=$(echo "$out" | sed -nE 's/.*reported-samples=([0-9]+).*/\1/p' | head -1)
    off=$(wav impulse "$OUT")
    echo "     block $1 ${2:+live }: reported $rep, wet at $off"
    [ -n "$rep" ] && [ "$off" = "$rep" ]
}
for BLK in 64 256 1000; do latency_ok "$BLK" || fail "T8 offline block $BLK: wet not at the reported latency"; done
for BLK in 256 1024; do
    ok=0; for _ in 1 2 3; do latency_ok "$BLK" --realtime && { ok=1; break; }; done
    [ "$ok" = 1 ] || fail "T8 live block $BLK: wet not at the reported latency (3 tries)"
done
pass "T8 latency: the wet arrives exactly the reported latency after its input, offline and live"

# ---- T9 dry/wet alignment at Mix 50% ----
# 50/50 of the dry impulse and its (open-filter) wet: aligned, the first audible sample is
# at the reported latency — a dry that skipped the plugin's delay would sound at sample 0.
for RT in "" --realtime; do
    out=$(host --block 256 $RT --set 1=1.0 --set 2=0.0 --set 50=0.5 --input impulse --seconds 0.3 --out "$OUT" 2>&1) \
        || { echo "$out"; fail "T9 render ${RT:-offline}"; }
    rep=$(echo "$out" | sed -nE 's/.*reported-samples=([0-9]+).*/\1/p' | head -1)
    at=$(wav loud "$OUT")
    [ "$at" = "$rep" ] || fail "T9 ${RT:-offline}: first audible sample at $at, want the reported latency $rep (dry and wet misaligned)"
done
pass "T9 dry/wet: at Mix 50% dry and wet coincide at the reported latency, offline and live"

echo "FX-FILTER PASS (§8.8 effect: processing, automation, sample accuracy, recall, latency$( [ "$WIN" = 1 ] || echo ', echo'))"
