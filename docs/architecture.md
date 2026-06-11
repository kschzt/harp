# Architecture — one page

## The shape of the system

```
┌─ DAW (Ableton Live, …) ──────────────────────────────────────────┐
│  ┌─ harp-shell.vst3 ─────────────────────────────────────────┐   │
│  │  processor/controller (VST3 API, frozen UIDs)             │   │
│  │  ┌─ embedded HARP runtime ───────────────────────────┐    │   │
│  │  │ feeder thread: pacing + events + link inbound     │    │   │
│  │  │  · paces SSI blocks (small fixed pipeline)        │    │   │
│  │  │  · timestamped param/ramp/note events (§9)        │    │   │
│  │  │ reader thread: audio-IN read ALWAYS pending ──►   │    │   │
│  │  │     lock-free ring ──► process()  (pad debt keeps │    │   │
│  │  │     latency constant across underruns)            │    │   │
│  │  │ main thread: hello, refs, snapshot, CAS push      │    │   │
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
| events | UMP notes + timestamped param sets/ramps + echo (§9) | framed link, stream 1 |
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
- "Inbound read pending" applies PER PIPE: the audio stream needs its
  own dedicated reader thread — if the device's response writes ever
  wait for the host to post a read, its strictly-serial pacing loop
  inherits that wait, and depth-probing a serial device injects the
  stalls it tries to absorb.
- Underrun policy: padded stream positions are SPENT. Late arrivals for
  them get dropped (pad debt), or every pad permanently grows latency
  and replays the missing moment as an "echo" while the DAW grid
  drifts. The ring cushion scales with the DAW block size
  (max(5×256, 2×block)); reported latency follows it.
- Event rate is bounded at the source: one ramp per param per 256
  samples, points folded into the next ramp's target — a 64-sample-
  buffer DAW emits ~750 points/s/param and the wire doesn't need them
  (the device interpolates at control rate regardless).
- Events go to the wire BEFORE the pacing frames covering their
  timestamps (a pacing frame triggers the render of its range), they're
  batched into one framed write per feeder cycle (per-event writes
  starve pacing), and reported latency includes a DAW block of event
  headroom (spec 0.3.3, normative).
- Notes are performance state OF A STREAM: they die at stream
  start/stop/session-end, note-offs are never droppable (overflow
  escalates to all-notes-off), and CC 120/123 panic works at every
  layer. A stuck note is always a bug.
- Late-event accounting: `evt_late` (notes/sets) is zero-tolerance;
  ramp-end misses are budgeted separately (`ramp_late`) because a
  ramp's start anchors at the previous automation point — one block of
  structural margin.
- Sessions are disposable; the supervisor is not. The shell's supervisor
  thread owns connect/run/reconnect: a transport death tears the session
  down (reader reaped, claim released) and the next attempt is just the
  connect path again — fresh link reassembly state, fresh rid space, new
  SSI time domain, project bundle re-asserted. Unplug/replug and a
  device firmware restart are the same code path, tested on hardware
  (scripts/replug-test.sh).
- Parsers are fuzzed (spec §16/T9): every byte a peer can send passes
  through code in fuzz/'s targets — framed-link reassembly, CBOR,
  envelopes, objects, audio headers — under ASan in CI, plus a live
  protocol-abuse test that asserts hostile traffic ends in session
  resets, never crashes (scripts/abuse-test.sh).
