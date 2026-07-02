# Shell runtime — thread lifecycle & coordination

A navigability aid for `shell/runtime*.cpp`: which threads exist, who spawns and
joins them, the lock-free rings they hand data across, and the atomics /
memory-ordering that make it correct. It is descriptive of the code as it stands
(`runtime.cpp`, `runtime_audio.cpp`, `runtime_events.cpp`, `runtime_session.cpp`,
`ring.h`), not aspirational. For the one-page system picture see
[`architecture.md`](architecture.md); this is the drill-down on threading.

Everything below lives inside ONE `HarpRuntime` (the embedded host runtime, §15.1).
The format shells (VST3 `plugin.cpp`, the FX `fx_plugin.cpp`, CLAP, AU) are thin
adapters that drive this one object; they add no threads of their own.

## The threads

| thread | member | spawned in | joined in | body |
|---|---|---|---|---|
| DAW audio | *(host-owned)* | — | — | `process()` → `pullAudio*()`, `queueParam/Ramp/Note()`, `feedTransport()`, `writeFxInput()`, `streamPos()` |
| supervisor | `supervisorThread_` | `start()` | `stop()` | `supervisor()`: session lifecycle + reconnect; runs `feeder()` while connected |
| reader | `readerThread_` | `sessionUp()` | `sessionDown()` | `reader()`: D→H audio frames → `audioRing_` (+ per-part sink rings) |
| event pump | `eventPumpThread_` | `sessionUp()` | `sessionDown()` | `eventPump()`: owned event ring → framed EVT writes |
| transport completion | *(in `host/usb_io.c`)* | transport open | transport close | async libusb completion reaping |
| main / UI | *(host-owned)* | — | — | `start()`, `stop()`, `getStateBundle()`/`setStateBundle()`, recall/reconcile |

On Apple hosts the three RT-adjacent threads (audio, reader, event pump) join the
host's CoreAudio workgroup — `WgState` + `wgMaintain()`, wired from the shell's
`RenderContextObserver`. `harp_thread_set_realtime()` puts reader + event pump on
the same RT class as the feeder so pacing and events stay in lockstep.

## Lifecycle: spawn / join nesting

```
start()                              running_ = true
 └─ supervisorThread_ = supervisor()
     while running_:
        sessionUp()                  hello + identity, audio.start,
        │                            reset SSI domain, connected_ = true
        │   readerThread_    = reader()      ← spawned here
        │   eventPumpThread_ = eventPump()   ← spawned here
        │
        feeder()                     returns on !running_ | transport died |
        │                            mode-flip pending
        │
        sessionDown()
            readerStop_ = true
            readerThread_.join()     ← joined BEFORE transport_ is freed
            readerStop_ = false
            eventPumpThread_.join()
            audio.stop (orderly), free transport_
        (reconnect: ~1 s cadence, or immediate re-dial on a live↔offline flip)
stop()                               running_ = false; supervisorThread_.join()
```

The supervisor OWNS the reader/eventPump lifecycle — `sessionUp`/`sessionDown` only
spawn and join them; the protocol work lives in the shared TUs. The load-bearing
ordering invariant: **the reader is joined before `transport_` is freed.** The DAW
audio thread only ever touches the lock-free rings and atomics — never `transport_`
— so a reconnect reaping the transport cannot race the audio thread.

Two loop-exit subtleties, both in the code's own comments:
- **reader** loops on `running_ && !readerStop_` — NOT on `connected_`, because the
  reader is the thread that *sets* `connected_ = false` on device-gone. On a
  mid-stream live↔offline flip the transport is still alive, so `sessionDown()` must
  explicitly `readerStop_ = true` (+ join) to quiesce it; on a dead-transport
  reconnect it self-exits within one `recvAudio` timeout (≤100 ms).
- **eventPump** DOES gate on `connected_` (it is not the flipper), so it unwinds on
  its own once `connected_` clears.

## The lock-free SPSC rings (`ring.h`)

All of these rings are single-producer / single-consumer with the same acquire/release
cursor pattern: the producer writes payload then `head_.store(release)`; the consumer
`head_.load(acquire)` then reads and `tail_.store(release)`. No locks on any of these
paths.

| ring | type | producer | consumer |
|---|---|---|---|
| `audioRing_` | `FloatRing` | `reader()` (USB demux / RTP 1:1) | DAW `pullAudio()` / `pullAudioBlocking()` |
| `AudioSink::ring` (per part) | `FloatRing` | `reader()` — sole writer of every sink (it iterates the set) | that instance's `pullAudio(sink)` — sole reader |
| owned event ring | `TimedRing` in `EventManager::ownerSource_` | DAW audio/plugin thread via `queue*` | `eventPump()` via `drainOwner()` |
| `echoRing_` (device→host) | `ParamRing` | `feeder()` via `pollEcho()` | DAW audio thread (`process()` → `outputParameterChanges`) |

