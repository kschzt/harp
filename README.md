# HARP — Hardware Audio Runtime Protocol

[![ci](https://github.com/kschzt/harp/actions/workflows/ci.yml/badge.svg)](https://github.com/kschzt/harp/actions/workflows/ci.yml)
[![hw — real device](https://github.com/kschzt/harp/actions/workflows/hw.yml/badge.svg)](https://github.com/kschzt/harp/actions/workflows/hw.yml)
[![eth — IP transport](https://github.com/kschzt/harp/actions/workflows/eth.yml/badge.svg)](https://github.com/kschzt/harp/actions/workflows/eth.yml)
[![reaper — real DAW](https://github.com/kschzt/harp/actions/workflows/reaper-e2e.yml/badge.svg)](https://github.com/kschzt/harp/actions/workflows/reaper-e2e.yml)
[![perf — RT feed](https://github.com/kschzt/harp/actions/workflows/perf-gate.yml/badge.svg)](https://github.com/kschzt/harp/actions/workflows/perf-gate.yml)
[![license: Apache-2.0 | CC-BY-4.0](https://img.shields.io/badge/license-Apache--2.0%20%7C%20CC--BY--4.0-blue)](LICENSING.md)

**Make hardware instruments behave like plugins.** HARP is an open protocol — a
**Version 1.0** specification with a machine-readable CDDL schema — and a complete
reference implementation that proves it: a Raspberry Pi 4B plays as an instrument
track in Ableton Live today, with Git-style total recall through the project's own
save/reopen, knobs as sample-accurate automation lanes, and offline bounce *through
the physical box*. Use the reference device, or **build your own device or host on
the protocol**.

<!-- ▶ DEMO VIDEO GOES HERE — ~60s: Pi as a Live track → automate a knob →
     save & reopen the set → recall restores the hardware → offline bounce. -->

Hardware synths, drum machines, and effects have connected to computers for
forty years, and the connection is still fragile in one specific way:
*recall*. Audio mostly works, MIDI mostly works — but reopen last month's
project and the hardware is not in the state you saved, and nothing will
tell you. The deep integrations that fix this (multichannel USB audio,
plugin-shell control, total recall) have existed only inside closed
single-vendor ecosystems, because every vendor has to rebuild drivers, state
sync, and DAW compatibility from scratch.

HARP is that integration as an open standard plus a working reference. The spec
(§1–§19) is normatively complete and implementable — a vendor can build a conforming
device or host **from the spec alone**, against the same wire formats the CDDL pins.
The reference device is the proof: a Raspberry Pi running a 16-part multitimbral,
8-voice-polyphonic synth (12 automatable params, indexed per part). Anything that
speaks the protocol gets the same treatment from any conforming host:

- **Total recall, Git-style** — device state is content-addressed and
  hash-verified; a saved project reopens with the hardware *provably* in
  the saved state, every overwrite preceded by a free snapshot, every
  mismatch resolved through explicit safe actions — never silently. The
  archive doubles as patch time-travel: every state the box has ever been
  pushed over is one click away.
- **Audio as a plugin** — a dedicated stream into the plugin shell that
  bypasses the OS audio stack (no aggregate-device hacks), including a
  host-paced mode where the hardware renders *deterministically*, byte-
  identical, faster than real time: offline bounce through a physical box.
  Reported latency is **~21 ms at DAW buffers ≤ 256** (host-paced, the PDC the
  DAW compensates), and free-running live play goes **as low as ~9 ms** with a
  device-declared real-time profile — in the neighborhood of a good audio
  interface, and *measured*, never guessed (§14.3 loopback).
- **Multitimbral, addressed like plugins** — one physical device is one
  session, and several shell instances can share it: drop the plugin on a
  handful of DAW tracks and each instance drives its own *part* — its own
  channel, params, recall state, and stereo output (16 parts, with a summed
  main mix alongside the per-part outputs). A recall-safe **Part** knob persists
  each track's part in the project, and that per-part state moves intact between
  the VST3 and AU formats (the CLAP shell writes the same recall bundle).
- **Polyphonic, with per-voice modulation** — each part is an 8-voice pool with
  deterministic voice allocation, so overlapping notes ring out on their own
  voices instead of stealing one. Modulation is *non-destructive* and *per
  voice*: a VST3 Note Expression or a CLAP per-note parameter modulation bends a
  single sounding note's filter cutoff without touching the stored patch — and
  the two formats render byte-identically (the device applies the §9.5 mod the
  same regardless of which shell sent it).
- **Sample-accurate everything** — DAW automation becomes
  device-interpolated ramps applied within ±1 sample; notes travel as UMP;
  an event *fence* makes "applied late" structurally impossible rather
  than statistically rare. Turn a knob on the hardware and the DAW records
  the automation (echo).
- **Musical time** — devices follow the DAW's transport: tempo,
  position, loops. The reference device's arpeggiator locks to Live's grid
  sample-exactly, survives loop wraps, and renders a *byte-identical
  groove* — determinism extended to musical time.
- **Identity, timing, diagnostics** — engine versioning and parameter-map
  hashing protect old songs from new firmware (a breaking engine-major change
  reopens the project read-only; an incompatible protocol surfaces an update
  prompt, not a dead session), latency is measured (never guessed), and honest
  error counters at every layer end "it glitched" support threads with evidence.
- **Roadie-proof sessions** — unplug the cable mid-song and plug it back:
  the shell reconnects, re-asserts the project's state, and audio resumes.
  Hostile or corrupt wire input ends in a clean session reset, never a
  crash (every parser is fuzzed; a live abuse test is part of CI).

**What "works" means here:** the four protocol planes (control, state, events,
audio) are implemented end-to-end and verified on real hardware — recall
through Ableton's own save/reopen, a tempo-locked arpeggiator, sample-accurate
automation, offline bounce through the box, and a 16-part multitimbral,
8-voice-polyphonic instrument with per-voice modulation, driven by several
plugin instances over one shared session. The plugin shell builds and is
CI-validated on **macOS, Windows, and Linux** — a VST3 on all three (pluginval
strictness 10), an Audio Unit on macOS (`auval`) at full parity, and a CLAP on
all three — all three formats rendering byte-identically (the conformance kit
asserts it), and a project's per-part recall state moving between VST3 and AU
(CLAP writes the same recall bundle). The device has been driven from **Ableton
Live** (macOS + Windows) and **Renoise** (Windows) by hand, plus a headless
**REAPER** render-and-recall e2e on Linux that runs on every push.

**Where 1.0 stands.** The spec is **Version 1.0** (26 June 2026); a spec→code audit
cleared the implementation with **zero blockers**. What 1.0 *is*: a complete,
normative protocol with a CDDL schema and a working reference device + host that
prove it implementable end-to-end and byte-identically. What 1.0 *isn't, yet*: an
ecosystem of instruments (building richer synths and shipping real devices on the
protocol is the roadmap, not a claim about the present), and not yet a turn-key
*certification* kit — the test suite is strong functional verification, but a unified
T1–T17 conformance harness and the §13 firmware-management bar are still ahead. The
[Status](#status) section is the full breakdown, and [Building on HARP](#building-on-harp)
is the on-ramp for device and host implementers.

## Repository map

```
spec/             the specification (v1.0) + machine-readable CDDL (the
                  normative wire schema, Appendix A)
core/             portable C11 protocol library, no dependencies:
                  framing, deterministic CBOR, SHA-256, content-addressed
                  objects, crash-atomic ref store, audio frame codec
device/           harp-deviced — the reference device daemon: a 16-part
                  multitimbral synth engine (12 automatable params, indexed
                  per part) behind the protocol. Transports: TCP (simulation,
                  any OS) and FunctionFS USB gadget (Linux)
host/             harp-probe — host-side CLI: recall flows, audio capture,
                  offline render, determinism + conformance checks (libusb)
shell/            the plugin shells over one embedded runtime: VST3 ("HARP
                  RefDev"), Audio Unit (shell/au) and CLAP (shell/clap) — same
                  params, same Recall Bundle, same "Part" routing param, same
                  noteId->voice map (note_voice_map.h). The runtime registry lets
                  several instances naming one device SHARE its session (each owns
                  a part); the AU also joins the host's CoreAudio workgroup. All
                  three render BYTE-IDENTICAL audio and a project's recall state
                  moves between them (asserted by the conformance kit)
tools/vst3-host/  CLI VST3 host for automated testing of any plugin —
                  params, block processing, WAV+hash, state round-trips,
                  multi-instance shared-session driving
tools/au-host/    its Audio Unit twin (drives the AU shell; same hashes;
                  save/load-state for the cross-format recall e2e)
tools/clap-host/  its CLAP twin (dlopens the .clap; drives notes/params and
                  CLAP per-note PARAM_MOD; same golden hashes as the others)
tests/            unit tests for the core (RFC 8949 vectors included) + the
                  device codec and the safety-contract invariants
docs/             architecture one-pager; VST3, multitimbral, and Ethernet-
                  transport (§4.4) design notes
scripts/          the conformance suites (eth-suite.sh is the single source of
                  truth across OSes) + Raspberry Pi provisioning runbook
external/         VST3 SDK + CLAP SDK clones (gitignored; shell/tools only)
```

## Getting started

### Path 1 — laptop only, five minutes (no hardware)

```sh
cmake -B build && cmake --build build
./build/harp-tests                                # unit tests
./build/harp-deviced --state-dir /tmp/refdev &    # simulated "hardware"
./build/harp-probe demo                           # narrated total-recall walkthrough
```

The `demo` walks the canonical project-open flow (spec §12.2): save,
front-panel edits dirtying state, mismatch detection on reopen,
archive-before-push, hash-verified restore.

**See it yourself:** that walkthrough reproduces the total-recall claim on
your laptop, no hardware. The bolder claims — byte-identical renders and
±1-sample timing — need a device: `harp-probe -d usb t15 4` renders twice and
byte-compares (Path 2 below), and those same checks run on a real Pi in CI
(`hw.yml`, on every push whenever the self-hosted rig is online).

For the visual version, start the web panel (the device's "front panel"
plus a live protocol inspector — refs, generations, the dirty flag in
real time):

```sh
python3 web/harp-panel.py /tmp/harp-panel.sock 8080 &
open http://localhost:8080      # drag sliders, watch the state model react
```

### Path 2 — a real device (Raspberry Pi 4B)

`scripts/pi-bringup.md` is the operations runbook: provision a Pi as a USB
gadget (vendor interface + dedicated audio endpoints), connect it to your
computer over USB-C, and the same `harp-probe` commands run against real
hardware — plus audio:

```sh
./build/harp-probe -d usb demo                 # recall, over the wire
./build/harp-probe -d usb record 4 take.wav    # free-running capture, MSC-verified
./build/harp-probe -d usb render 8 bounce.wav  # host-paced offline bounce (deterministic, faster than real time)
./build/harp-probe -d usb t15 4                # determinism: render twice, byte-compare
```

### Path 3 — the DAW (VST3, AU, or CLAP)

Builds on macOS, Windows, and Linux. Requires CMake ≥ 3.16 (3.20 for the
plugin tools), libusb (macOS: `brew install libusb`; Linux:
`apt install libusb-1.0-0-dev` plus
`sudo cp scripts/99-harp.rules /etc/udev/rules.d/` for rootless device
access; Windows: prebuilt MSVC binaries under `external/libusb-win`, built
with the Visual Studio 2022 generator), and the VST3 + CLAP SDKs cloned under
`external/`:

```sh
git clone --recursive https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk
git clone --branch 1.2.8 https://github.com/free-audio/clap.git external/clap
```

On **Windows** also fetch the prebuilt libusb into `external/libusb-win` — there is no
pkg-config, so the shell looks for it there; without it the configure aborts with
the cause (previously the shell targets silently weren't generated and a later
`--target install-live` failed with "project file does not exist"):

```powershell
$ver = '1.0.27'
Invoke-WebRequest -UseBasicParsing "https://github.com/libusb/libusb/releases/download/v$ver/libusb-$ver.7z" -OutFile libusb.7z
New-Item -ItemType Directory -Force external/libusb-win | Out-Null
7z x libusb.7z -oexternal/libusb-win -y   # needs 7-Zip; the bundled tar lacks the LZMA codec
```

```sh
cmake -B build-vst -S tools/vst3-host   # Windows: add -G "Visual Studio 17 2022" -A x64 if the default generator fails
cmake --build build-vst --target install-live   # macOS/Windows: install to the OS VST3 folder (+sign on macOS)
cmake --build build-vst --target install-linux  # Linux: install to ~/.vst3
cmake --build build-vst --target install-au     # macOS: the Audio Unit shell
cmake --build build-vst --target install-clap   # CLAP: ~/Library/Audio/Plug-Ins/CLAP (mac, bundle+signed) | %CommonProgramFiles%\CLAP (win) | ~/.clap (linux)
```

(The AU appears as **HARP Project: HARP RefDev**; `auval -v aumu rfdv
HARP` is Apple's own validation and runs in this repo's CI. All three
shells render byte-identically from the same drive — the conformance kit
asserts it.)

Rescan plug-ins in your DAW and drop **HARP RefDev** on a track. The
plugin claims the device over USB, streams host-paced audio into the
DAW's engine, exposes the knobs as automatable parameters, and stores a
Recall Bundle in the project file — reopening the project restores the
hardware (archiving whatever was on it first).

The shell can also be driven without any DAW, which is how it is tested:

```sh
./build-vst/harp-vst3-host ~/Library/Audio/Plug-Ins/VST3/harp-shell.vst3 \
    --set 3=0.8 --seconds 2 --out take.wav --hash
```

### Path 4 — a DAW with no hardware (the simulated device over §8.7 Ethernet)

Path 3 claims a real device over USB. To drive a DAW against the **simulated**
device instead — no hardware, the daemon and the shell talking over the §8.7
Ethernet/IP binding on loopback — run the device on a TCP port and point the shell
at it with an env var:

```sh
./build/harp-deviced --port 47987 --serial SIM-0001 --state-dir /tmp/refdev
```

On the network path a device declares its safe real-time setpoints in its identity
(`rt-profile`, capability `audio.rt-floor`) and the shell adopts them in place of the
conservative default jitter buffer / RTP packet size: **`--rt-floor N`** (buffer
floor) and **`--rt-nsamples N`** (RTP packet size). Each device supplies its own measured
values through its on-device service config (the systemd unit) — a board over a direct
1 Gbps cable runs `--rt-floor 320 --rt-nsamples 64`, a Pi over a switch `--rt-floor 448
--rt-nsamples 128`; with no flag a device declares nothing and the host keeps a safe
1024-frame / 256-sample default. The host clamps whatever is declared (floor to
`[2·max-DAW-block, 12288]`, packet to `[32, kBlock]`). A declared profile brings
free-running live latency down from ~32 ms (undeclared default, block 256) toward
**~9 ms** (320/64 at block 64). See [`scripts/eth-rtfloor-test.sh`](scripts/eth-rtfloor-test.sh).

Synths on a network advertise over mDNS (`_harp._tcp`); **`harp-probe discover`** browses
the segment and resolves each to its `host:port`. The shell discovers them too: with no
USB device and no `HARP_ETH_DEVICE`, `selectDevice` browses `_harp._tcp` and dials the
first synth it finds (§6.1). Set **`HARP_ETH_DEVICE=mdns`** to force that browse explicitly,
`HARP_ETH_DEVICE=HOST:PORT` to pin a literal address, or `HARP_NO_MDNS=1` to disable the
auto-browse. A GUI DAW does **not** inherit your terminal's environment, so set it where
the OS launcher sees it, *before* launching the DAW:

```sh
launchctl setenv HARP_ETH_DEVICE 127.0.0.1:47987    # macOS; Linux/Windows: set it in the env the DAW inherits
```

Then launch the DAW, rescan plug-ins if needed, and drop **harp-shell** on a MIDI
track — you'll see `connected: …` print in the device's terminal. Quit and relaunch
the DAW if it was already open (the env is read at the first dial); `launchctl
unsetenv HARP_ETH_DEVICE` returns to real USB hardware. The device may come up before
or after the DAW — the shell's supervisor reconnects either way.

## Building on HARP

HARP is a protocol first. The spec is the product; the reference implementation is the
proof; the kit is honest about what's turn-key and what isn't. If you want to build a
device, a host, or just verify a claim, start here.

**Build a device.** The spec (§4–§16) is normatively complete and implementable from
the document alone — transport (USB §4.3 or Ethernet/IP §4.4, both normative and proven),
the control envelope (§5), identity + capabilities (§6), time/clocking (§7–§8), events
(§9), the state model (§10–§11), session lifecycle (§12), and security (§16). The wire
formats are pinned in [`spec/harp.cddl`](spec/harp.cddl). The reference device
(`device/`, ~3k lines of C11) is the worked example — `session.c` (the protocol state
machine), `state.c` (content-addressed, crash-atomic storage), `engine.c` (the synth
DSP), `device.h` (the module contracts). *Not yet provided:* a step-by-step device-
implementer's guide or marked extension points — read the spec and the reference; and
`scripts/pi-bringup.md` is the reference Pi's **operations** runbook (provision/deploy/
debug), not an implementer tutorial. §13 firmware management is specified but not yet
implemented on the reference device.

**Build a host.** §15 defines the host runtime + shell requirements; the reference
runtime (`shell/runtime.cpp` / `.h`) — embedded in the VST3/AU/CLAP shells — is working
proof of DAW integration: discovery, sessions, the event merge, per-part demux, and
recall. *Not yet provided:* the runtime is not packaged as a standalone library (it is
embedded per plugin instance); the per-machine daemon (§15.1) that would cleanly separate
transport and sessions is on the roadmap.

**Verify conformance.** The conformance classes (§2.3, §17 — harp-core, harp-recall,
harp-stream, harp-class-audio, harp-perf, harp-fw) tell you exactly what a given claim
requires, and the test suite is strong functional verification: `scripts/eth-suite.sh`
is the single source of truth across three OSes, the four "no silent state loss" safety
contracts (CAS conflict §11.3, archive-before-push §11.4, param-map-hash §9.3, the event
fence §8.3.1) each have a regression-catching test, and the CDDL is machine-readable.
*Not yet provided:* a unified harness that runs the §17 T1–T17 battery by number and emits
a certification report — today the cert behaviors are spread across `hw-tests-linux.sh`
and `eth-suite.sh`, and the quantitative thresholds (±1 ms, ±1 sample, 24 h soak, ≥4
device / ≥64 channel, ±200 ppm) are documented in §17 but not yet enforced at full
certification scale. That harness — the path from *reference implementation* to
*certification kit* — is the headline post-1.0 project.

## Documentation

- **The specification**: [`spec/harp-spec-1.0.md`](spec/harp-spec-1.0.md).
  First read: §1 (motivation and design principles), §2 (conformance + terminology),
  §10–§11 (the state model — "Git, not SysEx"), §8 (the audio plane and clock domains),
  §17 (conformance classes). The changelog at the top records what implementation taught us.
- **Machine-readable schema**: [`spec/harp.cddl`](spec/harp.cddl) — the normative
  wire format (Appendix A), the contract a conforming implementation is checked against.
- **Architecture one-pager**: [`docs/architecture.md`](docs/architecture.md)
  — components, planes, and the shell's threading model.
- **VST3 shell design plan**: [`docs/vst3-shell-plan.md`](docs/vst3-shell-plan.md)
  — decisions, DAW-compatibility lore, and what's deliberately deferred.
- **Multitimbral plan + test matrix**: [`docs/multitimbral-plan.md`](docs/multitimbral-plan.md)
  — the per-part build (device parts → per-part params → session sharing →
  event merge → per-part audio → Part param), what each phase verified, and
  the deferred items.
- **Ethernet/IP transport design**: [`docs/ethernet-transport-plan.md`](docs/ethernet-transport-plan.md)
  — the §4.4 network binding (RTP/UDP for audio, the framed link over TCP for
  control, trusted-segment security, an AES67 bridge reserved). Now folded into
  the spec (§4.4, §7.3, §8.7) and CI-gated (`eth.yml`, three OSes): the framed
  control link, the RTP/UDP audio plane, the host-locked rate-trim loop + ASRC,
  and host-paced offline bounce. PTP-grade sync stays a hardware prototype
  (~22 µs idle), not yet wired into the runtime.
- **Pi runbook**: [`scripts/pi-bringup.md`](scripts/pi-bringup.md) —
  provisioning, the sudo-free deploy loop, USB debugging tricks.

## Status

**Working — verified end-to-end on real hardware:**

- **Protocol core** — `harp-core` (framing, deterministic CBOR, SHA-256,
  content-addressed object store), `harp-recall` (save / snapshot / restore),
  `harp-stream` (free-running and host-paced; `audio.deterministic` +
  `audio.offline-rate`), and two transport bindings: USB (FunctionFS gadget) and
  the §8.7 Ethernet/IP binding (framed control over TCP + an RTP/UDP audio plane).
- **The plugin shell** — VST3 on macOS/Windows/Linux, an Audio Unit on macOS,
  and a CLAP on macOS/Windows/Linux (all three byte-identical), driving the device in
  Ableton Live (macOS + Windows), Renoise (Windows), and an automated headless
  REAPER render (Linux).
- **Total recall, safety contracts tested** — content-addressed, hash-verified
  state with compare-and-swap conflict detection (§11.3), archive-before-push so
  every overwrite is preceded by a recoverable snapshot (§11.4), param-map-hash
  change-detection that protects stored automation (§9.3), and the four-safe-actions
  reconcile UI (Push / Pull / Read-only / Duplicate) in the Electra front-panel
  sidecar. Each of these contracts has a regression-catching test in CI.
- **Polyphony + per-voice modulation (§9.5)** — each part is an 8-voice pool
  with deterministic allocation (overlapping notes ring out, never steal one
  voice); a VST3 Note Expression or a CLAP per-note PARAM_MOD bends one voice's
  filter cutoff non-destructively, byte-identically across the two formats. The
  device's golden render is unchanged when no modulation is sent.
- **MPE input (§9.5/§9.10), all three shells** — an MPE controller's three
  per-note axes drive per-voice modulation: pitch-bend (semitones), timbre
  (CC74/brightness → filter cutoff), and pressure (→ loudness). They arrive as
  CLAP note expressions, VST3 Note Expression (Cubase + the per-note UI), or —
  for the DAWs that send MPE as **raw 16-channel MIDI** (a member-channel-per-note
  stream): Logic and Ableton Live into the AU, and Ableton Live into the VST3 (via
  `IMidiMapping`, engaged by the plugin's "MPE" toggle, which persists with the
  project). `shell/mpe_zone.h` collapses the zone onto one device part — a member
  channel must never become the multitimbral part (§9.4). However it arrives, the
  shell maps each axis to the SAME §9.5 per-voice mod, so a chord bends every note
  independently and the *same gesture renders byte-identically across all three
  shells* (a neutral MPE chord across member channels == the plain chord; raw bend
  −12 == VST3 Note-Expression == CLAP note-expression bend −12).
- **Output metering (§9.9)** — per-part and main-mix peak/RMS, exposed as
  readonly params streamed via echo and read back through `harp-probe meters`;
  folded read-after-write in the render, so the golden render is unperturbed.
- **The event plane (§9)** — parameter sets and ramps at exact sample
  timestamps (±1 sample on hardware), DAW automation synthesized into
  device-interpolated ramps, UMP note input (the refdev is a playable
  envelope+portamento synth), and front-panel echo (turn a knob on the device,
  the DAW records the automation). Note-offs are unloseable; the DAW's panic
  (CC 120/123) reaches the hardware. An event *fence* (§8.3.1) makes "applied
  late" structural rather than statistical, and has a test that fails if the
  fence regresses.
- **Musical time (§9.7)** — the device follows `evt.transport`: a note-latch
  arpeggiator whose step clock derives from the (timestamp, PPQ, tempo) anchor
  lands on division boundaries sample-exactly, survives loop wraps, and renders
  a byte-identical *groove hash*. (The automatable param map is 12 ids, shared
  across parts; old projects map onto matching ids with a warning, §9.3.)
- **Multitimbral (§9.4, §15.1–§15.2)** — one device is one session, and several
  shell instances that name the same unit *share* it, each owning a part
  (channel): its own params, its own recall, its own demuxed stereo output (16
  parts; a summed main mix alongside the per-part pairs). A recall-safe **Part**
  parameter persists each instance's part in the project, and per-part state
  moves between the VST3 and AU formats (CLAP writes the same recall bundle).
  Verified on hardware: channel→part routing is exclusive (no bleed), per-part
  timbres restore losslessly across save/reopen, sibling parts engage in the
  summed mix, a part added mid-session re-negotiates its own audio, and a
  per-part param reaches only its own part.
- **Multi-device** — two *separate* boards on one bus (distinct from
  multitimbral above): each instance binds its own by USB identity (saved
  serial, else first unclaimed same-model — never a different synth), reconnect
  pins that exact unit, two instances claim two devices by contention.
- **Reported latency** — **~21 ms** at DAW buffers ≤ 256 (host-paced, the PDC the
  DAW compensates; §6.4), built from the ring buffer + event headroom + the device
  render/turnaround block measured by the §14.3 loopback (never guessed). Free-running
  live play reaches **~9 ms** with a device-declared rt-profile (320/64 at block 64).
- **The web panel** — front panel plus a live protocol inspector: refs with
  ticking generations, the dirty flag, counters, and Snapshot / Panic /
  one-click Load of any archived state (patch time-travel — the current state
  is archived first, so nothing is ever lost).

**Gated in CI on every push:**

- **Ethernet/IP conformance suite** against the simulated device on three OSes
  (`scripts/eth-suite.sh`, the `eth` badge): the framed control link + RTP/UDP
  audio plane with no hardware — bit-exact live play with a host-locked rate-trim
  loop, multichannel ASRC, deterministic host-paced offline bounce, mid-session
  reconnect, RTP packet-loss tolerance, hostile-frame fault injection, the
  spec-conformance closures (credit flow-control §4.2.1, event transactions §9.6,
  admission control §8.4, engine-major read-only §12.2, the §14.4 diag bundle and
  §14.3 loopback, mDNS discovery §4.4.3 on macOS), and the four safety-contract
  tests (CAS conflict, archive-before-push, param-map-hash, event fence).
- **Hardware suite** on a real Pi (`scripts/hw-tests-linux.sh`, the `hw`
  badge): 21 sub-tests — 17 pass / 4 skip on the one-board, no-AU CI rig (the
  two-device and VST3↔AU-recall tests self-skip, and two multi-instance USB
  tests run on the eth rig instead). It gates the **golden
  oracle across VST3 + AU + CLAP** (byte-identical), **8-voice polyphony with
  deterministic voice-stealing**, **per-voice modulation** (VST3 Note Expression,
  CLAP per-note PARAM_MOD, and **MPE** pitch/timbre/pressure), **§9.9 output
  metering**, ±1-sample note timing with zero late events, a realtime soak
  flooding automation + notes + panel traffic at the DAW block (zero silence
  gaps, zero drops, bounded padding; a separate `soak-matrix.sh` sweeps buffers
  64–1024 as a release-grade check), an automated mid-stream replug, an
  IDM-flood determinism gate, the REAPER real-DAW determinism + recall
  round-trip, and the **multitimbral matrix** — channel→part routing, per-part
  recall, session sharing, an alias group playing, per-part audio demux (incl. a
  mid-session re-negotiation), per-part param isolation, and cross-format VST3↔AU
  recall (macOS) — with the daemon under test built on the board from the commit
  being tested. The runtime threads are also ThreadSanitizer-clean (incl. the
  multi-source event merge + demux) on the rig.
- **Sandboxed suite**: builds + unit tests on three OSes, pluginval
  strictness 10 (macOS / Windows / Linux), `auval` (macOS), fuzzed parsers
  (libFuzzer + ASan), and a protocol-abuse test that slams a live daemon with
  hostile traffic (sessions reset, nothing crashes).

**Certification status.** The §17 conformance battery (T1–T17) is normatively
specified; a portion runs today in the suites above — **T2** (unplug/replug,
`replug-test`), **T5** (silent hash-verified reopen, `recall-test`), **T9**
(malformed input, corrupt-CBOR + abuse), **T15** (byte-identical on-device renders,
`golden-test`), **T16** (event timing, `timing-test`), **T17** (musical-time tempo
lock, `tempo-lock-test`). **T3** (sleep/wake) and **T10** (power-loss on real hardware)
are not yet deployed, and the numeric thresholds in §17 are not yet enforced at full
certification scale. A unified harness that runs all of T1–T17 by number and emits a
report — together with §13 firmware management — is the remaining bar to a
*certification claim* (distinct from the functional completeness above).

**Not yet:** runtime/shell process split (§15.1 — currently embedded in-process, a
daemon refactor on the roadmap); firmware management (§13 — the structural bar for
certification); class-audio coexistence (§8.5 — specified, hardware-pending); the
free-running ASRC analog-device proof (the loop is implemented; the analog converter
HAT is pending); PTP-grade Ethernet clock sync wired into the runtime (the §8.7 binding
ships with a rate-trim loop + ASRC; PTP stays a hardware prototype); and the unified
T1–T17 certification harness. The §5.5 `core.changed` / `core.bye` / `core.identify` /
`core.ping` handlers ship; only the shell's auto-`bye`-on-disconnect is deferred (it
regressed the USB teardown — pending a USB-safe fix).

Breaking protocol changes are still negotiated at `core.hello` (§5.4) and flow through
HARP Enhancement Proposals (§18); two interoperating implementations are required before
a HEP merges, once the process formalizes.

## Licensing

- Specification text: **CC BY 4.0**
- Schemas, reference implementation, test suite: **Apache-2.0**
- Patent posture: royalty-free; contributors sign a non-assertion
  covenant (spec §19). Use of the protocol is unrestricted; the
  certification *mark* (future) requires passing the test suite.
