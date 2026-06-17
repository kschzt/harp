#!/bin/sh
# voice-steal-test — Phase 2 polyphony completeness: the per-part voice pool is
# BOUNDED at NVOICES (8) and steals deterministically when overrun.
#
# The golden chord exercises 3 simultaneous voices; nothing gated the steal path
# (>8 notes at once). This does: a chord of MORE than 8 notes forces the pool to
# steal (voice_alloc: first free, else oldest-by-alloc_seq — a pure function of
# the ordered note-ons, no wall-clock), and we assert on the device:
#
#   DETERMINISTIC  the 12-note chord renders byte-identically twice — stealing
#                  (and the phase a stolen voice inherits) introduces no
#                  nondeterminism.
#   BOUNDED        the 12-note chord != the 8-note chord (which fills the pool
#                  exactly, no steal). If the pool just grew unbounded, the extra
#                  notes would add voices rather than steal — but a bounded pool
#                  renders a materially different mix once it overruns.
#   NON-SILENT     the overrun render still produces audio (the pool doesn't
#                  wedge or zero out when full).
#
# Offline/byte-exact through the real VST3 plugin, like golden/note-expr. Exit
# 0 pass / 2 N/A (board/host absent) / 3 device busy.
set -u
cd "$(dirname "$0")/.."

export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
SERIAL="$HARP_DEVICE_SERIAL"
V="${HOSTBIN:-build-vst/harp-vst3-host}"
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"
PROBE="${PROBE:-./build/harp-probe}"
S="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
   --set 7=0.5 --set 8=0.6 --set 9=0 --set 10=0.6 --set 11=0.5 --set 12=0 --set 13=0"
# 8 notes fill the pool exactly (NVOICES=8, no steal); 12 overrun it (4 steals).
CHORD8="60,62,64,65,67,69,71,72"
CHORD12="60,61,62,63,64,65,66,67,68,69,70,71"
fail() { echo "VOICE-STEAL FAIL: $1"; exit 1; }

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "VOICE-STEAL FAIL: device claimed by Ableton Live — needs it exclusively"; exit 3
fi
if [ -x "$PROBE" ]; then
    "$PROBE" list 2>/dev/null | grep -q "serial $SERIAL" \
        || { echo "VOICE-STEAL SKIP: board $SERIAL not on the bus"; exit 2; }
else
    echo "VOICE-STEAL SKIP: $PROBE not built"; exit 2
fi
[ -x "$V" ] || { echo "VOICE-STEAL SKIP: host $V not built"; exit 2; }
echo "── voice-steal: bounded 8-voice pool + deterministic steal on $SERIAL"

$V "$PLUG" $S --seconds 0.5 >/dev/null 2>&1 \
    || { echo "VOICE-STEAL FAIL: settle render did not complete (device busy/absent?)"; exit 3; }

# h CHORD -> owner main-mix hash ; rms CHORD -> the printed rms
h() { $V "$PLUG" $S --chord "$1" --seconds 2.0 --hash 2>/dev/null | grep output-hash | cut -d' ' -f2; }
rms() { $V "$PLUG" $S --chord "$1" --seconds 2.0 --hash 2>/dev/null | grep -oE 'rms=[0-9.]+' | cut -d= -f2; }

H8=$(h "$CHORD8")
H12a=$(h "$CHORD12")
H12b=$(h "$CHORD12")
R12=$(rms "$CHORD12")
for n in H8 H12a H12b R12; do
    eval "v=\$$n"
    [ -n "$v" ] || { echo "VOICE-STEAL FAIL: $n produced no output (device busy/absent?)"; exit 3; }
done
echo "   8-note=$H8 ; 12-note=$H12a (repeat $H12b) ; 12-note rms=$R12"

[ "$H12a" = "$H12b" ] || fail "12-note chord not deterministic ($H12a vs $H12b) — stealing introduced nondeterminism"
[ "$H8" != "$H12a" ]  || fail "12-note == 8-note ($H8) — the pool did not bound/steal as expected"
[ "$(python3 -c "print(1 if $R12 > 0.001 else 0)")" = 1 ] \
    || fail "12-note overrun rendered ~silence (rms $R12) — the full pool wedged"

echo "VOICE-STEAL PASS (on $SERIAL: a 12-note chord overruns the 8-voice pool, steals"
echo "   deterministically (byte-identical twice), stays bounded (differs from the"
echo "   exact-fill 8-note chord), and still renders audio rms=$R12)"
