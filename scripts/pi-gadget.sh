#!/bin/sh
# Create the HARP USB gadget skeleton via configfs (run as root on the Pi).
# UDC binding is done by harp-deviced after it writes the FunctionFS
# descriptors — binding earlier would enumerate a broken device.
set -e

G=/sys/kernel/config/usb_gadget/harp
SERIAL="${1:-PI4B-0001}"

modprobe libcomposite

if [ ! -d "$G" ]; then
    mkdir -p "$G"
    cd "$G"
    echo 0x1209 > idVendor   # pid.codes prototyping VID
    echo 0x4852 > idProduct  # "HR"
    echo 0x0200 > bcdUSB
    echo 0x0002 > bcdDevice   # 0x0002: gadget now advertises Windows MS-OS
                              # descriptors (below). The bump also gives Windows
                              # a fresh device id so it re-queries them instead
                              # of reusing a cached "no MS-OS" verdict.
    mkdir -p strings/0x409
    echo "$SERIAL" > strings/0x409/serialnumber
    echo "HARP Reference Project" > strings/0x409/manufacturer
    echo "harp-refdev" > strings/0x409/product
    mkdir -p configs/c.1/strings/0x409
    echo "HARP" > configs/c.1/strings/0x409/configuration
    echo 250 > configs/c.1/MaxPower
    mkdir -p functions/ffs.harp
    ln -sf "$G/functions/ffs.harp" configs/c.1/

    # Microsoft OS descriptors: make Windows auto-load WinUSB for the vendor
    # interface so libusb works with zero driver ceremony (no Zadig), the same
    # plug-and-play experience as macOS/Linux. The function already declares the
    # "WINUSB" compatible ID in its FunctionFS descriptors (device/ffs.c); this
    # gadget-level block is what makes the device answer the 0xEE OS-string +
    # vendor (0xcd) request so Windows actually asks for it. Inert elsewhere.
    echo 1 > os_desc/use
    echo 0xcd > os_desc/b_vendor_code
    echo MSFT100 > os_desc/qw_sign
    ln -sf "$G/configs/c.1" os_desc/c.1
fi

mkdir -p /dev/ffs-harp
mountpoint -q /dev/ffs-harp || mount -t functionfs harp /dev/ffs-harp
echo "harp gadget skeleton ready (UDC bind deferred to harp-deviced)"
