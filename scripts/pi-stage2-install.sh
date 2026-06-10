#!/bin/sh
# Stage 2 install: switch the Pi to USB peripheral mode and install the
# gadget + USB daemon services. Run as root on the Pi. Reboots at the end.
set -ex

# 1. dwc2 peripheral mode (otg_mode=1 forces xhci host mode on the Pi 4)
sed -i "s/^otg_mode=1/#otg_mode=1  # disabled for HARP gadget mode/" /boot/firmware/config.txt
grep -q "dtoverlay=dwc2,dr_mode=peripheral" /boot/firmware/config.txt || \
    printf "\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n" >> /boot/firmware/config.txt

# 2. gadget skeleton service
install -m755 /home/jak/harp/scripts/pi-gadget.sh /usr/local/sbin/pi-gadget.sh
cat > /etc/systemd/system/harp-gadget.service <<EOF
[Unit]
Description=HARP USB gadget skeleton (configfs)
After=sys-kernel-config.mount

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/sbin/pi-gadget.sh PI4B-0001

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
ExecStart=/home/jak/harp/build/harp-deviced --state-dir /home/jak/harp-state --serial PI4B-0001 --ffs /dev/ffs-harp
Restart=always
RestartSec=1
User=root

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl disable harp-deviced
systemctl enable harp-gadget harp-deviced-usb
echo "stage 2 installed; rebooting"
reboot
