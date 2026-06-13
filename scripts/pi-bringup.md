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

## Web panel

`harp-panel.service` runs `web/harp-panel.py` (Python sidecar) against
the daemon's panel API (`/tmp/harp-panel.sock`); browse
`http://harp.local:8080` — front panel sliders + live protocol
inspector. Frontends reach the engine only through the panel API's
`knob` command (the `front_panel_set` path): edits dirty the live ref
and echo to the host as §9.4 events.

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

## Second device + CI hardware runner (PI4B-0002, the closet box)

`harptest.local` / serial **PI4B-0002** is provisioned identically (same
`harp-refdev` product, distinct serial so it coexists with the desk
device on one host). Provisioning a fresh board, start to finish:

```sh
# on the box (one-time, needs your password once):
ssh -t jak@harptest.local 'sudo cp -n /etc/sudoers /etc/sudoers.orig && \
  echo "jak ALL=(ALL) NOPASSWD: ALL" | sudo tee -a /etc/sudoers >/dev/null && \
  sudo visudo -c'                           # passwordless sudo, survives reboot
ssh jak@harptest.local 'sudo apt-get install -y git cmake build-essential libusb-1.0-0-dev
  git clone https://github.com/kschzt/harp.git ~/harp
  cmake -B ~/harp/build ~/harp && cmake --build ~/harp/build -j4
  sudo sh ~/harp/scripts/pi-stage2-install.sh PI4B-0002'   # peripheral mode, services, reboot
```

Closet-survival is built in: services are `enable`d (come up on power),
journald is persistent, and `wifi-watchdog.timer` bounces NetworkManager
if wlan0 has no IP 45 s after boot. The USB-C cable from the NUC is both
power and data — powering the NUC port boots the Pi and enumerates the
gadget with no intervention (verified across a reboot).

### The NUC (GHA self-hosted runner) — when it's in the closet

The NUC is the *host*: it runs the GitHub Actions runner and drives
PI4B-0002 over USB. Setup (Linux host):

```sh
sudo apt-get install -y git cmake build-essential libusb-1.0-0-dev
sudo cp ~/harp/scripts/99-harp.rules /etc/udev/rules.d/ && sudo udevadm control --reload
# GitHub repo -> Settings -> Actions -> Runners -> New self-hosted runner
#   label it `harp-hw`; install as a service so it survives reboot.
```

A `hw` CI job keyed to `runs-on: [self-hosted, harp-hw]` then runs the
hardware suite against the plugged-in device every push/nightly:
`harp-probe` flows (recall, t15, record/render) run directly; the
shell-driven tests (soak/timing/tempo-lock) need the Linux VST3 build
(`cmake --build build-vst --target install-linux`) and a Linux-path
variant of `scripts/hw-tests.sh` (the current one assumes macOS plugin
paths + an Ableton claim guard). That variant is the first task once the
NUC is reachable.
