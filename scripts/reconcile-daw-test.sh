#!/bin/sh
# reconcile-daw-test — the §11.4 four-action reconcile end-to-end in a real DAW
# (Phase 2B, layer 3). Layers 1 (the protocol relay) and 2 (the shell recall logic)
# are unit/CI-proven; this is the MANUAL hardware test that ties them together with a
# real conflict, the Electra, and your pick driving the action.
#
# Topology:
#   Mac (Ableton Live + the harp-shell VST3; harp-probe) <==USB==> Pi PI4B-0001
#       (harp-deviced + the Electra sidecar <- Electra One Mini)
#   The shell posts the offer to the Pi's mailbox over USB; the sidecar force-opens
#   the Electra Reconcile page; your pick returns to the shell, which executes it.
#
# This script does the automatable parts (verify the device refs); you drive Live +
# the Electra. Subcommands:
#   prereqs    one-time setup (what must be deployed/running before testing)
#   procedure  the per-action steps + expected outcomes
#   refs       show the device's refs (live + archive/ + duplicate/) to VERIFY an
#              action — run with Live CLOSED so the probe can claim the device
set -e
cd "$(dirname "$0")/.."
SERIAL="${HARP_DEVICE_SERIAL:-PI4B-0001}"
PROBE="${PROBE:-./build/harp-probe}"

case "${1:-procedure}" in
prereqs)
    cat <<'EOF'
── one-time setup ───────────────────────────────────────────────────────────
1. Pi (PI4B-0001 / harp.local) — layer-1 device code + the Electra sidecar:
     ssh jak@harp.local 'cd ~/harp && git pull && cmake --build build -j --target harp-deviced \
        && sudo systemctl restart harp-deviced'
   (harp-electra.service is already active — it surfaces the Reconcile page.)
2. Mac — install the layer-2 shell into Live's VST3 folder:
     cmake --build build-vst --target install-live
3. Launch Live WITH a generous reconcile window so you have time to pick on the
   Electra (the VST3 inherits the env of whatever launches it):
     HARP_RECONCILE_TIMEOUT_MS=30000 \
       /Applications/"Ableton Live 12 Suite.app"/Contents/MacOS/"Ableton Live 12 Suite"
   (0 = don't wait -> immediate archive-protected Push, the headless default.)
EOF
    ;;
procedure)
    cat <<'EOF'
── per-action test (repeat for each of the four) ────────────────────────────
BASELINE  In Live, add HARP on a track, move a few knobs, and SAVE the project.
          The shell pushes that state; device live == the project bundle (no offer).

STAGE     Make the device DIVERGE from the saved bundle WITHOUT Live knowing — turn
          a knob on the Electra (or the web panel). The device live ref is now dirty.

TRIGGER   Re-instantiate the recall: reload the project (or remove + re-add HARP).
          The shell recalls, sees the conflict, and posts the offer -> the Electra
          force-opens its Reconcile page.

PICK      On the Electra, click ONE of:  Push to Synth | Pull to DAW | Read-only |
          Duplicate.  The shell executes it.

VERIFY    Close Live (release the device), then:  scripts/reconcile-daw-test.sh refs

  EXPECTED per action:
   Push to Synth  device live == the project's state again (your Electra edit gone);
                  a new  archive/<ts>  ref holds the displaced edit (loss-free).
   Pull to DAW    device live UNCHANGED (your Electra edit stays); the host adopted
                  it — Live's next save captures the device's state. No archive ref.
   Read-only      device live UNCHANGED; no archive ref; nothing written either way.
   Duplicate      device live == the project's state; a  duplicate/<ts>  ref keeps
                  your edit as a device-visible named copy.

  Clean (no-conflict) sanity: with the device matching the bundle, reload -> NO offer
  (SYNCED silently). Unborn device -> first push, no offer.
EOF
    ;;
refs)
    [ -x "$PROBE" ] || { echo "build harp-probe first ($PROBE)"; exit 2; }
    echo "── device refs on $SERIAL (live + archive/ + duplicate/):"
    "$PROBE" -d "usb:$SERIAL" refs
    ;;
*)
    echo "usage: $0 {prereqs|procedure|refs}"; exit 2 ;;
esac
