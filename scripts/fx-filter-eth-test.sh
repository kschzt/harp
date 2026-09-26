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
#                    its input — offline at several DAW block sizes, and live, where every
#                    wet sample must equal the offline render shifted by the latency
#                    difference (§8.8)
#   T9 dry/wet       at Mix 50% the dry comes out on exactly the wet's latency, offline and
#                    live: the plugin delays its dry by the latency the wet carries (§8.8)
#   T10 late guard   a live wet that is PERSISTENTLY later than its budget (simulated:
#                    HARP_FX_TEST_UNDERBUDGET) re-anchors and stays audible instead of
#                    dropping every block, counted in x.harp.fx_reanchors. (That a TRANSIENT
#                    hiccup never re-anchors is pinned by the policy's unit test —
#                    runtime_units_tests, test_fx_late_guard_policy — not a wall clock.)
#   T11 live events  dense live automation (an LFO on Cutoff) is applied on time: evt_late,
#                    ramp_late and fence_timeouts stay 0 (§9.2, §8.3.1) — the effect's
#                    events carry no lead, the input gate orders them
#   T12 reconnect    the device restarts mid-render, and separately comes up only after
#                    the render started: after the runtime (re)connects, every live wet
#                    sample is again the offline render shifted by exactly the reported
#                    latency (the audio thread adopts each new session's SSI domain)
#
# NO-FLAKE DESIGN. Every assertion is deterministic on a loaded runner:
#   - every client (render or probe) waits until the device has finished the previous
#     session (it serves one at a time), and every render must connect on its first attempt
#     — a render that connected late would run disconnected and silently prove nothing;
#   - offline renders are host-paced and byte-deterministic (the FX shell's offline pull
#     waits for the device while connected, never pads on a wall-clock timeout);
#   - live checks never retry: a scheduler hiccup can only pad a live block with zeros
#     (dropping its late wet to keep the delay), so live output is compared sample-exact
#     against the offline render with "or exactly zero" as the only allowance.
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
TRAIN=fx-filter-train.wav; OUT=fx-filter-out.wav; REF=fx-filter-ref.wav; DB=fx-filter-diag.cbor
SESSF=fx-filter.sessions; HOSTOUT=fx-filter-host.out; SOCK=/tmp/harp-fxf-panel.sock
DEVLOG=/tmp/fx-filter-dev.log; LOG=/tmp/fx-filter-host.log

# fail() works from anywhere, including inside $(...): the message goes to the script's own
# stderr (fd 3, saved before any redirection) and the MAIN shell is terminated via TERM.
exec 3>&2
fail() { echo "FX-FILTER FAIL: $1" >&3; kill -s TERM $$; exit 1; }
pass() { echo "  ✓ $1"; }
[ -n "$FXDEVICED" ] && [ -x "$FXDEVICED" ] || fail "harp-fx-filter not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"
[ -n "$FXPLUG" ] && find "$FXPLUG/Contents" -type f -name 'harp-fx-shell*' 2>/dev/null | grep -q . \
    || fail "harp-fx-shell.vst3 not built (no module in ${FXPLUG:-<not found>})"

cleanup() {
    kill -9 "${DP:-}" 2>/dev/null; wait "${DP:-}" 2>/dev/null
    rm -rf "$STATEDIR" "$STATEFILE" "$STATEFILE.legacy" "$NOISE" "$TRAIN" "$OUT" "$REF" "$DB" \
           "$SESSF" "$HOSTOUT" fx-filter.dp
}
rm -rf "$STATEDIR" "$STATEFILE" "$NOISE" "$OUT" "$SOCK"; : > "$DEVLOG"; echo 0 > "$SESSF"
PANEL=(); [ "$WIN" = 0 ] && PANEL=(--panel-sock "$SOCK")
"$FXDEVICED" --port "$PORT" --state-dir "$STATEDIR" "${PANEL[@]}" >"$DEVLOG" 2>&1 &
DP=$!
trap cleanup EXIT
trap 'cleanup; exit 1' INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

