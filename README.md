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
  mismatch resolved through explicit safe actions — never silently.
- **Audio as a plugin** — a dedicated stream into the plugin shell that
  bypasses the OS audio stack (no aggregate-device hacks), including a
  host-paced mode where the hardware renders *deterministically*, byte-
  identical, faster than real time: offline bounce through a physical box.
- **Identity, timing, diagnostics** — engine versioning that protects old
  songs from new firmware, measured (never guessed) latency, and error
  counters that end "it glitched" support threads with evidence.

This repository holds the [specification](spec/harp-spec-draft-0.3.md)
(draft 0.3.2, CC BY 4.0) and its reference implementation (Apache-2.0): a
portable C core, a reference device daemon that turns a Raspberry Pi 4B
into a HARP instrument over USB, a host CLI, a CLI VST3 test host, and a
VST3 plugin shell. **The whole stack runs today**: the Pi plays as an
instrument track in Ableton Live 12 — knobs as automation, total recall
through the Live set's own save/reopen, offline bounce through the
hardware. Repository layout follows the spec's Appendix E.

## Status

Working today, verified Pi 4B ↔ macOS over the **normative §4.3 USB
binding** (and a TCP dev transport for simulation):

- **`core/`** — portable C11 library, no dependencies: §4.2 framed link,
  RFC 8949 deterministic CBOR, SHA-256, §10 content-addressed objects
  (blob/list/tree/snapshot), file-backed object store, refs with
  crash-atomic (tmp+rename) CAS updates, §5.2 envelopes, §8.2 audio
  frame codec.
- **`device/harp-deviced`** — reference device daemon (`harp-core`,
  `harp-recall`, `harp-stream`). The engine is a stereo drone synth whose
  whole voice is the 8 recallable params. Transports: TCP (simulation, any
  OS) and FunctionFS USB gadget (Linux). Audio: free-running streaming with
  MSC timestamps, and host-paced mode with `audio.deterministic` (T15
  passes: byte-identical double renders) and `audio.offline-rate`
  (21.7× real time measured). State survives power cycles and cable yanks.
- **`host/harp-probe`** — host CLI: the §12.2 project-open flow (pull,
  archive-before-push, CAS refset, silent path on hash match), audio
  capture and offline render over libusb.
- **`shell/`** — the **VST3 plugin** (`HARP RefDev`, Instrument|Synth):
  embedded §15 runtime (feeder thread, lock-free rings, host-paced
  stream into `process()`), device params as automation,
  `getState`/`setState` as the §15.3 Recall Bundle with
  archive-before-push reconcile. Verified in **Ableton Live 12** and
  closed-loop against the CLI host below.
- **`tools/vst3-host`** — CLI VST3 host for automated, agent-driven
  testing: params, block processing, WAV + hash, DAW-style state
  round-trips. The SDK validator also runs on every shell build.
- **`tests/`** — unit tests (RFC 8949 vectors included).

Not yet: event plane (§9 — params currently ride a vendor method;
sample-accurate events, ramps, and echo-to-automation come with it),
four-safe-actions UI (v0 auto-resolves by Push-with-archive), runtime/
shell process split (§15.1), firmware management (§13), class-audio
coexistence (§8.5), free-running ASRC path for analog devices, AU/CLAP
ports, TCP companion spec (§4.4).

## Build & demo

```sh
cmake -B build && cmake --build build     # libusb-1.0 enables the USB transport
./build/harp-tests                        # unit tests

# simulator (TCP dev transport):
./build/harp-deviced --state-dir /tmp/refdev &
./build/harp-probe demo                   # narrated §12.2/§11.4 recall walkthrough

# real hardware (Pi gadget, see scripts/pi-bringup.md):
./build/harp-probe -d usb identify
./build/harp-probe -d usb demo
./build/harp-probe -d usb record 4 take.wav    # free-running capture, MSC-verified
./build/harp-probe -d usb render 8 bounce.wav  # host-paced offline bounce
./build/harp-probe -d usb t15 4                # determinism: render twice, byte-compare
```

`harp-probe` subcommands: `identify`, `refs`, `counters`, `params`,
`knob ID VAL` (simulate a front-panel edit), `save` (Pull), `restore`
(Push with archive-before-push), `record SECS WAV`, `render SECS WAV`,
`t15 SECS`, `dev-restart` (respawn the daemon — sudo-free deploys), `demo`.
Flags: `-d HOST:PORT|usb` (default `127.0.0.1:47800`), `-s STOREDIR`
(default `./host-store`).

```sh
# VST3 shell + CLI test host (needs CMake >= 3.25; SDK in external/vst3sdk)
cmake -B build-vst -S tools/vst3-host
cmake --build build-vst --target install-live   # build, re-seal, install for Live
./build-vst/harp-vst3-host ~/Library/Audio/Plug-Ins/VST3/harp-shell.vst3 \
    --set 3=0.8 --seconds 2 --out take.wav      # drive the shell without a DAW
```

DAW-compatibility lore the hard way (details in git history and
`docs/vst3-shell-plan.md`): Live requires Instrument plugins to declare
an event input bus; the SDK's moduleinfotool breaks the codesign seal it
just made (re-seal post-build or Live rejects the bundle); Live doesn't
follow VST3 symlinks; a changed plugin binary needs a Live restart, not
a rescan.

## Transport note

TCP here is a **development transport** (the framed link over a socket) —
per spec §4.4 a network binding must not ship under the `harp` identifier
until its companion spec exists. The product path is the USB binding:
vendor interface FF/48/01, framed link on one bulk pair, HARP stream on a
second (§8.2), found by the §6.1 class-triple probe.

## Implementation findings → spec 0.3.1 / 0.3.2

Ambiguities and wire-level lessons from this implementation were folded
into the spec as the 0.3.1 errata (see the changelog at the top of
`spec/harp-spec-draft-0.3.md`): state-closure definition (parents
excluded) for refset validation and bundles, `state.want` response,
`expect = null` semantics, `slots = 0` pacing frames, class-triple
discovery promoted to MUST (gadget devices can't author BOS platform
capabilities), and — the hard-won one — bulk-pair flow-control rules:
mutually blocked writers deadlock with both sides locally correct, so
hosts drain inbound whenever outbound stalls. T15 (`audio.deterministic`)
and `audio.offline-rate` are demonstrated on this hardware: byte-identical
double renders, 8 s bounced in 0.37 s (21.7× real time).

0.3.2 came from the first DAW integration: per-edit fsync of the dirty
flag starved the audio path on the Pi's SD card under slider drags (now:
persist the clean→dirty transition only, coalesce the rest, flush before
any reader), and shells must let control-plane traffic yield to stream
service — audio outranks knobs on the host too.
