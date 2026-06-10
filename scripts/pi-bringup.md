# Raspberry Pi 4B reference device — setup and operations

Current state: **operational.** The Pi enumerates on the host as
`1209:4852 "harp-refdev"` (serial `PI4B-0001`), USB 2.0 High Speed, with
the framed link and the HARP stream on two bulk endpoint pairs.

## Provisioning a fresh Pi (what was actually done)

1. Debian 13 (trixie) arm64, ssh access, `cmake make gcc` installed.
2. Clone/rsync this repo to `~/harp`, `cmake -B build && cmake --build build`.
3. Peripheral mode + services: `sudo sh ~/harp/scripts/pi-stage2-install.sh`
   — comments out `otg_mode=1` (forces xhci host mode on the Pi 4), adds
   `dtoverlay=dwc2,dr_mode=peripheral`, installs `harp-gadget.service`
   (configfs skeleton via `pi-gadget.sh`; UDC bind is deferred to the
   daemon, which must write FunctionFS descriptors first) and
   `harp-deviced-usb.service`, then reboots.
4. Cable: the Pi's USB-C port to the Mac. **The same port is the Pi's
   power input** — the Mac powers the board; unplugging the cable
   power-cycles it (which the protocol survives; that's the point).

`harp-deviced-usb.service` Conflicts= the TCP `harp-deviced.service`:
one protocol stack owns the state store at a time.

## Day-to-day deploy loop (no sudo needed)

```sh
rsync -a --exclude build --exclude .git . jak@harp.local:~/harp/
ssh jak@harp.local 'cmake --build ~/harp/build'
./build/harp-probe -d usb dev-restart    # daemon exits; systemd respawns new binary
```

## Debugging notes (hard-won)

- **dwc2 debugfs** is the truth serum:
  `sudo cat /sys/kernel/debug/usb/fe980000.usb/state` — `DOEPTSIZ`/
  `DIEPTSIZ` residues show exactly how many bytes of a transfer moved
  before a stall.
- Which fd is a thread blocked on: `cat /proc/PID/task/TID/syscall`
  (field 2 = fd, see `/proc/PID/fd`).
- Bulk pairs deadlock when both sides block writing (no TCP-style
  buffering). Host-side cure is drain-on-stall — see spec §4.2.1 (0.3.1)
  and `host/usb_io.c` / `render_host_paced`.
- Passwordless sudo: the NOPASSWD line lives in `/etc/sudoers` proper;
  drop-ins under `/etc/sudoers.d/` were being wiped on reboot on this
  image (root cause not identified).
