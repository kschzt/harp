# Architecture — one page

## The shape of the system

```
┌─ DAW (Ableton Live, …) ──────────────────────────────────────────┐
│  ┌─ harp-shell.vst3 ─────────────────────────────────────────┐   │
│  │  processor/controller (VST3 API, frozen UIDs)             │   │
│  │  ┌─ embedded HARP runtime ───────────────────────────┐    │   │
│  │  │ feeder thread: ALL USB I/O                        │    │   │
│  │  │  · paces SSI blocks ──► lock-free ring ──► process()   │   │
│  │  │  · param pushes (coalesced, audio outranks knobs) │    │   │
│  │  │  · control plane: hello, refs, snapshot, CAS push │    │   │
│  │  └───────────────────────────────────────────────────┘    │   │
│  └──────────────────────────┬────────────────────────────────┘   │
└─────────────────────────────┼────────────────────────────────────┘
                       USB (vendor interface FF/48/01)
              bulk pair 1: framed link (ctl │ obj │ evt │ log)
              bulk pair 2: HARP stream (timestamped audio frames)
┌─────────────────────────────┼────────────────────────────────────┐
│  Raspberry Pi 4B — harp-deviced (FunctionFS gadget)               │
│  session loop (ctl/obj) · audio thread (render/pace) · engine     │
│  content-addressed object store + refs (crash-atomic CAS)         │
└───────────────────────────────────────────────────────────────────┘
```

The same `harp-deviced` also speaks the framed link over TCP for
hardware-free development (`harp-probe -d host:port`), and `harp-probe`
exercises every protocol flow from the command line.

## The four planes (spec §3.2)

| plane | carries | transport |
|---|---|---|
| control | CBOR request/response/notify (`core.*`, `state.*`, `audio.*`) | framed link, stream 0 |
| state | content-addressed objects, credit-controlled bulk | framed link, stream 2 |
| events | UMP + parameter events (§9 — not yet implemented) | framed link, stream 1 |
| audio | timestamped PCM frames | dedicated bulk endpoint pair |

## State model in one breath (spec §10–§11)

Immutable objects (blob/list/tree/snapshot) addressed by SHA-256 of
their deterministic CBOR encoding; named refs with generation counters
and a dirty flag; ref updates only by compare-and-swap; every overwrite
preceded by a snapshot that content addressing makes free. A project's
expected state and a device's actual state compare in 33 bytes.

## Clocking (spec §7–§8)

Free-running mode: the device crystal owns the stream, frames carry
`(epoch, MSC)` timestamps, the host resamples (ASRC) into the DAW
domain. Host-paced mode (pure-digital paths): the host's Stream Sample
Index paces rendering — no device clock in the loop, deterministic
byte-identical output, pacing faster than real time = offline bounce.
The reference shell uses host-paced exclusively (the refdev has no
converters).

## Threading rules that bit us (so they're rules now)

- The DAW audio thread never blocks, allocates, or touches USB; it reads
  a lock-free ring. Offline rendering contexts may block on the wire;
  real-time contexts pad silence and count it (spec §15.1).
- Control traffic yields to stream service on both sides; on the device,
  per-edit dirty-flag persistence coalesces (only clean→dirty hits
  storage synchronously) because fsync-per-knob-tick starves audio on SD
  cards (spec §9.2, §10.3).
- Bulk USB pairs have no TCP-style buffering: a peer blocked writing
  while its counterpart blocks writing the other way deadlocks with both
  locally correct. Keep an inbound read pending; drain on stall
  (spec §4.2.1).
