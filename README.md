# HARP — Hardware Audio Recall Protocol

Reference implementation of the [HARP specification](spec/harp-spec-draft-0.3.md)
(draft 0.3.0). Layout follows the spec's Appendix E.

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
- **`tests/`** — unit tests (RFC 8949 vectors included).

Not yet: event plane (§9), VST3 shell + runtime/shell split (§15),
firmware management (§13), class-audio coexistence (§8.5), TCP companion
spec (§4.4).

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

## Transport note

TCP here is a **development transport** (the framed link over a socket) —
per spec §4.4 a network binding must not ship under the `harp` identifier
until its companion spec exists. The product path is the USB binding:
vendor interface FF/48/01, framed link on one bulk pair, HARP stream on a
second (§8.2), found by the §6.1 class-triple probe.

## Implementation findings → spec 0.3.1

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