export HARP_ETH_DEVICE="127.0.0.1:$PORT"
export HARP_DEVICE_SERIAL="SIM-0001"
export HARP_RECONCILE_TIMEOUT_MS=1000 # interactive recall path: archive the displaced state (T6)
PD="-d $HARP_ETH_DEVICE"

# Every client below — a host render or a probe call — is exactly ONE device session, and the
# device serves one session at a time: it returns to accept() only after tearing the previous
# one down. A client that connects earlier waits in the listen backlog, on a loaded runner long
# enough to miss the runtime's 2 s hello bound — and an offline render whose first connect fails
# runs DISCONNECTED (silence, no recall), which is how this test once flaked on windows-2022.
# So each client first waits for the device to have ended every session opened so far (the
# count lives in a file: clients also run inside $(...) subshells). Deterministic, not timed.
idle() {
    local want; want=$(cat "$SESSF")
    for _ in $(seq 1 600); do
        [ "$(grep -c 'session ended; awaiting reattach' "$DEVLOG")" -ge "$want" ] && return 0
        sleep 0.05
    done
    cat "$DEVLOG" >&3; fail "the device did not finish session $want within 30 s"
}
session() { idle; echo $(( $(cat "$SESSF") + 1 )) > "$SESSF"; }
probe() { session; "$PROBE" $PD "$@"; }
# A render is hard-bounded (a no-connect would otherwise supervise for hot-plug forever) and
# must connect on its FIRST attempt; its output is also kept in $HOSTOUT for the checks.
host() {
    session
    perl -e 'alarm 60; exec @ARGV' "$HOSTBIN" "$FXPLUG" "$@" 2>&1 | tee "$HOSTOUT"
    local rc=${PIPESTATUS[0]}
    if ! grep -q "connected:" "$HOSTOUT" || grep -q "supervising for hot-plug\|hello failed" "$HOSTOUT"; then
        cat "$HOSTOUT" >&3; fail "a render did not connect to the device on its first attempt ($*)"
    fi
    return "$rc"
}
reported() { sed -nE 's/.*reported-samples=([0-9]+).*/\1/p' "$HOSTOUT" | head -1; }
# "0,e1,e1+e2,...": the delays a live wet may legitimately have beyond the reported one —
# each late-guard re-anchor logs how much it added ("trails by N more frames")
extras() { sed -nE 's/.*trails by ([0-9]+) more frames.*/\1/p' "$HOSTOUT" | awk 'BEGIN{t=0; printf "0"} {t+=$1; printf ",%d", t} END{print ""}'; }
hash_of() { sed -n 's/^output-hash: //p'; }
param() { probe params 2>/dev/null | sed -nE "s/^ *\[$1\].*[[:space:]]([0-9.]+)$/\1/p"; }
counter() { probe counters 2>/dev/null | sed -nE "s/^ *(x\.[a-z0-9.-]+\.)?$1 = ([0-9]+).*/\2/p" | head -1; }

