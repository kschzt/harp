# USB FunctionFS gadget "wedge" — root cause, evidence, and fix

**Status:** root-caused with a live kernel stack and a deterministic bench reproduction.
Two preventive host-side fixes are implemented + validated here (fix 1, fix 3). Host-side
*recovery* of an already-wedged gadget was measured to be **impossible on macOS** — see the
experiments — so the residual (uncatchable SIGKILL/crash) is handled by external auto-recovery.

## Symptom

Under the soak (and in principle after a DAW crash), a USB device (harp / harp2 =
PI4B-0001 / PI4B-0003) intermittently stops responding: the host claims it at the USB level
but the protocol hello/params never complete, so the level param resolves to `id 0` and the
device looks "wedged". Historically the only known cure was a Pi reboot.

## Root cause

There is **no host-liveness guard anywhere on the USB/FunctionFS data plane.** Every endpoint
I/O is an unbounded blocking syscall whose only exit is the host tearing the interface down
(`-ESHUTDOWN`): `device/ffs_link.c:25` (link read), `:44` (link write), `device/audio_loop.c:59`
(host-paced audio read). The eth/TCP guards (`SO_RCVTIMEO`, the §16 pre-hello deadline) are
socket-only and never apply to USB.

When a USB host dies **ungracefully** mid-stream (`kill -9`, a DAW crash, or the soak's
pause-`pkill`), macOS does not always deconfigure→reconfigure the gadget, so no FUNCTIONFS
DISABLE→ENABLE cycle fires. The device is then parked here (captured live, harp2 while wedged):

```
do_wait_intr_irq
ffs_ep0_read   [usb_f_fs]      <- blocked reading the ep0 control endpoint
vfs_read -> ksys_read -> __arm64_sys_read
```

i.e. in `harp_ffs_serve`'s outer loop (`device/ffs.c:198`) waiting for an ENABLE that never
comes. A new host claim succeeds at the USB level but, the gadget still being configured, macOS
issues no fresh `SET_CONFIGURATION`, so no ENABLE — no session forms and the hello hangs. With
`Restart=always` but **no `WatchdogSec`/`sd_notify`**, a daemon parked in a blocking read is
invisible to systemd, so it presents as "wedged until reboot".

## Bench experiments (the decisive part)

Reproduced deterministically on harp2 (PI4B-0003) over USB to a macOS host:

- **Reproduction.** `kill -9` of a USB render mid-stream wedges the gadget (5 consecutive probes
  hung ~50 s, no self-recovery). The wedge is **intermittent** — it bites on a cold/long-idle
  daemon and not on a warm one (a race in the macOS deconfigure path). `libusb_set_configuration(h, 0)`
  is a *reliable* inducer (deconfigure → "waiting for enable").
- **Host-side recovery is NOT possible on macOS (measured, both negative):**
  - `set_configuration(1→1)` is a no-op (darwin skips it when already at that config).
  - `set_configuration(0→1)` returns success but does **not** re-ENABLE the gadget — the device
    stays wedged.
  - `libusb_reset_device` returns success and produces a DISABLE on the gadget but **no ENABLE**
    — the device stays wedged. Notably it did **not** cause a D-state (the audit's fear was
    unfounded for this soft-wedge mode), but it also does not help.
  - **Only a device-side daemon restart** (`systemctl restart harp-deviced-usb` → re-bind the UDC
    → host re-enumerates → fresh ENABLE) recovers it.

Conclusion: once wedged, the host **cannot** recover the gadget on macOS. So the fix must
**prevent** the wedge for catchable deaths, and **auto-recover externally** for the rest.

## Fixes implemented here (validated on the Mac against real hardware)

1. **Host SIGTERM/SIGINT graceful close** (`tools/vst3-host/main.cpp`). A signal handler sets a
   stop flag; each render loop breaks into the normal teardown (`setActive(false)` → runtime
   release → `sessionDown` → `harp_usb_close` = release_interface + close). That clean close is
   what lets macOS deconfigure the gadget so the next claim re-ENABLEs it. **Validated:** old host
   `SIGTERM` → exit 143 (terminated, no teardown); new host `SIGTERM` mid-stream → exit 0, the
   gadget logs a clean `audio stream stopped`, and the next probe resolves — device left usable.
   Covers the soak's pause-`pkill` (its actual trigger) and a CLI Ctrl-C.
2. **harp-probe internal ctl timeout** (`host/harp-probe.c`). `harp_usb_set_ctl_timeout(io, 8000)`
   after open, so a wedged daemon makes the probe **return + release cleanly** instead of relying
   on an external `perl alarm` that SIGALRM-kills it mid-claim (an ungraceful exit that leaves a
   torn frame and can wedge the next claimant). The shell already did this; harp-probe did not.

## The residual + how it's covered

- **SIGKILL / crash** (uncatchable, no clean close): the host can't run a teardown and — proven
  above — can't recover the gadget either. This is handled by **external auto-recovery**: the soak
  harness (`scripts/harp-soak.sh`) detects a wedged USB target and `systemctl restart`s its daemon,
  keeping it under test (shipped separately). For the product, the same daemon-restart is the cure
  (a "reconnect" action, a systemd/device watchdog, or a replug).
- **A device-side watchdog cannot distinguish** a wedged "waiting for enable" from a legitimately
  idle one (both park in `ffs_ep0_read`; the gadget never sees the failed host claims) — so any
  device-side auto-recovery needs an out-of-band trigger, not a simple inactivity timer. Tracked as
  follow-up; the harness daemon-restart is the pragmatic answer today.
- **The hard kernel D-state mode** ("needs reboot" / "chronic mpe-wedge") was **not** reproduced
  here (all threads interruptible S-state); its true cause is still unidentified.

## Validation

- **Mac:** the experiments above + the fix-1 before/after, against real PI4B-0003 over USB.
- **HW rig / soak:** the overnight soak with these binaries is the long-running intermittent-wedge
  detector — zero wedges post-fix is the proof. CI (`hw.yml`) exercises the device build + the
  behavioral suite (USB-teardown regressions don't show on loopback/eth).