`FloatRing::clear()` is a consumer-side `tail_` move — safe against the producer's
`head_` writes — used by `syncSinkEpoch()` to drop a stale pre-renegotiation ring.

**Per-part audio demux (§8.2, P5b).** The device streams ONE frame per pacing range
carrying the UNION of every attached instance's requested output slots. `reader()`
splits each frame (`demuxUnionFrame`) into the owner main mix (`audioRing_`, the
byte-identical single-instance default) plus each registered `AudioSink`'s columns.
The AudioSink *registry* (add on acquire / remove on release) is the only shared
mutable structure and is guarded by `sinksMutex_` against the reader's iteration; the
audio thread never touches that mutex (it holds a raw `AudioSink*`). `haveSinks_` is
an atomic mirror for the reader's relaxed fast-path.

## The pull path & the underrun tail

`pullAudio*()` is the RT consumer of `audioRing_` / a sink ring. There are four
variants — {owner, per-part sink} × {realtime, offline-blocking}:

- **realtime** (`pullAudio`): non-blocking drain; on a short read pad the tail with
  silence and (when `connected_`) count the underrun.
- **offline-blocking** (`pullAudioBlocking`, §8.3 / `kOffline`): poll until the SSI
  range arrives, bounded by a timeout, with a live↔offline mode-flip fence; counts
  unconditionally.

All four share one epilogue — zero-fill `[got, want)`, accrue the pad debt, bump the
`underruns_`/`padSamples_` diag counters — factored into `HarpRuntime::padUnderrun()`
(`runtime.cpp`). Each caller keeps its own gating by what it passes: the owner
realtime pull passes a *null* pad-debt accumulator when an FX is armed (its short read
is PDC-late priming silence, not spent SSIs) and the `connected_` load as the count
gate; the offline pulls pass `true`.

**Pad debt = constant latency across underruns.** A padded SSI is *spent*: when the
late frame finally arrives, `settlePadDebt()` / `settleSinkPadDebt()` drop exactly
that many floats (consumer-side reads) so the stream stays phase-aligned rather than
drifting later on every gap. `syncSinkEpoch()` handles a sink whose slots (re)entered
the union mid-session: on the first pull after the epoch bump it zeroes a bogus pad
debt and `clear()`s the stale ring so the first real demuxed frame plays (the B3 fix).
`padDebt`/`padDebtFloats_` are audio-thread-owned (single consumer) and need no atomic.

## Mutex scopes

- **`ctlMutex_`** — serializes all framed-link control-plane traffic against the
  feeder. Held by: the eventPump's batched EVT write and its escalated-panic write;
  the main thread's `getStateBundle`/`setStateBundle`/recall/reconcile; the feeder's
  P5b re-negotiation (`audioRenegotiateLocked`) and the §14.3 loopback probe; the
  fence-epoch mutations (`advanceEpoch`, `resetFence`). **The DAW audio thread never
  takes `ctlMutex_`** — it only touches lock-free rings and atomics, so a stalled
  control op can never block audio.
- **`sinksMutex_`** — guards the AudioSink registry and `epoch` bumps (add/remove off
  the wire, on the plugin thread) against `reader()`'s per-frame iteration. Off the
  RT audio path.
- **`wgMutex_`** — guards the CoreAudio workgroup handle across the observer callback
  and the RT threads' join/leave.

## Atomics & memory-ordering coordination (RT producer ↔ consumer)

- **Ring publish** — `FloatRing`/`TimedRing`/`ParamRing` cursors: producer releases
  on `head_`, consumer acquires it, then releases on `tail_`. The acquire/release
  pairing is the sole synchronisation between the reader and the audio thread — no
  lock, no fence beyond the cursor stores.
- **`connected_`** — release-stored by `sessionUp` (up) and by `reader()` on
  device-gone (down); acquire-loaded by the realtime `pullAudio` counter gate, the
  feeder, and the supervisor.
- **`readerStop_`** — release-stored by `sessionDown`/the re-neg, acquire-loaded in
  the reader loop; the handshake that quiesces the reader while the transport is
  still alive.
- **`evtQueuedSeq_`** — MONOTONIC, only ever `fetch_add(release)` by `queue*`;
  `evtEpochBase_` is written only under `ctlMutex_` (`advanceEpoch` on a re-neg,
  `resetFence` at session start). Fence readers go through `fenceStamp()` (a saturating
  `hw - base`), so events straddling a re-negotiation are counted below the new epoch
  base and never satisfy a post-reneg frame's §8.3.1 fence.
- **`AudioSink::epoch`** — release-stored under `sinksMutex_` by `audioStart`,
  acquire-loaded by `syncSinkEpoch()`; `epochSeen` is consumer-private (pull side only).
- **`ssiRead_`** — advanced by the owner pull (`fetch_add`, relaxed) as the
  stream-domain clock; read cross-thread via `streamPos()` for event timestamping.
  `haveSinks_`, `unionWidth_`, `freeRunning_`, `wantHostPaced_` are relaxed flags read
  on the reader/audio fast paths and published off the RT path.