# WAV analysis (stdlib only: runs on the Windows runner's python too).
#   rms FILE FROM TO          RMS of the left channel over [FROM, TO)
#   quarters FILE             brightness (first-difference / signal energy) per quarter
#   impulse FILE              first non-zero sample
#   loud FILE                 first sample with |x| > 0.01
#   rmstail FILE              RMS of the left channel over the second half
#   firstdiff FILE OTHER      first sample where the two renders differ
#   aligned LIVE REF ROFF R N0 P FROM EXTRAS
#                             every impulse's live wet equals REF's exactly at +R (+ a logged
#                             re-anchor extra), or is zero; nothing anywhere else. Prints
#                             "INTACT TOTAL EXTRA" (impulses from FROM on) or "MISMATCH ..."
#   onsets FILE THR           every sample with |x| > THR, comma-separated
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
elif op == 'rmstail':
    s = x[len(x) // 2:]; print('%.6f' % math.sqrt(sum(v * v for v in s) / len(s)))
elif op == 'loud':
    print(next((i for i, v in enumerate(x) if abs(v) > 0.01), -1))
elif op == 'firstdiff':
    y = left(sys.argv[3])
    print(next((i for i, (a, b) in enumerate(zip(x, y)) if a != b), -1))
elif op == 'aligned':
    # aligned LIVE REF ROFF REP N0 PER FROM EXTRAS: an impulse train (first at N0, every
    # PER) through the effect. REF is its offline render (wet at +ROFF); in LIVE each
    # impulse's wet window must equal REF's EXACTLY at +REP+e for e in EXTRAS (0 first,
    # then the cumulative re-anchor extras the host logged), in non-decreasing order over
    # time — or be exactly zero (a padded block / a gap / a disconnect). Every live sample
    # outside the windows so placed must be exactly zero: nothing may land anywhere else.
    ref = left(sys.argv[3]); roff, rep = int(sys.argv[4]), int(sys.argv[5])
    n0, per, frm = int(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8])
    extras = [int(e) for e in sys.argv[9].split(',')]
    W = per // 2
    covered = bytearray(len(x))
    level = intact = tot = 0
    for I in range(n0, len(x), per):
        for c in range(level, len(extras)):
            s = I + rep + extras[c]
            n = min(W, len(x) - s, len(ref) - (I + roff))  # the render's end cuts the last window
            if n <= 0:
                continue
            w, seg = x[s:s + n], ref[I + roff:I + roff + n]
            if not any(w):
                continue
            if all(a == b or a == 0.0 for a, b in zip(w, seg)):
                level = c
                for j in range(s, s + n):
                    covered[j] = 1
                if I >= frm and n == W:
                    tot += 1
                    intact += all(a == b for a, b in zip(w, seg))
                break
        else:
            if I >= frm and I + rep + extras[level] + W <= min(len(x), len(ref) + rep - roff):
                tot += 1  # nothing of it came through: padded / dropped, counted not intact
    stray = next((i for i, v in enumerate(x) if v != 0.0 and not covered[i]), -1)
    if stray >= 0:
        print('MISMATCH %d %r (wet outside every allowed position)' % (stray, x[stray])); sys.exit(0)
    print('%d %d %d' % (intact, tot, extras[level]))
elif op == 'onsets':
    print(','.join(str(i) for i, v in enumerate(x) if abs(v) > float(sys.argv[3])))
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
# an impulse train (0.9, first at 2400, every 4800) for the latency + dry/wet checks
python3 - "$TRAIN" <<'EOF'
import struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
fr = bytearray()
for i in range(48000):
    v = 29491 if i >= 2400 and (i - 2400) % 4800 == 0 else 0
    fr += struct.pack('<hh', v, v)
w.writeframes(bytes(fr))
EOF

echo "── fx-filter: $FXDEVICED on 127.0.0.1:$PORT, driven by $(basename "$FXPLUG")"

# ---- T1 identity ----
ID=$(probe identify 2>&1) || { echo "$ID"; fail "T1 probe identify"; }
echo "$ID" | grep -q "audio.fx" || { echo "$ID"; fail "T1 device does not advertise audio.fx"; }
echo "$ID" | grep -q "engine: fx-filter 1.0.0" || { echo "$ID"; fail "T1 wrong engine identity"; }
pass "T1 identity: audio.fx, engine fx-filter 1.0.0"

# ---- T2 processing: the track audio is filtered by the device ----
host --set 1=1.0 --set 2=0.0 --input sine:1000 --seconds 0.3 --out "$OUT" >"$LOG" 2>&1 \
    || { cat "$LOG"; fail "T2 open render"; }
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
pass "T5 sample-accurate: automation applied on its exact input sample at blocks 64/256/1024"

