# Architecture — one page

*Tracks HARP spec **1.1.2**. Two normative transport bindings: USB (§4.3) and Ethernet/IP (§4.4, §8.7).*

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

`harp-probe` exercises every protocol flow from the command line, over either
binding.

## Two transport bindings (spec §4.3–§4.4)

Everything above the pipe is transport-agnostic (`harp_io`,
`core/include/harp/link.h`); one protocol rides two normative bindings.

**USB (§4.3)** — the diagram above, the normative floor. A FunctionFS gadget
exposes the vendor interface (FF/48/01): bulk pair 1 carries the framed link
(ctl │ obj │ evt │ log), bulk pair 2 the native §8.2 audio frame. Reliable,
ordered, host-paced.

**Ethernet/IP (§4.4, §8.7)** — a first-class second binding for when USB stops
scaling (hub chains, ≳ 5 devices, §8.4 admission), not "USB with a longer
cable." The *same* framed link runs over one TCP connection (ctl/obj/evt/log
verbatim — socket buffering dissolves the §4.2.1 bulk-pair deadlock); the audio
plane moves to **RTP/UDP** (§8.7: payload type 96 single-packet / 97 grouped,
one SSRC per stream-slot group, sequence numbers for the loss detection USB's
reliable bulk made unnecessary). Devices are found by mDNS/DNS-SD
(`_harp._tcp`) on a dedicated **trusted segment** — media in the clear, signed
firmware the one constant (§16). The hardware-free dev link
(`harp-deviced --port` / `harp-probe -d host:port`) is this binding on
localhost, the same code path.

```
   host ⇄ ── framed link over TCP (ctl │ obj │ evt │ log) ─────── device
          └─ audio over RTP/UDP, free-running (live) ───────────┘
             or host-paced over TCP (deterministic offline bounce)
```

Both bindings are conformance-bearing and CI-gated: the §8.7 suite
(`scripts/eth-suite.sh`, the `eth.yml` workflow) runs on localhost across
macOS / Windows / Linux, and — since 2026-07-06 — over a **real network hop**
against the rig Pi (`hw.yml`): framed control, the RTP/UDP audio plane, the
host-locked rate-trim + ASRC, and the byte-exact host-paced offline bounce. USB
itself is gated on the real Pis (`hw.yml`).

## The four planes (spec §3.2)

| plane | carries | transport |
|---|---|---|
| control | CBOR request/response/notify (`core.*`, `state.*`, `audio.*`) | framed link, stream 0 |
| state | content-addressed objects, credit-controlled bulk | framed link, stream 2 |
| events | UMP notes + timestamped param sets/ramps + echo (§9) | framed link, stream 1 |
| audio | timestamped PCM frames | bulk pair (USB) │ RTP/UDP (Ethernet, §8.7) |

The three framed-link streams (0/1/2) carry over both bindings unchanged; only
the audio plane and the outermost pipe differ (bulk vs TCP+RTP/UDP).

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

Over **USB** the reference shell is host-paced exclusively (the refdev is
pure-digital, no converters). Over the **§8.7 Ethernet** binding the real-time
audio path is free-running RTP: because the refdev has a tunable emit rate it
advertises `audio.rate-lock`, so the host holds it **bit-exact** with an
`audio.trim` rate-correction loop (no resampler in the path), falling back to
receiver-side ASRC otherwise — while the **offline** bounce stays host-paced
over the reliable TCP link (deterministic, byte-identical, T15). A single
device's clock is recovered host-side from the stream itself (§7.3); device-side
PTPv2 for multi-device timeline alignment is a hardware prototype only (~22 µs
idle, software-timestamped), not yet wired into the runtime.

## Latency (spec §6.4, measured §14.3)

The reported PDC latency at DAW buffers ≤ 256 is **~21 ms** (1024 samples at
48 kHz), host-paced — the value the plugin advertises to the DAW
(`getLatencySamples`) and the DAW delay-compensates. Its breakdown:

| term | frames | ms @48k |
|---|---|---|
| ring buffer (2 × 256 pacing block) | 512 | 10.7 |
| event headroom (§9.2, one block) | 256 | 5.3 |
| device render/turnaround block (§6.4 key 3, what §14.3 measures as the RTT) | 256 | 5.3 |
| **total reported** | **1024** | **~21** |

The transport alone (ring + headroom = 768 frames ≈ **16 ms**) is the
async-libusb milestone figure recorded in `debt.md` #10; the reported *total*
folds in the device render block so the **declared** PDC equals the
**§14.3-measured** one. The value is CI-asserted to the sample
(`scripts/reported-latency-test.sh`).

On the free-running §8.7 path the constant is the RTP jitter buffer instead of
the USB ring: ~32 ms at the safe undeclared default (block 256), down to
**~9 ms** when a device declares a real-time profile (`audio.rt-floor`: a
320-frame buffer floor + 64-sample RTP packet at block 64).

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
  let the ring cushion drop 5 -> 2 blocks (the ~16 ms transport floor: ring
  512 + event headroom 256; the full reported PDC folds in the device render
  block for ~21 ms — see Latency above), and the §4.2.1 drain-on-stall dance
  is gone by construction. Debugging it taught: a die() mid-stream with transfers
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
  (§9.2, normative).
- Events get their OWN thread (the event pump) — their wire deadline is
  ~one DAW block while a pacing write can stall 8 ms in drain-on-stall,
  and sharing a loop spends the event budget on someone else's stall.
  But decoupling alone is WORSE than sharing: events and pacing ride
  different USB pipes with no mutual ordering, and the in-loop ordering
  was silently load-bearing (measured: pump-alone tripled evt_late).
  The event fence (§8.3.1, normative) restores order by construction:
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
