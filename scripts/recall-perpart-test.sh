#!/bin/sh
# recall-perpart-test — proves PER-PART parameter recall (P3): distinct
# timbres set on different parts survive a save -> scramble -> reload
# round-trip, each part restored to its own value.
#
# This is the P3 promotion of the recall round-trip. recall-roundtrip.sh
# proves the device-wide Recall Bundle survives getState/setState; here we
# prove the bundle carries the SEPARATE per-part param state — that two parts
# tuned to different filter cutoffs each come back exactly as set, not
# collapsed onto one shared global timbre (the pre-P3 behaviour we guard).
#
# METHODOLOGY (matters — learned the hard way): the captures that we compare
# must render each part from its PERSISTED base value, with NO --set during
# the capture render. A param-set applied as an event mid-render smooths from
# the previous value (a transient), whereas a restored base value lands at
# s_init — so "set-during-capture" and "restored-base" renders differ even
# when recall is perfect. So we SET in dedicated setup invocations and CAPTURE
# pure (note only). Also note --channel is process-wide in the host, so each
# part is set in its OWN invocation; per-part values persist on the device
# (live ref) across invocations.
#
# We assert via the host's content hash ('--hash' -> 'output-hash: <hex>'),
# the same oracle golden-test.sh trusts: identical hash == identical audio.
#
#   1. SETUP    — set part 0 cutoff low (3=0.2), part 5 cutoff high (3=0.9).
#   2. CAPTURE  — render each part pure (note only); H0 != H5 proves the
#                 per-part cutoff is live (not a shared global), else the
#                 round-trip would be vacuous, so we fail fast.
#   3. SAVE     — getState (--save-state) serializes the whole 16-part bundle.
#   4. SCRAMBLE — swap the cutoffs (part 0 high, part 5 low) so a no-op reload
#                 can't pass: the device is now in a DIFFERENT state.
#   5. RELOAD   — setState (--load-state) the saved bundle, re-capture pure.
#                 R0==H0 and R5==H5 => each part's value was restored to ITS
#                 own part, losslessly.
#
# Run on a host with the device attached (the hardware runner).
# Env overrides: PLUG (shell path); HARP_DEVICE_SERIAL (which board).
# usage: scripts/recall-perpart-test.sh   (device must be unclaimed)
# exit:  0 PASS; 1 recall FAIL; 3 device not exclusively claimable.
set -u
cd "$(dirname "$0")/.."

# Pin one board on a multi-board bus (overridable for CI's closet rig).
export HARP_DEVICE_SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"

V=build-vst/harp-vst3-host
PLUG="${PLUG:-$HOME/Library/Audio/Plug-Ins/VST3/harp-shell.vst3}"  # Linux CI overrides -> ~/.vst3
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM

if pgrep -x "Live" >/dev/null 2>&1; then
    echo "RECALL-PERPART FAIL: device claimed by Ableton Live — the suite needs it exclusively"
    exit 3
fi

DUR=1.0      # seconds per capture — long enough for the note's envelope
NOTE=69      # A4

# set_part CH CUTOFF — persist part CH's filter cutoff (param 3). --channel is
# process-wide, so this whole invocation drives part CH. Short render to apply.
set_part() { "$V" "$PLUG" --channel "$1" --set 3="$2" --seconds 0.3 >/dev/null 2>&1; }
# cap CH — render part CH PURE (a note on its channel, no --set) and echo the
# content hash; the timbre comes entirely from the part's persisted base value.
cap() { "$V" "$PLUG" --channel "$1" --notes "$NOTE" --seconds "$DUR" --part "$1" --hash 2>/dev/null \
            | sed -n 's/^output-hash: //p'; }

echo "── recall-perpart: distinct per-part timbres survive save/scramble/reload ($HARP_DEVICE_SERIAL)"

# device-exclusive guard: a settle render must complete
set_part 0 0.5 || true
"$V" "$PLUG" --seconds 0.3 >/dev/null 2>&1 \
    || { echo "RECALL-PERPART FAIL: render did not complete (device busy/absent?)"; exit 3; }

# ---- 1+2: two distinct per-part timbres ----
echo "[perpart] set part 0 cutoff=0.2, part 5 cutoff=0.9; capture each ..."
set_part 0 0.2
set_part 5 0.9
H0=$(cap 0)
H5=$(cap 5)
if [ -z "$H0" ] || [ -z "$H5" ]; then
    echo "RECALL-PERPART FAIL: no render hash from host (render error / device absent?)"; exit 1
fi
if [ "$H0" = "$H5" ]; then
    echo "RECALL-PERPART FAIL(setup): part 0 and part 5 hashed identically ($H0) —"
    echo "   the per-part cutoff did not take effect (params global, not per-part); round-trip vacuous."
    exit 1
fi
echo "   distinct: part0=$H0  part5=$H5  (per-part cutoff is live)"

# ---- 3: save the whole 16-part bundle ----
echo "[perpart] save the 16-part Recall Bundle (getState) ..."
"$V" "$PLUG" --seconds 0.3 --save-state "$T/perpart.bundle" >/dev/null 2>&1
[ -s "$T/perpart.bundle" ] || { echo "RECALL-PERPART FAIL: getState saved no bundle"; exit 1; }

# ---- 4: scramble both parts (swap) so a no-op reload can't pass ----
echo "[perpart] scramble: part 0 cutoff=0.9, part 5 cutoff=0.2 ..."
set_part 0 0.9
set_part 5 0.2
S0=$(cap 0)
if [ "$S0" = "$H0" ]; then
    echo "RECALL-PERPART FAIL(setup): scramble had no effect on part 0 (still $H0) — test would be vacuous"; exit 1
fi

# ---- 5: reload the bundle, re-capture pure ----
echo "[perpart] reload the bundle (setState), re-capture parts 0 and 5 ..."
"$V" "$PLUG" --load-state "$T/perpart.bundle" --seconds 0.3 >/dev/null 2>&1
R0=$(cap 0)
R5=$(cap 5)
echo "   recalled: part0=$R0  part5=$R5  (want part0=$H0 part5=$H5)"

FAIL=0
[ -n "$R0" ] && [ -n "$R5" ] || { echo "RECALL-PERPART FAIL: no render hash after reload (setState broken?)"; FAIL=1; }
[ "$R0" = "$H0" ] || { echo "RECALL-PERPART FAIL: part 0 recalled $R0 != saved $H0 — per-part cutoff lost"; FAIL=1; }
[ "$R5" = "$H5" ] || { echo "RECALL-PERPART FAIL: part 5 recalled $R5 != saved $H5 — per-part cutoff lost"; FAIL=1; }

if [ "$FAIL" -eq 0 ]; then
    echo "RECALL-PERPART PASS (distinct per-part timbres restored losslessly across save/scramble/reload)"
    exit 0
fi
echo "RECALL-PERPART FAIL: per-part recall did not round-trip — see lines above"
exit 1