# ---- T6 recall ----
A0=$(probe refs 2>/dev/null | grep -c "archive/")
SET=(--set 1=0.31 --set 2=0.64 --set 50=0.35) # 50 = the host-side dry/wet Mix
HPRE=$(host "${SET[@]}" --input "wav:$NOISE" --seconds 0.5 --hash --save-state "$STATEFILE" 2>/dev/null | hash_of)
[ -n "$HPRE" ] && [ -s "$STATEFILE" ] || fail "T6 save render (no hash or empty state)"
probe knob 1 0.90 >/dev/null 2>&1 || fail "T6 knob 1 mutate"
probe knob 2 0.05 >/dev/null 2>&1 || fail "T6 knob 2 mutate"
[ "$(param 1)" = "0.900" ] || fail "T6 the mutation did not reach the device (Cutoff $(param 1))"
host --load-state "$STATEFILE" --input "wav:$NOISE" --seconds 0.5 --hash >"$LOG" 2>&1 \
    || { cat "$LOG"; fail "T6 load render"; }
grep -q "restored\|SYNCED" "$LOG" || { cat "$LOG"; fail "T6 no recall action logged"; }
HPOST=$(hash_of <"$LOG")
[ "$HPRE" = "$HPOST" ] || fail "T6 restored render $HPOST != saved render $HPRE (lossy recall)"
C=$(param 1); R=$(param 2)
python3 -c "import sys; sys.exit(0 if abs(float('${C:-9}')-0.31)<0.001 and abs(float('${R:-9}')-0.64)<0.001 else 1)" \
    || fail "T6 params not restored: Cutoff=${C:-?} Resonance=${R:-?} (want 0.31 / 0.64)"
A1=$(probe refs 2>/dev/null | grep -c "archive/")
[ "$A1" -gt "$A0" ] || fail "T6 the displaced device state was not archived ($A0 -> $A1)"
# an older build's state is the bare recall bundle (no 'HF1' + Mix header): it must still
# restore the device (Mix falls back to 100% wet)
probe knob 1 0.90 >/dev/null 2>&1 || fail "T6 knob 1 re-mutate"
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
# (headless recall: this checks the state FORMAT, not the §11.4 reconcile offer, so push at
# once rather than wait out the 1 s offer window the archive check above uses)
HARP_RECONCILE_TIMEOUT_MS=0 host --load-state "$STATEFILE.legacy" --input "wav:$NOISE" --seconds 0.5 >"$LOG" 2>&1 \
    || { cat "$LOG"; fail "T6 legacy-state load render"; }
rm -f "$STATEFILE.legacy"
grep -q "restored\|SYNCED\|Push" "$LOG" || { cat "$LOG"; fail "T6 legacy state: no recall action logged"; }
[ "$(param 1)" = "0.310" ] || { cat "$LOG"; fail "T6 legacy (header-less) state did not restore the device (Cutoff $(param 1))"; }
# Two sessions displacing device state in the same wall-clock second collide on the
# second-granularity archive name; the second push used to abort ("project state apply
# failed" — the recall silently not applied: this test's windows-2022 flake). Pin the
# timestamp so the collision is certain: both recalls must apply, archived as <ts>, <ts>.001.
for i in 1 2; do
    probe knob 1 0.90 >/dev/null 2>&1 || fail "T6 archive-collision mutate $i"
    HARP_TEST_ARCHIVE_TS=collide HARP_RECONCILE_TIMEOUT_MS=0 host --load-state "$STATEFILE" \
        --input "wav:$NOISE" --seconds 0.3 >"$LOG" 2>&1 || { cat "$LOG"; fail "T6 archive-collision recall $i"; }
    [ "$(param 1)" = "0.310" ] || { cat "$LOG"; fail "T6 recall $i with a colliding archive name was not applied (Cutoff $(param 1))"; }
done
ARCH=$(probe refs 2>/dev/null | grep -oE 'archive/collide(\.[0-9]+)?' | sort | tr '\n' ' ')
[ "$ARCH" = "archive/collide archive/collide.001 " ] || fail "T6 colliding archives not both kept: [$ARCH]"
pass "T6 recall: Cutoff $C Resonance $R + Mix restored, render byte-identical ($HPOST), archives $A0 -> $A1; legacy state loads; same-second recalls from two sessions both apply (archived $ARCH)"

