#!/bin/sh
# Stage 2 install: switch the Pi to USB peripheral mode and install the
# gadget + USB daemon services. Run as root on the Pi. Reboots at the end.
#
#   sudo sh pi-stage2-install.sh [SERIAL]   (default PI4B-0001)
#
# Each board needs a DISTINCT serial: the host binds a project's recall
# bundle to a device by (product, serial), so PI4B-0001 (desk) and
# PI4B-0002 (CI/closet) can coexist on one host without confusion.
set -ex
SERIAL="${1:-PI4B-0001}"

# 1. dwc2 peripheral mode (otg_mode=1 forces xhci host mode on the Pi 4;
#    some images also ship an explicit dr_mode=host overlay — neutralize
#    both, then add peripheral, or the two dwc2 overlays fight)
sed -i "s/^otg_mode=1/#otg_mode=1  # disabled for HARP gadget mode/" /boot/firmware/config.txt
sed -i "s/^dtoverlay=dwc2,dr_mode=host/#&  # disabled for HARP gadget mode/" /boot/firmware/config.txt
grep -q "dtoverlay=dwc2,dr_mode=peripheral" /boot/firmware/config.txt || \
    printf "\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n" >> /boot/firmware/config.txt

# 2. gadget skeleton service
install -m755 "$(dirname "$0")/pi-gadget.sh" /usr/local/sbin/pi-gadget.sh
cat > /etc/systemd/system/harp-gadget.service <<EOF
[Unit]
Description=HARP USB gadget skeleton (configfs)
After=sys-kernel-config.mount

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/pi-gadget.sh $SERIAL

[Install]
WantedBy=multi-user.target
EOF

# 3. USB daemon service (mutually exclusive with the TCP one: one protocol
#    stack owns the state store at a time)
cat > /etc/systemd/system/harp-deviced-usb.service <<EOF
[Unit]
Description=HARP reference device daemon (USB gadget transport)
Requires=harp-gadget.service
After=harp-gadget.service
Conflicts=harp-deviced.service

[Service]
ExecStart=/home/ci/harp/build/harp-deviced --state-dir /home/ci/harp-state --serial $SERIAL --ffs /dev/ffs-harp
Restart=always
RestartSec=1
User=root

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl disable harp-deviced 2>/dev/null || true # absent on fresh boxes
systemctl enable harp-gadget harp-deviced-usb
# --- resilience: persistent journal + Wi-Fi watchdog -------------------
# (Wi-Fi sometimes fails to come up after a gadget-mode power-cycle —
#  observed twice; root cause still unknown because the first journals
#  were volatile. Persistence makes the next occurrence diagnosable;
#  the watchdog makes it self-healing meanwhile.)
mkdir -p /var/log/journal
systemd-tmpfiles --create --prefix /var/log/journal

cat > /etc/systemd/system/wifi-watchdog.service <<'UNIT'
[Unit]
Description=Recover wlan0 if it failed to come up (gadget-boot race)
[Service]
Type=oneshot
ExecStart=/bin/sh -c "ip -4 addr show wlan0 | grep -q inet || (echo wifi-watchdog: wlan0 has no IPv4, bouncing NetworkManager; systemctl restart NetworkManager)"
UNIT

cat > /etc/systemd/system/wifi-watchdog.timer <<'UNIT'
[Unit]
Description=Check wlan0 45 s after boot, then every 2 min
[Timer]
OnBootSec=45
OnUnitActiveSec=120
[Install]
WantedBy=timers.target
UNIT

systemctl daemon-reload
systemctl enable --now wifi-watchdog.timer

echo "stage 2 installed; rebooting"
reboot
