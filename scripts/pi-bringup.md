# Raspberry Pi 4B bring-up plan

Two stages; both run the same `harp-deviced`.

## Stage 1 — TCP over LAN (works the moment SSH is up)

```sh
# on the Pi (Raspberry Pi OS / any Debian-ish arm64)
sudo apt install -y cmake build-essential
git clone <this repo> harp && cd harp
cmake -B build && cmake --build build
./build/harp-deviced --state-dir ~/harp-state --serial PI4B-0001
# on the Mac
./build/harp-probe -d <pi-ip>:47800 demo
```

## Stage 2 — USB gadget (the normative §4.3 binding)

The Pi 4B's USB-C port is the DWC2 OTG controller. Plan:

1. `/boot/firmware/config.txt`: `dtoverlay=dwc2` (+ `modules-load=dwc2` in
   `cmdline.txt`), reboot.
2. configfs gadget: vendor-specific interface class `0xFF`/`0x48`/`0x01`,
   one FunctionFS function (`ffs.harp`) exposing bulk IN/OUT for the framed
   link; VID 0x1209 (pid.codes) for prototyping.
3. New transport backend in `harp-deviced`: open `ep0`, write descriptors,
   service `ep1`/`ep2` — fd-based like the socket, so the session code is
   unchanged.
4. Host side: libusb backend for `harp-probe` (claims the vendor interface;
   no driver needed on macOS).

Known gaps vs spec on this hardware:
- BOS platform capability descriptor (§4.3.2) is not exposable via
  libcomposite without kernel patches → hosts use the interface-class
  probe fallback (§6.1 allows it).
- Intermediate bring-up shortcut if FunctionFS fights back: gadget
  ethernet (`g_ether`) + the stage-1 TCP transport over the USB cable.