# ---- T7 front-panel echo -> DAW automation ----
if [ "$WIN" = 1 ]; then
    echo "  ⏭ T7 echo skipped: the MinGW device's front panel is a stub (no AF_UNIX panel socket)"
else
    panel() { python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect('$SOCK'); s.send(sys.argv[1].encode()+b'\n'); s.recv(256); s.close()" "$1"; }
    : >"$LOG"
    ( for _ in $(seq 1 60); do grep -q "connected:" "$LOG" 2>/dev/null && break; sleep 0.1; done
      sleep 0.3; panel "knob 0 1 0.42"; sleep 0.3; panel "knob 0 2 0.77" ) &
    INJ=$!
    host --input "wav:$NOISE" --seconds 3 --realtime >"$LOG" 2>&1 || true
    kill -9 "$INJ" 2>/dev/null; wait "$INJ" 2>/dev/null
    grep -q "echo: param 1 -> 0.4200" "$LOG" || { grep "echo:" "$LOG"; fail "T7 Cutoff knob not echoed to the plugin"; }
    grep -q "echo: param 2 -> 0.7700" "$LOG" || { grep "echo:" "$LOG"; fail "T7 Resonance knob not echoed to the plugin"; }
    grep -E "echo: param ([0-9]+)" "$LOG" | grep -vE "echo: param [12] " \
        && fail "T7 the shell forwarded echoes for ids that are not its params (e.g. meters)"
    pass "T7 echo: front-panel Cutoff/Resonance moves surfaced as plugin automation"
fi

# ---- T8 the wet arrives exactly the reported latency after its input ----
# Offline (deterministic): the first wet sample of an impulse sits at the reported latency.
for BLK in 64 256 1000; do
    host --block "$BLK" --set 1=1.0 --set 2=0.0 --input impulse --seconds 0.3 --out "$OUT" >/dev/null 2>&1 \
        || fail "T8 offline render (block $BLK)"
    R=$(reported); AT=$(wav impulse "$OUT")
    echo "     offline block $BLK: reported $R, wet at $AT"
    [ -n "$R" ] && [ "$AT" = "$R" ] || fail "T8 offline block $BLK: wet at $AT, reported latency ${R:-?}"
done
# Live: the device's render is deterministic, so every live impulse's wet must EQUAL its
# offline render exactly at the reported latency — or be exactly zero, where a scheduler
# hiccup padded a block (its late wet is then dropped to keep the delay). If the runner
# stalled long enough for the late guard to re-anchor, the host logged the extra delay, and
# the wet must then sit exactly there instead (never anywhere else). A misplaced sample can
# never pass; a stalled runner can never fail. At least half the impulses must come through
# intact, so a mostly-padded run does not pass vacuously.
host --block 256 --set 1=1.0 --set 2=0.0 --input "wav:$TRAIN" --seconds 1 --out "$REF" >/dev/null 2>&1 \
    || fail "T8 offline reference render"
ROFF=$(reported)
for BLK in 256 1024; do
    host --realtime --block "$BLK" --set 1=1.0 --set 2=0.0 --input "wav:$TRAIN" --seconds 1 --out "$OUT" \
        >/dev/null 2>&1 || fail "T8 live render (block $BLK)"
    R=$(reported); EX=$(extras)
    SH=$(wav aligned "$OUT" "$REF" "$ROFF" "$R" 2400 4800 0 "$EX")
    case "$SH" in MISMATCH*) fail "T8 live block $BLK: wet not at the reported latency $R ($SH)";; esac
    set -- $SH
    echo "     live block $BLK: reported $R, $1 of $2 impulses' wet sample-exact at +$R$( [ "$3" = 0 ] || echo " (+$3 after a logged re-anchor)"), nothing misplaced"
    [ "$2" -ge 1 ] && [ $(( 2 * $1 )) -ge "$2" ] \
        || fail "T8 live block $BLK: only $1 of $2 impulses came through (the runner starved the stream)"
