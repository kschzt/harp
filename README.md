# HARP — Hardware Audio Runtime Protocol

**An open standard that makes hardware instruments behave like plugins.**

Hardware synths, drum machines, and effects have connected to computers for
forty years, and the connection is still fragile in one specific way:
*recall*. Audio mostly works, MIDI mostly works — but reopen last month's
project and the hardware is not in the state you saved, and nothing will
tell you. The deep integrations that fix this (multichannel USB audio,
plugin-shell control, total recall) exist only inside closed single-vendor
ecosystems, because every vendor must rebuild drivers, state sync, and DAW
compatibility from scratch — and most can't afford to.

HARP makes that integration a shared, open substrate. A device implementing
it gets, from any conforming host:

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
  at ~25× real time, **16 ms total reported latency** at typical buffer
  sizes — in the neighborhood of a good audio interface.
- **Sample-accurate everything (§9)** — DAW automation becomes
  device-interpolated ramps applied within ±1 sample; notes travel as UMP;
  an event *fence* makes "applied late" structurally impossible rather
  than statistically rare. Turn a knob on the hardware and the DAW records
  the automation (echo).
- **Musical time (§9.7)** — devices follow the DAW's transport: tempo,
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

**The whole stack runs today**: a Raspberry Pi 4B plays as an instrument
track in Ableton Live 12 — knobs as automation lanes, total recall through
the Live set's own save/reopen, a tempo-locked arpeggiator, offline bounce
through the hardware.

**Degree of completeness:** the four protocol planes (control, state,
events, audio) are implemented end-to-end and verified on real hardware by
a conformance kit — recall round-trips, ±1-sample event timing, realtime
floods across buffer sizes 64–1024, hot-replug, tempo lock, determinism
hashes — plus fuzzed parsers and a ThreadSanitizer-clean device. The
specification is an **editor's draft** (0.3.4): breaking changes are
expected and versions are negotiated at hello; its changelog records what
implementation taught us. One reference device, one DAW (Live), one OS
(macOS) so far — multi-device support, the four-actions recall UI, AU/CLAP
ports, and the Ethernet binding are next (see [Status](#status) for the
full honest list).

## Repository map

```
spec/             the specification (draft 0.3.4) + machine-readable CDDL
core/             portable C11 protocol library, no dependencies:
                  framing, deterministic CBOR, SHA-256, content-addressed
                  objects, crash-atomic ref store, audio frame codec
device/           harp-deviced — the reference device daemon: an 8-knob
                  stereo synth engine behind the protocol. Transports:
                  TCP (simulation, any OS) and FunctionFS USB gadget (Linux)
host/             harp-probe — host-side CLI: recall flows, audio capture,
                  offline render, determinism checks (libusb)
shell/            the VST3 plugin ("HARP RefDev"): embedded host runtime,
                  host-paced audio into process(), params as automation,
                  getState/setState = Recall Bundle
tools/vst3-host/  CLI VST3 host for automated testing of any plugin —
                  params, block processing, WAV+hash, state round-trips
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

Requires CMake ≥ 3.25, libusb (`brew install libusb`), and the VST3 SDK
cloned to `external/vst3sdk` (`git clone --recursive
https://github.com/steinbergmedia/vst3sdk.git external/vst3sdk`).

```sh
cmake -B build-vst -S tools/vst3-host
cmake --build build-vst --target install-live   # build, sign, install for Live
```

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

Working and verified end-to-end: `harp-core`, `harp-recall`,
`harp-stream` (free-running + host-paced with `audio.deterministic` and
`audio.offline-rate`), the USB binding, the VST3 shell in Ableton
Live 12, and the event plane (§9): parameter sets and ramps applied at
exact sample timestamps (±1 sample verified on hardware), DAW
automation synthesized into device-interpolated ramps, UMP note input
(the refdev is a playable envelope+portamento synth), and front-panel
echo — turn a knob on the device's web panel and the DAW records
automation. The panel doubles as a live protocol inspector with
actions: refs with ticking generations, the dirty flag, counters, plus
Snapshot, Panic, and one-click Load of any archived state (patch
time-travel; current state is archived first — nothing is ever lost).
The hardware conformance kit (`scripts/hw-tests.sh`) runs recall
round-trips, note-timing checks (±1 sample pitch arrival, zero late
events), a realtime soak that floods automation + notes + panel
traffic asserting zero silence gaps, zero drops, and bounded padding
across DAW buffer sizes 64–1024, and an automated replug test: the
device detaches mid-stream and the shell reconnects on its own —
session re-established, project state re-asserted, audio back.
Note-offs are unloseable by design, and the DAW's panic (CC 120/123)
reaches the hardware. Every wire-facing parser is fuzzed (libFuzzer +
ASan in CI; `fuzz/`), and a protocol-abuse test slams a live daemon
with hostile traffic asserting sessions reset and nothing crashes.
Certification-suite behaviors exercised in miniature: T2/T3
(unplug/replug), T5 (silent hash-verified reopen), T9 (malformed
input), T10 (power-loss safety), T15 (byte-identical renders), T16
(event timing).

The arpeggiator demo is in: the device declares `evt.transport` (§9.7)
and follows the DAW's musical timeline — a note-latch arp whose step
clock derives from the (timestamp, PPQ, tempo) anchor, landing steps on
division boundaries sample-exactly by construction. Params grew to 12
(the first param-map-hash change; old projects map onto matching ids
with a warning, §9.3). The conformance kit gained T17: grid-exactness,
loop-wrap survival, and a byte-identical "groove hash" — two renders of
the same chord and transport are the same file.

Not yet: four-safe-actions UI (v0 auto-resolves by Push-with-archive),
runtime/shell process split (§15.1), firmware management (§13),
class-audio coexistence (§8.5), free-running ASRC for analog devices,
AU/CLAP/Windows-ASIO ports, TCP companion spec (§4.4).

The spec is an **editor's draft**: breaking changes expected, version
negotiated at `core.hello`. Changes flow through HARP Enhancement
Proposals (§18); two interoperating implementations are required before
a HEP merges once the process formalizes.

## Licensing

- Specification text: **CC BY 4.0**
- Schemas, reference implementation, test suite: **Apache-2.0**
- Patent posture: royalty-free; contributors sign a non-assertion
  covenant (spec §19). Use of the protocol is unrestricted; the
  certification *mark* (future) requires passing the test suite.
