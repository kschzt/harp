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

## Spec ambiguities found while implementing (candidate HEP/errata)

1. **§15.3 "full closure of `live/project`"** — a snapshot's closure
   strictly includes its parent chain, i.e. unbounded history in every
   bundle. This implementation embeds the *state* closure (root tree +
   reachable objects, parents excluded) and devices do not require parent
   ancestry to be present at `state.refset`.
2. **`state.want` response body** is unspecified; this implementation
   replies `{0: count-of-objects-queued}` before sending objects.
3. **`state.refset` on an unborn ref** with the create flag: the `expect`
   field is still required by the CDDL; we send `null` and the device
   accepts `null`-or-create.
