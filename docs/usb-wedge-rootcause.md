# USB FunctionFS gadget "wedge" — root cause, evidence, fix plan

**Status (2026-06-28):** root-caused with a live kernel stack + a deterministic (if
state-dependent) bench reproduction. The transient/harness mitigation is shipped in
`scripts/harp-soak.sh`. The protocol-level *never-wedge* fix is designed below and needs
a branch + HW-CI-rig validation (USB-teardown regressions do not show on loopback/eth).

## Symptom

Under the soak, a USB device (harp / harp2 = PI4B-0001 / PI4B-0003) intermittently stops
responding: the host `harp-probe` claims it at the USB level but the protocol hello/params
never complete, so the level param resolves to `id 0` and the harness labels it
"wedged/flaky USB". Historically the only known cure was a Pi reboot.

## Root cause (confirmed)

There is **no host-liveness guard anywhere on the USB/FunctionFS data plane.** Every endpoint
I/O is an unbounded blocking syscall whose only exit is the host tearing the interface down
(`-ESHUTDOWN`):

- `device/ffs_link.c:25` — `read(ep_out)` (link/ctl), no timeout
- `device/ffs_link.c:44` — `write(ep_in)` (link), no timeout
- `device/audio_loop.c:59` — `read(ep4)` (host-paced audio), no timeout

The eth/TCP guards (`SO_RCVTIMEO`, the §16 10 s pre-hello deadline) are socket-only and never
apply to USB (`harp-deviced.c` sets `ctl_sock = INVALID` for USB).