done
pass "T8 latency: the wet arrives exactly the reported latency after its input, offline and live"

# ---- T9 dry/wet alignment at Mix 50% ----
# Cutoff at 20 Hz leaves the wet of each impulse tiny, so every sample over 0.3 is the DRY
# half (0.45): each must sit at exactly impulse + the reported latency. The dry never
# crosses the wire and is never padded, so this is exact live as well as offline.
for RT in "" --realtime; do
    host --block 256 $RT --set 1=0.0 --set 2=0.0 --set 50=0.5 --input "wav:$TRAIN" --seconds 1 --out "$OUT" \
        >/dev/null 2>&1 || fail "T9 render ${RT:-offline}"
    R=$(reported)
    GOT=$(wav onsets "$OUT" 0.3)
    WANT=$(python3 -c "import sys; r=int(sys.argv[1]); print(','.join(str(k + r) for k in range(2400, 48000, 4800) if k + r < 48000))" "$R")
    [ "$GOT" = "$WANT" ] || fail "T9 ${RT:-offline}: dry at [$GOT], want [$WANT] (the reported latency $R after each impulse)"
done
pass "T9 dry/wet: at Mix 50% the dry lands exactly on the wet's latency, offline and live"

# ---- T10 the live late guard ----
# With the whole latency budget removed the wet can never be on time. Paying the pad debt
# would drop every block (a silent insert); the guard must re-anchor and the rest of the
# render must carry the wet (0.35 rms when intact; 0.0 without the guard).
HARP_FX_TEST_UNDERBUDGET=100000 host --realtime --block 256 --set 1=1.0 --input sine:1000 --seconds 2 \
    --out "$OUT" --diag-bundle "$DB" >"$LOG" 2>&1 || { cat "$LOG"; fail "T10 late live render"; }
grep -q "FX re-anchor" "$LOG" || { cat "$LOG"; fail "T10 no re-anchor logged"; }
TAIL=$(wav rmstail "$OUT")
python3 -c "import sys; sys.exit(0 if $TAIL > 0.2 else 1)" \
    || fail "T10 persistently-late wet went silent (second-half rms $TAIL) — the late guard did not re-anchor"
if python3 -c "import cbor2" 2>/dev/null; then
    N=$(python3 -c "import cbor2,sys; print(cbor2.loads(open(sys.argv[1],'rb').read())[5]['x.harp.fx_reanchors'])" "$DB") \
        || fail "T10 diag bundle unreadable"
    [ "$N" = 1 ] || fail "T10 x.harp.fx_reanchors = $N, want exactly 1 (one re-anchor settles a constant lateness)"
    CNT="x.harp.fx_reanchors=$N"
else
    CNT="(cbor2 absent: counter not decoded)"
fi
pass "T10 late guard: persistently-late wet re-anchored and stayed audible (tail rms $TAIL, $CNT)"

# ---- T11 live automation is applied on time ----
# An LFO on Cutoff (dense sub-block points) and a ramp on Resonance, live, at two block
# sizes: the effect's events are stamped at their input's SSI and ordered by the input gate
# (queued before that input is written), so none may land late or wedge a fence.
EL0=$(counter evt_late); RL0=$(counter ramp_late); FT0=$(counter fence_timeouts)
for BLK in 64 256; do
    host --realtime --block "$BLK" --input "wav:$NOISE" --lfo 1=3:4 --ramp 2=0.1:0.6 --seconds 2 \
        >/dev/null 2>&1 || fail "T11 live automation render (block $BLK)"
done
EL1=$(counter evt_late); RL1=$(counter ramp_late); FT1=$(counter fence_timeouts)
[ "$EL1" = "$EL0" ] && [ "$RL1" = "$RL0" ] && [ "$FT1" = "$FT0" ] \
    || fail "T11 live automation late: evt_late $EL0->$EL1, ramp_late $RL0->$RL1, fence_timeouts $FT0->$FT1"
