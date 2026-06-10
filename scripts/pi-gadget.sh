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
    echo 0x0001 > bcdDevice
    mkdir -p strings/0x409
    echo "$SERIAL" > strings/0x409/serialnumber
    echo "HARP Reference Project" > strings/0x409/manufacturer
    echo "harp-refdev" > strings/0x409/product
    mkdir -p configs/c.1/strings/0x409
    echo "HARP" > configs/c.1/strings/0x409/configuration
    echo 250 > configs/c.1/MaxPower
    mkdir -p functions/ffs.harp
    ln -sf "$G/functions/ffs.harp" configs/c.1/
fi

mkdir -p /dev/ffs-harp
mountpoint -q /dev/ffs-harp || mount -t functionfs harp /dev/ffs-harp
echo "harp gadget skeleton ready (UDC bind deferred to harp-deviced)"
