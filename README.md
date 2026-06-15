# HARP — Hardware Audio Runtime Protocol

[![ci](https://github.com/kschzt/harp/actions/workflows/ci.yml/badge.svg)](https://github.com/kschzt/harp/actions/workflows/ci.yml)
[![hw — real device](https://github.com/kschzt/harp/actions/workflows/hw.yml/badge.svg)](https://github.com/kschzt/harp/actions/workflows/hw.yml)
[![license: Apache-2.0 | CC-BY-4.0](https://img.shields.io/badge/license-Apache--2.0%20%7C%20CC--BY--4.0-blue)](LICENSE.md)

**Make hardware instruments behave like plugins.** A Raspberry Pi 4B plays as
an instrument track in Ableton Live today — Git-style total recall through the
project's own save/reopen, knobs as sample-accurate automation lanes, and
offline bounce *through the physical box*.

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

HARP is a complete, working implementation of that integration — with an open
spec underneath, if you want to build a device on it. The reference device is
a Raspberry Pi running an 8-knob stereo synth; anything that speaks the
protocol gets the same treatment from any conforming host:

- **Total recall, Git-style** — device state is content-addressed and
  hash-verified; a saved project reopens with the hardware *provably* in
  the saved state, every overwrite preceded by a free snapshot, every
  mismatch resolved through explicit safe actions — never silently. The
  archive doubles as patch time-travel: every state the box has ever been
  pushed over is one click away.
- **Audio as a plugin** — a dedicated stream into the plugin shell that
  bypasses the OS audio stack (no aggregate-device hacks), including a
  host-paced mode where the hardware renders *deterministically*, byte-
  identical, faster than real time: offline bounce through a physical box
  at ~25× real time, with **16 ms of reported latency at DAW buffers ≤ 256**
  — in the neighborhood of a good audio interface.
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
  hashing protect old songs from new firmware, latency is measured (never
  guessed), and error counters at every layer end "it glitched" support
  threads with evidence.
- **Roadie-proof sessions** — unplug the cable mid-song and plug it back:
  the shell reconnects, re-asserts the project's state, and audio resumes.
  Hostile or corrupt wire input ends in a clean session reset, never a
  crash (every parser is fuzzed; a live abuse test is part of CI).

**What "works" means here:** the four protocol planes (control, state, events,
audio) are implemented end-to-end and verified on real hardware — recall
through Ableton's own save/reopen, a tempo-locked arpeggiator, sample-accurate
automation, offline bounce through the box. The plugin shell builds and is
CI-validated on **macOS, Windows, and Linux** — a VST3 on all three (pluginval
strictness 10) plus an Audio Unit on macOS (`auval`) — and the device has been
driven from **Ableton Live** (macOS + Windows) and **Renoise** (Windows) by
hand, plus a headless **REAPER** render-and-recall e2e on Linux that runs on
every push.

**What it is, and isn't, yet:** today HARP is a complete recall + audio system
with one reference device — a Raspberry Pi synth. It is *not* yet an ecosystem
of instruments; building richer synths and shipping real devices on top of the
protocol is the roadmap, not a claim about the present. The spec is an
**editor's draft** (0.3.6): breaking changes are expected and negotiated at
`hello`. The four-actions recall UI, a CLAP port, and the Ethernet binding are
next — the [Status](#status) section is the full breakdown.

## Repository map

```
spec/             the specification (draft 0.3.6) + machine-readable CDDL
core/             portable C11 protocol library, no dependencies:
                  framing, deterministic CBOR, SHA-256, content-addressed
                  objects, crash-atomic ref store, audio frame codec
device/           harp-deviced — the reference device daemon: an 8-knob
                  stereo synth engine behind the protocol. Transports:
                  TCP (simulation, any OS) and FunctionFS USB gadget (Linux)
host/             harp-probe — host-side CLI: recall flows, audio capture,
                  offline render, determinism checks (libusb)
shell/            the plugin shells over one embedded runtime:
                  VST3 ("HARP RefDev") and Audio Unit (shell/au) — same
                  params, same Recall Bundle in the project file; the AU
                  also joins the host's CoreAudio workgroup. Both render
                  BYTE-IDENTICAL audio from the same drive (asserted by
                  the conformance kit)
tools/vst3-host/  CLI VST3 host for automated testing of any plugin —
                  params, block processing, WAV+hash, state round-trips
tools/au-host/    its Audio Unit twin (drives the AU shell; same hashes)
tests/            unit tests for the core (RFC 8949 vectors included)
docs/             architecture one-pager, VST3 shell design plan
scripts/          Raspberry Pi provisioning + operations runbook
external/         VST3 SDK clone (gitignored; needed for shell/tools only)
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
byte-compares (Path 2 below), and those same checks run on a real Pi in CI on
every push (the green **hw** badge above).

For the visual version, start the web panel (the device's "front panel"
plus a live protocol inspector — refs, generations, the dirty flag in
real time):

```sh
python3 web/harp-panel.py /tmp/harp-panel.sock 8080 &
open http://localhost:8080      # drag sliders, watch the state model react
```

### Path 2 — a real device (Raspberry Pi 4B)

`scripts/pi-bringup.md` is the runbook: provision a Pi as a USB gadget
(vendor interface + dedicated audio endpoints), connect it to your
computer over USB-C, and the same `harp-probe` commands run against real
hardware — plus audio:

```sh
./build/harp-probe -d usb demo                 # recall, over the wire
./build/harp-probe -d usb record 4 take.wav    # free-running capture, MSC-verified
./build/harp-probe -d usb render 8 bounce.wav  # host-paced offline bounce (~25x realtime)
./build/harp-probe -d usb t15 4                # determinism: render twice, byte-compare
```

### Path 3 — the DAW (VST3 shell)

Builds on macOS, Windows, and Linux. Requires CMake ≥ 3.25, libusb (macOS:
`brew install libusb`; Linux: `apt install libusb-1.0-0-dev` plus
`sudo cp scripts/99-harp.rules /etc/udev/rules.d/` for rootless device
access; Windows: prebuilt MSVC binaries under `external/libusb-win`, built
with the Visual Studio 2022 generator), and the VST3 SDK
cloned to `external/vst3sdk` (`git clone --recursive
https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk`).

```sh
cmake -B build-vst -S tools/vst3-host
cmake --build build-vst --target install-live   # macOS/Windows: install to the OS VST3 folder (+sign on macOS)
cmake --build build-vst --target install-linux  # Linux: install to ~/.vst3
cmake --build build-vst --target install-au     # macOS: the Audio Unit shell
```

(The AU appears as **HARP Project: HARP RefDev**; `auval -v aumu rfdv
HARP` is Apple's own validation and runs in this repo's CI. Both shells
render byte-identically from the same drive — the conformance kit
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

## Documentation

- **The specification**: [`spec/harp-spec-draft-0.3.md`](spec/harp-spec-draft-0.3.md).
  First read: §1 (motivation and design principles), §10–§11 (the state
  model — "Git, not SysEx"), §8 (the audio plane and clock domains).
  The changelog at the top records what implementation taught us.
- **Architecture one-pager**: [`docs/architecture.md`](docs/architecture.md)
  — components, planes, and the shell's threading model.
- **VST3 shell design plan**: [`docs/vst3-shell-plan.md`](docs/vst3-shell-plan.md)
  — decisions, DAW-compatibility lore, and what's deliberately deferred.
- **Pi runbook**: [`scripts/pi-bringup.md`](scripts/pi-bringup.md) —
  provisioning, the sudo-free deploy loop, USB debugging tricks.

## Status

**Working — verified end-to-end on real hardware:**

- **Protocol core** — `harp-core` (framing, deterministic CBOR, SHA-256,
  content-addressed object store), `harp-recall` (save / snapshot / restore),
  `harp-stream` (free-running and host-paced; `audio.deterministic` +
  `audio.offline-rate`), and the USB binding.
- **The plugin shell** — VST3 on macOS/Windows/Linux and an Audio Unit on
  macOS, driving the device in Ableton Live (macOS + Windows), Renoise
  (Windows), and an automated headless REAPER render (Linux).
- **The event plane (§9)** — parameter sets and ramps at exact sample
  timestamps (±1 sample on hardware), DAW automation synthesized into
  device-interpolated ramps, UMP note input (the refdev is a playable
  envelope+portamento synth), and front-panel echo (turn a knob on the device,
  the DAW records the automation). Note-offs are unloseable; the DAW's panic
  (CC 120/123) reaches the hardware.
- **Musical time (§9.7)** — the device follows `evt.transport`: a note-latch
  arpeggiator whose step clock derives from the (timestamp, PPQ, tempo) anchor
  lands on division boundaries sample-exactly, survives loop wraps, and renders
  a byte-identical *groove hash*. (Params grew to 12 — the first param-map-hash
  change; old projects map onto matching ids with a warning, §9.3.)
- **Multi-device** — two boards on one bus; each instance binds its own by USB
  identity (saved serial, else first unclaimed same-model — never a different
  synth), reconnect pins that exact unit, two instances claim two devices by
  contention.
- **The web panel** — front panel plus a live protocol inspector: refs with
  ticking generations, the dirty flag, counters, and Snapshot / Panic /
  one-click Load of any archived state (patch time-travel — the current state
  is archived first, so nothing is ever lost).

**Gated in CI on every push:**

- **Hardware suite** on a real Pi (`scripts/hw-tests-linux.sh`, the `hw`
  badge): recall round-trips, ±1-sample note timing with zero late events, a
  realtime soak flooding automation + notes + panel traffic across DAW buffers
  64–1024 (zero silence gaps, zero drops, bounded padding), an automated
  mid-stream replug, an IDM-flood determinism gate, and the REAPER real-DAW
  determinism + recall round-trip — with the daemon under test built on the
  board from the commit being tested.
- **Sandboxed suite**: builds + unit tests on three OSes, pluginval
  strictness 10 (macOS / Windows / Linux), `auval` (macOS), fuzzed parsers
  (libFuzzer + ASan), and a protocol-abuse test that slams a live daemon with
  hostile traffic (sessions reset, nothing crashes).
- Certification-suite behaviors in miniature: T2/T3 (unplug/replug), T5
  (silent hash-verified reopen), T9 (malformed input), T10 (power-loss safety),
  T15/T17 (byte-identical renders), T16 (event timing).

**Not yet:** the four-safe-actions UI (v0 auto-resolves by Push-with-archive),
runtime/shell process split (§15.1), firmware management (§13), class-audio
coexistence (§8.5), free-running ASRC for analog devices, a CLAP port, the TCP
companion spec (§4.4).

The spec is an **editor's draft**: breaking changes expected, version
negotiated at `core.hello`. Changes flow through HARP Enhancement Proposals
(§18); two interoperating implementations are required before a HEP merges once
the process formalizes.

## Licensing

- Specification text: **CC BY 4.0**
- Schemas, reference implementation, test suite: **Apache-2.0**
- Patent posture: royalty-free; contributors sign a non-assertion
  covenant (spec §19). Use of the protocol is unrestricted; the
  certification *mark* (future) requires passing the test suite.