pass "T11 live events: dense live automation at blocks 64/256 applied on time (evt_late, ramp_late, fence_timeouts unchanged)"

# ---- T12 reconnect + late connect: the live wet realigns exactly ----
# A fresh device (default params, fresh state: a hard kill loses uncommitted state, so every
# render here must see the same device state). The runtime reconnects on its own (~1 s
# retry); the audio thread then adopts the new session's domain. Every live sample must be
# the offline render shifted by the reported latency, or exactly zero (while disconnected /
# pre-rolling) — and the wet must be sample-exact again after the (re)connect.
kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null
rm -rf "$STATEDIR"; : > "$DEVLOG"; echo 0 > "$SESSF"
startdev() { "$FXDEVICED" --port "$PORT" --state-dir "$STATEDIR" "${PANEL[@]}" >>"$DEVLOG" 2>&1 & DP=$!; }
listening() { for _ in $(seq 1 50); do [ "$(grep -c "listening on $PORT" "$DEVLOG")" -ge "$1" ] && return 0; sleep 0.1; done; return 1; }
startdev; listening 1 || fail "T12 fresh device did not start"
python3 - "$TRAIN" <<'EOF'
import struct, sys, wave
w = wave.open(sys.argv[1], 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
fr = bytearray()
for i in range(8 * 48000):
    v = 29491 if i >= 2400 and (i - 2400) % 4800 == 0 else 0
    fr += struct.pack('<hh', v, v)
w.writeframes(bytes(fr))
EOF
host --block 256 --input "wav:$TRAIN" --seconds 8 --out "$REF" >/dev/null 2>&1 || fail "T12 offline reference"
ROFF=$(reported)
# tail N FILE: impulses (of those due at t >= FROM s) whose wet is intact
realigned() { # realigned WHAT FROM_S
    local r sh
    r=$(sed -nE 's/.*reported-samples=([0-9]+).*/\1/p' "$HOSTOUT" | head -1)
    grep -q "connected:" "$HOSTOUT" || { cat "$HOSTOUT" >&3; fail "T12 $1: never connected"; }
    sh=$(wav aligned "$OUT" "$REF" "$ROFF" "$r" 2400 4800 $((2400 + 4800 * ($2 * 10))) "$(extras)")
    case "$sh" in MISMATCH*) cat "$HOSTOUT" >&3; fail "T12 $1: a wet sample is misplaced after the (re)connect ($sh)";; esac
    set -- "$1" $sh
    echo "     $1: $2 of $3 impulses after the (re)connect window sample-exact at +$r$( [ "$4" = 0 ] || echo " (+$4 after a logged re-anchor)")"
    [ "$3" -ge 1 ] && [ $(( 2 * $2 )) -ge "$3" ] || fail "T12 $1: only $2 of $3 impulses realigned after the (re)connect"
}
# (a) restart mid-render: kill the device 1.5 s into an 8 s live render, bring it back.
# (Truncate the host log BEFORE the watcher starts: it must key on THIS render's connect,
# not the previous render's line.)
: > "$HOSTOUT"
( for _ in $(seq 1 100); do grep -q "connected:" "$HOSTOUT" 2>/dev/null && break; sleep 0.05; done
  sleep 1.5; kill -9 "$DP" 2>/dev/null; sleep 0.3
  "$FXDEVICED" --port "$PORT" --state-dir "$STATEDIR" "${PANEL[@]}" >>"$DEVLOG" 2>&1 & echo $! > fx-filter.dp ) &
RST=$!
perl -e 'alarm 60; exec @ARGV' "$HOSTBIN" "$FXPLUG" --realtime --block 256 --input "wav:$TRAIN" --seconds 8 \
    --out "$OUT" >"$HOSTOUT" 2>&1 || { cat "$HOSTOUT" >&3; fail "T12 restart render"; }
