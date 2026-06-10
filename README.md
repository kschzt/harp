# HARP — Hardware Audio Recall Protocol

Reference implementation of the [HARP specification](spec/harp-spec-draft-0.3.md)
(draft 0.3.0). Layout follows the spec's Appendix E.

## Status

Working today (`harp-core` + `harp-recall` over a TCP dev transport):

- **`core/`** — portable C11 library, no dependencies: §4.2 framed link,
  RFC 8949 deterministic CBOR, SHA-256, §10 content-addressed objects
  (blob/list/tree/snapshot), file-backed object store, refs with
  crash-atomic (tmp+rename) CAS updates, §5.2 envelopes.
- **`device/harp-deviced`** — reference device daemon. The "engine" is a bank
  of 8 parameters; knob turns dirty the live ref, snapshots serialize it,
  refsets restore it atomically. State survives power cycles. Runs on macOS
  and Linux (Pi 4B, KR260).
- **`host/harp-probe`** — host CLI implementing the §12.2 project-open flow:
  pull, archive-before-push, CAS refset, silent path on hash match.
- **`tests/`** — unit tests for the core (RFC 8949 test vectors included).

Not yet: audio plane (§8), event plane (§9), VST3 shell (§15), FunctionFS
USB gadget transport, firmware management (§13).

## Build & demo

```sh
cmake -B build && cmake --build build
./build/harp-tests                                   # unit tests

./build/harp-deviced --state-dir /tmp/refdev &       # the "hardware"
./build/harp-probe demo                              # narrated recall walkthrough
```

`harp-probe` subcommands: `identify`, `refs`, `counters`, `params`,
`knob ID VAL` (simulate a front-panel edit), `save` (Pull), `restore`
(Push with archive-before-push), `demo`. Flags: `-d HOST:PORT`
(default `127.0.0.1:47800`), `-s STOREDIR` (default `./host-store`).

## Transport note

TCP here is a **development transport** (the framed link over a socket) —
per spec §4.4 a network binding must not ship under the `harp` identifier
until its companion spec exists. The normative USB binding (§4.3) lands as a
FunctionFS gadget backend for `harp-deviced` on the Raspberry Pi 4B; the
link layer is fd-based, so only the attach path changes.

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