When a USB host process dies **ungracefully** mid-stream (`kill -9`, a DAW crash, or the soak
heartbeat's `pkill`), macOS does not always deconfigure→reconfigure the gadget, so **no
FUNCTIONFS DISABLE→ENABLE cycle fires.** The device is then parked here (live capture, harp2,
while wedged):

```
do_wait_intr_irq
ffs_ep0_read   [usb_f_fs]      <- blocked reading the ep0 control endpoint
vfs_read -> ksys_read -> __arm64_sys_read
```

i.e. waiting in `harp_ffs_serve`'s outer loop (`device/ffs.c:198`) for an ENABLE event that
never comes. A new host claim succeeds at the USB level but, because the gadget is still
configured, macOS issues no fresh `SET_CONFIGURATION`, so no ENABLE — the device never opens
a session and the probe hangs waiting for a hello response. **It does not self-recover**
(observed: 5 consecutive probes hung ~50 s). The only cure short of a reboot is a **device-side
daemon restart** (`systemctl restart harp-deviced-usb` → re-binds the UDC → host re-enumerates
→ fresh ENABLE), confirmed to recover instantly.

With `Restart=always` but **no `WatchdogSec`/`sd_notify`**, a daemon that is *alive but parked*
in a blocking read is invisible to systemd — nothing restarts it automatically, so it presents
as "wedged until I reboot".

### Why it's intermittent

The wedge only manifests when macOS leaves the gadget configured after the ungraceful death
(no DISABLE). On a freshly-restarted/warm daemon the same `kill -9` + reclaim recovers on its
own; after a long-idle or ungracefully-ended prior session it sticks. It is a race in the
host-OS deconfigure path, which is why it reads as "flaky USB".

## Reproduction (bench)

```
# 1. start a USB render, SIGKILL it mid-stream
HARP_DEVICE_SERIAL=PI4B-0003 ./build-vst/harp-vst3-host <bundle> --notes 50,53,57 \
    --seconds 25 --realtime --out /tmp/w.wav & HP=$!; sleep 4; kill -9 $HP
# 2. probe repeatedly — when wedged, every probe claims but hangs (alarm fires), id 0
for i in 1 2 3 4 5; do perl -e 'alarm 10; exec @ARGV' ./build/harp-probe -d usb:PI4B-0003 params; done
# 3. inspect the device (ssh): main thread parked in ffs_ep0_read, journal stuck at "waiting for enable"
# 4. recover (NOT a reboot): ssh jak@harp2.local 'sudo systemctl restart harp-deviced-usb'
```

## Fixes

### Shipped — harness mitigation (`scripts/harp-soak.sh`)

Auto-recover a wedged USB target via `systemctl restart harp-deviced-usb` (per-target recover
ssh host), at startup and every 3 rounds, then re-probe and log `RECOVERED` — so USB stays under
test and the wedge surfaces as a finding instead of silently sidelining the device for ~10 min.
Validated against the real wedge.

### Designed — the never-wedge protocol fix (branch + HW-CI rig)

Ordered by value / safety. **Do NOT** use `libusb_reset_device` or aggressive UDC cycling — the
audit's strongest warning is that a wrong host-side recovery converts this recoverable soft
wedge into the uninterruptible **kernel D-state** reboot-class mode.

1. **Host SIGTERM/SIGINT graceful-close handler** (`tools/vst3-host/main.cpp`, `host/harp-probe.c`)
   — self-pipe → the existing clean `sessionDown` (audio.stop + drain + `libusb_release_interface`
   + close), bounded by a ~500 ms alarm fallback. Makes the heartbeat's default `pkill` (SIGTERM)
   take the clean path that yields the device-side `-ESHUTDOWN`. Covers SIGTERM, not SIGKILL.
2. **Host re-enable-on-claim** (`host/usb_io.c`) — if the hello times out on a fresh open, force a
   `SET_CONFIGURATION` (config 0→1, *not* `reset_device`) to drive a DISABLE→ENABLE so the device
   opens a fresh session — self-heals from *any* prior ungraceful death incl. SIGKILL/crash. Needs
   careful HW validation (this is the step with D-state-conversion risk if done wrong).
3. **`harp-probe` internal ctl timeout** (`host/harp-probe.c:2142`) — call
   `harp_usb_set_ctl_timeout(io, 8000)` after open so the hello is bounded internally and the probe
   returns + releases cleanly instead of being SIGKILLed by the harness `alarm 8` mid-claim (which
   leaves another torn frame). Low risk.
4. **Host `clear_halt` on open** (`host/usb_io.c:592`) — `libusb_clear_halt` the link+audio
   endpoints after claim (flush gadget FIFO + reset toggle, no bus reset) + discard any stale
   device→host bytes before the first hello read. Low risk; pairs with the device framer reset.
5. **Device session-liveness watchdog** (dedicated timer thread, NOT the ep0 thread) — on
   *mid-frame* inactivity (after a header, before payload completes — must NOT fire on a legitimately
   idle session) set an abort flag + `pthread_kill(SIGUSR1)` the parked thread so the interruptible
   FFS read returns `-EINTR`; the loop then re-runs `harp_link_init` on the still-enabled endpoints.
   Requires a host-side periodic keepalive to avoid the idle-session regression. Cannot wake a true
   D-state thread — so it is necessary but not sufficient for the hard mode.
6. **systemd `Type=notify` + `WatchdogSec`** (`scripts/pi-stage2-install.sh`) — `sd_notify(WATCHDOG=1)`
   pumped from the always-alive panel/watchdog thread (NOT the ep0/session thread, which legitimately
   blocks for a whole stream); stops pumping only on an unrecoverable wedge → systemd restarts as a
   last resort. Defense-in-depth so no human/reboot is ever needed.

### Still open

- The historical **hard kernel D-state** ("needs reboot", "chronic mpe-wedge") was **not** reproduced
  here (all threads interruptible S-state). Its true cause is unidentified — the audit's
  UDC-unbind-races-dwc2-dequeue theory was adversarially *refuted* for the observed incident. Needs a
  separate reproduction (tight `systemctl stop`-during-stream loop on the Linux HW rig, watching for a
  `D`-state thread) to capture its stack.
