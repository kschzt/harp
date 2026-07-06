#!/bin/sh
# free-running real-hop RTP fidelity gate — §8.7 audio over a REAL NIC.
#
# The byte-exact HOST-PACED path is gated by offline-golden-eth (PIN_BOUNCE_EXTERN). This is
# its FREE-RUNNING (live/ASRC) twin: it drives eth-rtp-test against a --tone harp-deviced over
# the real network hop and asserts the free-running path delivers the CORRECT audio CLEANLY:
#   - tone frequency within ±TOL_HZ of expected  -> the RIGHT audio arrives (not garbage/silence)
#   - zero packet loss / underflow / bad          -> clean §8.7 RTP delivery over the real link
#   - SINAD above a LOOSE floor                    -> bounded distortion
# The SINAD VALUE is intentionally floored, not tightly gated: over a real NIC the packet jitter
# drives eth-rtp-test's ASRC rate-centering trim, which FM-modulates the recovered tone (a HARNESS
# artifact, documented in tools/eth-rtp-test.c) — so the value swings run-to-run (~29-51 dB on the
# rig) while frequency + zero-loss are deterministic. The floor (20 dB, well under the observed
# min) catches a GROSS failure without flaking; frequency+loss are the real teeth.
set -u
EP="${1:?usage: freerun-sinad-eth-test.sh TONE_HOST:PORT [tone_hz]}"
TONE_HZ="${2:-1000}"
RTP="${RTP:-./build/harp-eth-rtp-test}"
TOL_HZ="${TOL_HZ:-5}"
SINAD_FLOOR="${SINAD_FLOOR:-15}"   # observed min ~28 dB on the rig; 15 = ~13 dB of margin so a loaded runner's worse jitter can't flake it

[ -x "$RTP" ] || { echo "FREERUN-SINAD SKIP: $RTP not built (needs libsamplerate)"; exit 2; }

echo "── free-running SINAD over the real §8.7 hop against $EP (${TONE_HZ} Hz tone)"
out=$("$RTP" "$EP" 6 47811 2>&1)
echo "$out" | grep -aE "audio.start|drift|SINAD|PASS|FAIL|hello" | sed 's/^/   /'

tone=$(printf '%s\n' "$out"  | grep -oE 'tone [0-9.]+ Hz'   | grep -oE '[0-9.]+' | head -1)
sinad=$(printf '%s\n' "$out" | grep -oE 'SINAD -?[0-9.]+ dB' | grep -oE -- '-?[0-9.]+' | head -1)
lost=$(printf '%s\n' "$out"  | grep -oE 'lost=[0-9]+'  | grep -oE '[0-9]+' | head -1)
under=$(printf '%s\n' "$out" | grep -oE 'under=[0-9]+' | grep -oE '[0-9]+' | head -1)
bad=$(printf '%s\n' "$out"   | grep -oE 'bad=[0-9]+'   | grep -oE '[0-9]+' | head -1)

python3 - "${tone:-}" "${sinad:-}" "${lost:-99}" "${under:-99}" "${bad:-99}" "$TONE_HZ" "$TOL_HZ" "$SINAD_FLOOR" <<'PY'
import sys
a = sys.argv
tone  = float(a[1]) if a[1] else -1.0
sinad = float(a[2]) if a[2] else -1e9
lost, under, bad = int(a[3]), int(a[4]), int(a[5])
exp, tol, floor = float(a[6]), float(a[7]), float(a[8])
p = []
if abs(tone - exp) > tol: p.append(f"tone {tone} Hz != {exp}±{tol}")
if lost:  p.append(f"packet loss {lost}")
if under: p.append(f"underflow {under}")
if bad:   p.append(f"bad packets {bad}")
if sinad < floor: p.append(f"SINAD {sinad} dB < floor {floor}")
if p:
    print("FREERUN-SINAD FAIL:", "; ".join(p)); sys.exit(1)
print(f"FREERUN-SINAD PASS: correct tone {tone} Hz (want {exp}±{tol}), SINAD {sinad} dB (>= {floor}), "
      f"0 loss / 0 underflow / 0 bad — free-running §8.7 RTP delivers clean audio over the real NIC")
PY