wait "$RST"; DP=$(cat fx-filter.dp); rm -f fx-filter.dp
grep -q "device reconnected" "$HOSTOUT" || { cat "$HOSTOUT" >&3; fail "T12 the runtime did not reconnect after the device restart"; }
realigned "device restart mid-render" 5
# (b) late connect: the device comes up only after the plugin activated without it. Ordered
# by EVENT, not by a sleep: the device starts once the host has logged that it found no
# device and is supervising for hot-plug (plugin load time varies a lot across runners —
# a fixed delay let the device beat activation on windows-2022, so it was no late connect).
kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null
: > "$HOSTOUT"
( for _ in $(seq 1 600); do grep -q "supervising for hot-plug" "$HOSTOUT" 2>/dev/null && break; sleep 0.05; done
  "$FXDEVICED" --port "$PORT" --state-dir "$STATEDIR" "${PANEL[@]}" >>"$DEVLOG" 2>&1 & echo $! > fx-filter.dp ) &
LATE=$!
perl -e 'alarm 60; exec @ARGV' "$HOSTBIN" "$FXPLUG" --realtime --block 256 --input "wav:$TRAIN" --seconds 8 \
    --out "$OUT" >"$HOSTOUT" 2>&1 || { cat "$HOSTOUT" >&3; fail "T12 late-connect render"; }
wait "$LATE"; DP=$(cat fx-filter.dp); rm -f fx-filter.dp
grep -q "supervising for hot-plug" "$HOSTOUT" || { cat "$HOSTOUT" >&3; fail "T12 late connect: the plugin activated WITH a device — not a late connect"; }
realigned "device up after the render started" 5
# the latency was latched at activation WITHOUT the device's declared pipeline (no device
# yet), so the contract is a loud warning when the device then connects with one
grep -q "is not in the FX latency reported to the host" "$HOSTOUT" \
    || { cat "$HOSTOUT" >&3; fail "T12 late connect: no warning that the device pipeline is missing from the reported latency"; }
# (c) automation across a restart: an LFO on Cutoff through a mid-render device restart.
# Events stamped before the audio thread adopts the new session are delivered "now" (never
# with an old-domain timestamp — unit-tested in test_fx_event_domain_restamp); here the
# restarted device must see nothing late and no fence expire.
: > "$HOSTOUT"
( for _ in $(seq 1 100); do grep -q "connected:" "$HOSTOUT" 2>/dev/null && break; sleep 0.05; done
  sleep 1.5; kill -9 "$DP" 2>/dev/null; sleep 0.3
  "$FXDEVICED" --port "$PORT" --state-dir "$STATEDIR" "${PANEL[@]}" >>"$DEVLOG" 2>&1 & echo $! > fx-filter.dp ) &
RST=$!
perl -e 'alarm 60; exec @ARGV' "$HOSTBIN" "$FXPLUG" --realtime --block 256 --input "wav:$NOISE" --lfo 1=2:4 \
    --seconds 6 --out "$OUT" >"$HOSTOUT" 2>&1 || { cat "$HOSTOUT" >&3; fail "T12 automated restart render"; }
wait "$RST"; DP=$(cat fx-filter.dp); rm -f fx-filter.dp
grep -q "device reconnected" "$HOSTOUT" || { cat "$HOSTOUT" >&3; fail "T12 (c): no reconnect"; }
EL=$("$PROBE" $PD counters 2>/dev/null | sed -nE 's/^ *evt_late = ([0-9]+).*/\1/p')
FT=$("$PROBE" $PD counters 2>/dev/null | sed -nE 's/^ *x\.[a-z0-9.-]+\.fence_timeouts = ([0-9]+).*/\1/p')
[ "${EL:-x}" = 0 ] && [ "${FT:-x}" = 0 ] \
    || fail "T12 automation across the restart: evt_late=${EL:-?} fence_timeouts=${FT:-?} on the restarted device"
pass "T12 reconnect: after a mid-render device restart and after a late connect, the live wet is sample-exact at the reported latency again; automation across a restart lands on time"

echo "FX-FILTER PASS (§8.8 effect: processing, automation, sample accuracy, recall, latency, reconnect$( [ "$WIN" = 1 ] || echo ', echo'))"
