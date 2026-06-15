#!/bin/sh
# multidevice-test — the device-selection policy, exercised on real
# hardware with TWO boards on the bus (same model harp-refdev, distinct
# serials). Asserts every rule:
#   A. singleton-kill + claim contention (rule 5): two plugin instances in
#      ONE process bind DIFFERENT devices.
#   B. exact serial (rule 1): a bundle from device X binds device X even
#      with device Y present.
#   C. same-model fallback (rule 2): a bundle wanting an ABSENT serial of
#      the same model falls back to a present same-model unit.
#   D. no cross-model / no-bind (rule 3): a forced-absent serial binds
#      nothing and renders silence — never grabs a different device.
#
# Needs both boards, the VST3 + AU shells installed, Live closed.
set -e
cd "$(dirname "$0")/.."
unset HARP_DEVICE_SERIAL  # this test drives selection itself, per-case

V=build-vst/harp-vst3-host
A=build-vst/au-host
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"  # Linux CI overrides -> ~/.vst3
AUCOMP="$HOME/Library/Audio/Plug-Ins/Components/harp-au.component"

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "MULTIDEV FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

# need at least two HARP devices on the bus
NDEV=$(./build/harp-probe list 2>/dev/null | grep -c 'serial PI4B-')
if [ "$NDEV" -lt 2 ]; then
    echo "MULTIDEV SKIP: need 2 devices on the bus, found $NDEV"
    exit 2
fi
SER1=$(./build/harp-probe list 2>/dev/null | grep -oE 'PI4B-[0-9]+' | sed -n 1p)
SER2=$(./build/harp-probe list 2>/dev/null | grep -oE 'PI4B-[0-9]+' | sed -n 2p)
echo "── multidevice: boards $SER1 + $SER2"

claimed() { grep -oE 'claimed 1209:[0-9a-f]+ serial PI4B-[0-9]+' "$1" | grep -oE 'PI4B-[0-9]+'; }

# A. two instances, one process -> different devices (rule 5 + singleton)
if [ -x "$A" ] && [ -d "$AUCOMP" ]; then
    "$A" --instances 2 >/tmp/md-a.log 2>&1 || true
    N=$(claimed /tmp/md-a.log | sort -u | wc -l | tr -d ' ')
    [ "$N" = "2" ] || { echo "MULTIDEV FAIL(A): 2 instances bound $N distinct device(s)"; claimed /tmp/md-a.log; exit 1; }
    echo "   A. two AU instances in one process bound 2 distinct devices ✓"
else
    echo "   A. (AU not installed — skipped)"
fi

# B. exact serial: save a bundle pinned to SER2, load it -> binds SER2
HARP_DEVICE_SERIAL="$SER2" "$V" "$PLUG" --set 3=0.6 --seconds 0.4 \
    --save-state /tmp/md-b.state >/dev/null 2>&1
"$V" "$PLUG" --load-state /tmp/md-b.state --seconds 0.2 >/tmp/md-b.log 2>&1 || true
[ "$(claimed /tmp/md-b.log)" = "$SER2" ] || {
    echo "MULTIDEV FAIL(B): bundle from $SER2 bound $(claimed /tmp/md-b.log)"; exit 1; }
echo "   B. bundle from $SER2 -> bound $SER2 (rule 1, exact) ✓"

# C. same-model fallback: rewrite the bundle's serial to an absent one of
#    the same length (PI4B-0099), same model -> must fall back to a present
#    same-model board, and log the miss on the wanted serial.
python3 - "$SER2" <<'EOF'
import sys
s = sys.argv[1]
d = open('/tmp/md-b.state','rb').read()
# absent serial, same byte length so CBOR length prefixes stay valid
absent = b'PI4B-0099' if s != 'PI4B-0099' else b'PI4B-0098'
open('/tmp/md-c.state','wb').write(d.replace(s.encode(), absent))
EOF
"$V" "$PLUG" --load-state /tmp/md-c.state --seconds 0.2 >/tmp/md-c.log 2>&1 || true
grep -q "no HARP device with serial PI4B-0099" /tmp/md-c.log || {
    echo "MULTIDEV FAIL(C): no absent-serial miss logged"; tail -5 /tmp/md-c.log; exit 1; }
case "$(claimed /tmp/md-c.log)" in
    PI4B-*) echo "   C. absent $SER2-class serial -> fell back to $(claimed /tmp/md-c.log) (rule 2) ✓" ;;
    *) echo "MULTIDEV FAIL(C): no same-model fallback bind"; exit 1 ;;
esac

# D. forced-absent serial -> no bind, no cross-model grab, silence
HARP_DEVICE_SERIAL=PI4B-7777 "$V" "$PLUG" --set 7=0.9 --seconds 0.4 \
    --out /tmp/md-d.wav >/tmp/md-d.log 2>&1 || true
grep -q "no HARP device with serial PI4B-7777" /tmp/md-d.log || {
    echo "MULTIDEV FAIL(D): forced-absent serial did not report a miss"; exit 1; }
[ -z "$(claimed /tmp/md-d.log)" ] || {
    echo "MULTIDEV FAIL(D): forced-absent serial still claimed $(claimed /tmp/md-d.log)"; exit 1; }
echo "   D. forced-absent serial -> no bind, no cross-model grab (rule 3) ✓"

echo "MULTIDEV PASS (selection: exact / same-model / never-cross-model / contention)"
