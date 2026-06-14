# Architecture — one page

## The shape of the system

```
┌─ DAW (Ableton Live, Logic, …) ───────────────────────────────────┐
│  ┌─ harp-shell.vst3 / harp-au.component ─────────────────────┐   │
│  │  thin format shells (VST3 processor/controller; AUv2      │   │
│  │  dispatch + workgroup join) over ONE embedded runtime     │   │
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
- The transport is async libusb (second generation): one event thread
  owns completion reaping at elevated QoS, both IN pipes keep transfers
  always pending into byte FIFOs, pacing writes are fire-and-forget
  slots. This killed the sync transport's 20-25 ms completion tails and
  let the ring cushion drop 5 -> 2 blocks: 16 ms total reported latency
  at DAW blocks <= 256, and the §4.2.1 drain-on-stall dance is gone by
  construction. Debugging it taught: a die() mid-stream with transfers
  pending can corrupt bus state (toggle desync) that survives
  process death and makes EVERY subsequent run fail — heal with a
  bus-level reattach (daemon restart) before trusting any A/B result.
- The frontier cap is event-timing law: the pacing frontier never
  advances past read + target + (headroom − dawBlock), bounded at the
  frame END, so the earliest timestamp a block can mint clears every
  in-flight frame by at least one DAW block. Without it, small-block
  sessions mint timestamps into already-paced ranges and no wire
  ordering can save them (measured at 64: mid-frame events applied a
  frame late while fence_timeouts stayed zero — delivery was perfect,
  the math wasn't).
- One runtime per plugin instance (no singleton): each binds its own
  device by USB identity. Selection is exact-serial -> first-unclaimed
  same-model -> fresh-any, NEVER a different model; the USB claim is the
  mutual exclusion so racing instances get distinct devices; once bound,
  reconnect targets that exact serial only (a replug can't steal a
  sibling track's device). The VST3 Controller, a separate object, never
  opens a device — it reads knob values straight from the self-describing
  recall bundle. The bundle records the device's USB vid:pid:serial.
- Device cross-thread state is C11 atomics with explicit, commented
  orderings — no "benign race" volatiles (UB, and TSan-opaque). Engine
  note/queue/ramp state is private to engine.c; other threads get
  panic-grade operations that must work even when the queue cannot
  (all-notes-off, note-off-if, full-check). TSan runs on the Pi against
  production USB traffic; instrumented builds render different floats
  than release builds, so oracles are only ever captured from release.
- Synthesized-event hygiene: never emit a timestamp the stream has
  already passed. The ramp-thinning pend flush once emitted 64-sample
  ramps whose END equaled "now" (~1100/s at 64-sample buffers); a pend
  with no successor for a pacing block is a finished gesture and
  flushes as a "now" SET of the final value.
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
  batched into one framed write per cycle (per-event writes starve the
  pipe), and reported latency includes a DAW block of event headroom
  (spec 0.3.3, normative).
- Events get their OWN thread (the event pump) — their wire deadline is
  ~one DAW block while a pacing write can stall 8 ms in drain-on-stall,
  and sharing a loop spends the event budget on someone else's stall.
  But decoupling alone is WORSE than sharing: events and pacing ride
  different USB pipes with no mutual ordering, and the in-loop ordering
  was silently load-bearing (measured: pump-alone tripled evt_late).
  The event fence (spec 0.3.4, §8.3.1) restores order by construction:
  every pacing frame names the count of events queued so far; the
  device won't render the range until it has consumed that many. Fence
  waits are µs; timeouts are harmless overshoot (the fence counts
  events timestamped blocks in the future) and counted. Flood-verified:
  evt_late zero, ramp_late rate down 10×.
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
