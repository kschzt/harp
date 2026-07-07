#!/bin/sh
# rig-eth-provision — reproducibly provision the eth-hw rig Pi's two §8.7 Ethernet systemd units.
#
# WHY THIS IS SEPARATE FROM pi-stage2-install.sh: the general Pi provisioner (pi-stage2-install.sh)
# runs on EVERY HARP Pi and provisions the USB gadget + USB daemon. These two units are RIG-SPECIFIC
# — they hardcode the CI runner's trusted-peer IP (§16(b) rate-limit exemption) and the 478xx eth
# ports — so baking them into the shared provisioner would wrongly enable an eth daemon on the desk
# Pi. This script provisions ONLY the eth-hw rig board (PI4B-0002). Run once, as root, on that Pi:
#   sudo scripts/rig-eth-provision.sh                 # PI4B-0002 defaults
#   sudo SERIAL=PI4B-000X PEER=10.10.0.3 scripts/rig-eth-provision.sh   # another rig board
# Idempotent: re-running rewrites the units + reloads systemd. The daemon BINARY is rebuilt from the
# commit under test by the hw.yml eth-hw bring-up (`systemctl restart harp-deviced-eth`); this only
# owns the unit definitions. It replaces the previously hand-created units, which were
# un-reproducible (the tone unit's absence from any script was the SINAD gate's provenance gap).
#
# The two units:
#   harp-deviced-eth       — the synth daemon on :47800 (the §8.7 conformance-suite target). Conflicts
#                            with harp-deviced-usb (one transport at a time on the shared binary/state).
#   harp-deviced-eth-tone  — a --tone 1000 daemon on :47801 (the free-running SINAD fidelity source).
#                            Own state-dir + panel-sock so it coexists with the synth daemon; started
#                            and stopped explicitly by the hw.yml SINAD step, so no Conflicts= line.
set -eu

SERIAL="${SERIAL:-PI4B-0002}"          # device identity (matches the USB daemon's serial)
PEER="${PEER:-10.10.0.3}"              # CI runner IP — §16(b) pre-hello rate-limit exemption
HARP_DIR="${HARP_DIR:-/home/ci/harp}"  # the deployed tree; ExecStart points at its build/harp-deviced
USER_NAME="${USER_NAME:-ci}"
DEVICED="$HARP_DIR/build/harp-deviced"

[ "$(id -u)" = 0 ] || { echo "rig-eth-provision: must run as root (writes /etc/systemd/system)"; exit 1; }

cat > /etc/systemd/system/harp-deviced-eth.service <<UNIT
[Unit]
Description=HARP eth synth daemon (§8.7 conformance target, :47800)
Conflicts=harp-deviced-usb.service
After=network-online.target
Wants=network-online.target

[Service]
User=$USER_NAME
ExecStart=$DEVICED --port 47800 --state-dir /home/$USER_NAME/harp-eth-state --serial $SERIAL --trusted-peer $PEER
Restart=on-failure

[Install]
WantedBy=multi-user.target
UNIT

cat > /etc/systemd/system/harp-deviced-eth-tone.service <<UNIT
[Unit]
Description=HARP eth tone daemon (free-running SINAD source, :47801, 1 kHz)
After=network-online.target
Wants=network-online.target

[Service]
User=$USER_NAME
ExecStart=$DEVICED --port 47801 --state-dir /home/$USER_NAME/harp-tone-state --serial $SERIAL --trusted-peer $PEER --panel-sock /tmp/harp-tone-panel.sock --tone 1000
Restart=on-failure

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
# Enable both so a rig reboot restores them; the eth-hw job restarts/stops them per run, and the tone
# unit is left stopped between runs (the SINAD step stops it), so enabling is for reboot-persistence.
systemctl enable harp-deviced-eth.service harp-deviced-eth-tone.service >/dev/null 2>&1 || true

echo "rig-eth-provision: installed harp-deviced-eth (:47800) + harp-deviced-eth-tone (:47801) for $SERIAL (peer $PEER)"
echo "  binary: $DEVICED  (rebuilt from the commit under test by the hw.yml eth-hw bring-up)"
