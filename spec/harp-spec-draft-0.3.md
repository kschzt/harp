# HARP — Hardware Audio Recall Protocol

**An open standard for integrating hardware instruments with audio software hosts.**

Specification, Draft 0.3.0 — 10 June 2026

| | |
|---|---|
| **Status** | Editor's Draft — not yet stable, breaking changes expected |
| **Working name** | "HARP" is provisional pending trademark search; the protocol identifier `harp` is used normatively throughout |
| **Editors** | *(to be formed — initial working group)* |
| **Spec license** | CC BY 4.0 |
| **Schema & reference code license** | Apache-2.0 |
| **Patent policy** | Royalty-free; contributors sign a non-assertion covenant (§19) |
| **Feedback** | via HARP Enhancement Proposals (HEPs), see §18 |

> **Changes in 0.3.0** — The event layer is upgraded to full plugin-API parity (§9): scheduled ramp events, CLAP-style non-destructive modulation, per-voice addressing, full parameter flags (readonly/meter, bypass, hidden, periodic, modulatable), enum and piecewise display mapping with device-side format/parse fallback, output-parameter metering, and — closing the largest gap — transport/tempo/musical-time events so hardware sequencers follow the DAW timeline the way plugins do, not via MIDI clock. UMP remains the carrier for musical events. Certification adds T16–T17.

> **Changes in 0.2.0** — The audio plane is restructured around a corrected premise: a DAW is already locked to one low-latency interface and its clock, so plugin-bound device audio cannot ride the OS audio stack. The dedicated HARP stream (formerly "Profile B") is now the primary, normative plugin path (`harp-stream`); class-compliant audio (formerly "Profile A") becomes an optional coexistence feature for standalone use (`harp-class-audio`). New: an explicit clock-domain model — free-running devices with mandatory host-side ASRC and drift tracking, plus an optional host-paced mode in which the DAW clock paces device rendering, enabling deterministic and offline rendering. New: multi-device requirements — ≥ 8 concurrent sessions, bandwidth admission control, per-device clock recovery, cross-device timeline alignment. Certification adds T13–T15.

---

## Abstract

HARP defines how a hardware musical instrument and a software host (a DAW, a plugin, or a standalone application) establish identity, exchange audio and timed control events, and — critically — synchronize *state* such that a host project, once reopened, restores the connected hardware to exactly the condition in which it was saved.

HARP deliberately does not reinvent note semantics or plugin hosting — events ride on MIDI 2.0 Universal MIDI Packets, and devices surface in DAWs through existing plugin formats. Audio is the one place HARP defines its own transport, of necessity: a DAW is already locked to a single low-latency interface and its clock, so plugin-bound device audio travels a dedicated HARP stream straight into the plugin shell, bypassing the OS audio stack entirely. Devices MAY additionally present class-compliant USB audio for standalone use. What HARP adds is the layer that today exists only in closed, vendor-specific systems: device identity with verifiable state hashes, a content-addressed state model with Git-like safe synchronization, sample-accurate time correlation, mandatory diagnostics, firmware-aware recall, and a conformance suite strict enough that "HARP Certified" can mean *boring reliability*.

The design principle throughout is that the system is **hostile to ambiguity**: host state, hardware state, firmware version, engine version, sample rate, latency, and patch memory must never silently drift apart.

---

## Table of contents

1. Introduction
2. Conformance and terminology
3. Architecture
4. Transport bindings and framing
5. Control plane
6. Identity, discovery, and capabilities
7. Time, clocking, and the Monotonic Sample Counter
8. Audio plane
9. Events, parameters, and automation
10. State model
11. State synchronization
12. Session lifecycle and recovery
13. Firmware management
14. Diagnostics and observability
15. Host runtime and plugin shell requirements
16. Security considerations
17. Conformance classes and certification
18. Specification versioning and extensibility
19. Governance, licensing, and intellectual property

Appendix A — CDDL definitions (normative)
Appendix B — Worked example: the project-open handshake
Appendix C — Registries
Appendix D — User-facing feature mapping (informative)
Appendix E — Reference implementation and repository layout (informative)

---

## 1. Introduction

### 1.1 Motivation

Hardware instruments and software hosts have been connected for forty years, yet the connection remains fragile in a specific, well-understood way. Audio transport mostly works. Note transport mostly works. What does not work — outside a handful of closed, single-vendor ecosystems — is *recall*: the guarantee that a host project saved on Tuesday reopens on Friday with the connected hardware in precisely the state it was in, or with an honest, safe explanation of why it cannot be.

Single-vendor systems have demonstrated that deep integration is achievable: multichannel audio over USB, plugin-shell control, total recall of device state. They have also demonstrated the cost of keeping it closed — every vendor who wants this must rebuild drivers, plugin shells, state sync, firmware tooling, and a DAW compatibility matrix from scratch, and most cannot afford to. The result is an industry where boutique manufacturers build excellent sound engines and ship them with integration that is, at best, a librarian application and a prayer.

HARP exists so that integration competence becomes a *shared substrate* rather than a competitive moat. A vendor implementing HARP on a device gets, from any conforming host: enumeration, identity, multichannel audio, timed events, total recall with safety guarantees, firmware-mismatch handling, and diagnostics — without writing a line of host-side code.

### 1.2 Design principles

The following principles are normative inputs to every design decision in this specification:

1. **Hostility to ambiguity.** Any state that can drift between host and device is hashed, versioned, and compared. Mismatch is detected, surfaced, and resolved through explicit, safe actions — never silently.
2. **Recall is a transaction, not a stream.** State synchronization follows the model of version control systems: content-addressed objects, atomic compare-and-swap reference updates, snapshots before destructive operations. Total recall must never become total overwrite.
3. **Do not reinvent what exists — and do not pretend the OS audio stack serves the plugin path.** Note semantics are MIDI 2.0 UMP. Plugin hosting is VST3/AU/CLAP/AAX, wrapped by a shell. Audio into the plugin is the one transport HARP owns (§8.1), because no existing mechanism delivers device audio into a DAW that is already clocked to another interface.
4. **Diagnostics are a feature, not an afterthought.** Every conforming device counts its errors and can prove its health. "It glitched" must be answerable with evidence.
5. **Honesty about timing.** Latency is measured and reported, never guessed. Time correlation between host and device clock domains has stated uncertainty.
6. **Fail safe, fail loud, recover automatically.** Unplug, replug, sleep, wake, sample-rate change, and firmware mismatch are first-class protocol states with defined behavior, not edge cases.
7. **Vendor neutrality.** No vendor — including the specification's steward — receives privileged capability. Extension points are open and namespaced.

### 1.3 Non-goals

HARP does not define: a sound engine; a plugin API (it *targets* existing ones); digital rights management for sample content; a control-surface/HUI replacement; networked multi-room audio distribution (a network transport binding is reserved, §4.4, but Dante/AVB-class routing is out of scope); or inter-host song synchronization (Ableton Link and MIDI Clock remain the tools for that).

### 1.4 Relationship to existing standards

| Standard | Relationship |
|---|---|
| USB Audio Class 2/3 | Optional coexistence: a device MAY also present UAC2/3 functions for standalone/interface use (§8.5). The plugin path never depends on them. |
| MIDI 2.0 / UMP / MIDI-CI | Musical events (notes, expression, MPE) are carried as UMP packets inside timestamped HARP event frames (§9.10); the plugin-grade parameter/transport layer is HARP-native (§9.1). MIDI-CI Property Exchange MAY coexist; HARP's state model supersedes it for recall. |
| VST3, AU, CLAP, AAX | Targets. A HARP host runtime presents devices to DAWs through shells in these formats (§15). |
| CBOR (RFC 8949), CDDL (RFC 8610) | All control-plane and state encoding is CBOR; schemas are CDDL; hashed objects use Core Deterministic Encoding (§5.1, §10.2). |
| SHA-256 (FIPS 180-4), Ed25519 (RFC 8032) | State hashing and firmware signing respectively. |
| Ableton Link, MIDI Clock, OSC | Orthogonal; unaffected by HARP. |

A note on the obvious question — *why will this succeed where open standards usually stall*: the historical weakness of community protocols in this space is not specification, it is the unfunded grind of QA, signed installers, and DAW compatibility. HARP's governance model (§17, §19) is designed around that exact weakness: the specification and reference implementation are free and open; the **certification mark** is a controlled trademark whose licensing fees fund the test lab, the compatibility matrix, and the continuous QA that the open protocol needs to deserve user trust. The protocol is the commons; certification is the economic engine that maintains it.

---

## 2. Conformance and terminology

### 2.1 Requirement keywords

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**, **SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY**, and **OPTIONAL** in this document are to be interpreted as described in RFC 2119 and RFC 8174 when, and only when, they appear in all capitals.

### 2.2 Terms

**Device** — the hardware instrument, effect, or bridge implementing the device side of HARP.

**Host** — the computer-side implementation: the HARP runtime plus any plugin shells and applications built on it.

**Runtime** — the host-side service implementing discovery, transport, sessions, and the state store, shared by all shells and applications on that machine.

**Shell** — a plugin (VST3/AU/CLAP/AAX) or application presenting one HARP device to a DAW or user.

**Engine** — the sound-generating/processing core of a device. Engine version is distinct from firmware version (§6.2, §13.4).

**Object** — an immutable, content-addressed unit of state: `blob`, `list`, `tree`, or `snapshot` (§10).

**Ref** — a named, mutable pointer to an object hash, with a generation counter and dirty flag (§11.1).

**Snapshot** — an object capturing a complete state root at a point in time, with parentage (§10.1).

**Recall Bundle** — a serialized archive of refs plus objects sufficient to restore device state, embeddable in a DAW project (§15.3).

**MSC** — Monotonic Sample Counter, the device's master time base (§7).

**Session** — the negotiated association between one runtime and one device (§12).

**Plane** — one of the four concurrent channels of a session: control, state, events, audio (§3.2).

### 2.3 Conformance classes (overview)

Conformance is claimed per class; classes compose. Detailed requirements per class are consolidated in §17.

| Class | Summary |
|---|---|
| `harp-core` | Identity, discovery, control plane, time correlation, mandatory diagnostics counters. Prerequisite for all others. |
| `harp-recall` | Content-addressed state model, refs, safe synchronization, atomic apply, snapshot-on-demand. |
| `harp-stream` | Dedicated device↔plugin audio streaming with in-band timestamps — the plugin path (§8). |
| `harp-class-audio` | Optional coexisting class-compliant audio (UAC2/3) for standalone/interface use (§8.5). |
| `harp-perf` | Timestamped events with sample-accurate application (requires `harp-stream`). |
| `harp-fw` | In-protocol firmware management. |

A device claiming any class MUST implement `harp-core`. **HARP Certified** (§17.3) is a separate, trademark-controlled attestation that a specific device/firmware/host combination has passed the certification suite.

---

## 3. Architecture

### 3.1 Layering

```
┌─────────────────────────────────────────────────────────────┐
│  DAW / standalone app                                       │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Plugin shell (VST3 / AU / CLAP / AAX)  — §15        │   │
│  └──────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  HARP host runtime — sessions, state store,          │   │
│  │  discovery, recovery, diagnostics aggregation        │   │
│  └──────────────────────────────────────────────────────┘   │
│      control │ state │ events        audio                  │
│  ┌───────────┴───────┴──────┐  ┌─────┴──────────────────┐   │
│  │  HARP framed link (§4.2) │  │ HARP stream (§8)       │   │
│  │  one bulk pair, streams  │  │ plugin audio path      │   │
│  └───────────┬──────────────┘  └─────┬──────────────────┘   │
│                          (optional UAC2/3 coexists, §8.5)   │
├──────────────┴───────────────────────┴──────────────────────┤
│  Transport binding: USB (normative, §4.3); TCP (reserved)   │
├──────────────────────────────────────────────────────────────┤
│  Device: HARP endpoint, state store, engine, front panel    │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 The four planes

A session comprises four logical planes with different delivery requirements:

| Plane | Carries | Properties |
|---|---|---|
| **Control** | Requests, responses, notifications | Reliable, ordered, low rate, small messages |
| **State** | Objects (patches, projects, samples), firmware images | Reliable, bulk, credit flow-controlled, MUST NOT starve control |
| **Events** | UMP packets, parameter events, transactions | Reliable, ordered, timestamped, low latency |
| **Audio** | PCM streams | Real-time; dedicated HARP stream into the shell (§8); never via the OS audio stack |

Control, state, and events multiplex over one framed link (§4.2). Audio uses dedicated transport resources (§8) so that a 200 MB sample upload can never glitch playback.

### 3.3 Ownership of truth

For *live* engine state, the device is the source of truth (front panels exist; hands turn knobs). For *saved* state, the host project is the source of truth. HARP's job is to make the boundary between these explicit: refs and generations say what the device holds; the Recall Bundle says what the project expects; synchronization (§11) reconciles them only through the four safe actions (§11.4).

---

## 4. Transport bindings and framing

### 4.1 Transport requirements

A transport binding MUST provide: (a) a reliable, ordered byte pipe in each direction for the framed link; (b) device enumeration with stable addressing for the duration of attachment; (c) detach notification to the host. It SHOULD provide a low-jitter path suitable for the HARP stream (§8.2) and a mechanism for pre-claim identification of HARP support.

### 4.2 The framed link

Control, state, and event planes share one bidirectional byte pipe, divided into frames:

```
HARP frame
  u8   fver      frame format version, = 0x01
  u8   stream    logical stream id
  u16  flags     bit 0: FIN (end of message)   bits 1–15: reserved, MUST be 0
  u32  length    payload byte count, little-endian, ≤ 65 536
  u8[] payload
```

Defined streams:

| id | stream | plane | notes |
|---|---|---|---|
| 0 | `ctl` | Control | CBOR envelopes (§5.2) |
| 1 | `evt` | Events | event frames (§9) |
| 2 | `obj` | State | object/firmware transfer, credit-controlled (§4.2.1) |
| 3 | `log` | Diagnostics | device log records (§14.4) |
| 4–127 | — | — | reserved for future standardization |
| 128–255 | — | — | vendor experimental; MUST NOT be required for conformant operation |

Messages larger than one frame are split across consecutive frames of the same stream; FIN marks the last. Frames of different streams MAY interleave; frames within one stream MUST NOT. Receivers MUST treat malformed frames (bad `fver`, oversize `length`, reserved flags set) as fatal to the session and reset (§12.4).

#### 4.2.1 Flow control

Streams `ctl`, `evt`, and `log` are bounded by design: implementations MUST keep individual messages on these streams ≤ 64 KiB and SHOULD keep `ctl`/`evt` messages ≤ 4 KiB. Stream `obj` carries arbitrarily large transfers and is credit-controlled: each side grants the other transmit credit in bytes via `core.credit` (§5.5); senders MUST NOT exceed granted credit. A sender with queued `ctl` or `evt` frames MUST schedule them ahead of queued `obj` frames.

### 4.3 USB binding (normative)

A HARP device on USB:

1. MUST present a vendor-specific interface (class `0xFF`, subclass `0x48` 'H', protocol `0x01`) with one bulk IN and one bulk OUT endpoint carrying the framed link. Max packet size SHOULD be the maximum for the negotiated bus speed.
2. MUST include a Binary Object Store (BOS) Platform Capability descriptor with UUID `b1f0c3a6-78a4-4e0e-9c5b-2d7a0e51c9d3` *(placeholder; final value assigned at first stable release — see Appendix C)*, carrying `bcdHARP` (protocol version) and the interface number of the framed link. This allows hosts to identify HARP support without claiming interfaces.
3. SHOULD use the device's USB VID/PID and serial-number string descriptor as its primary identity keys (§6.2).
4. If claiming `harp-stream`, MUST present the dedicated audio endpoints described in §8.2.
5. If claiming `harp-class-audio`, MUST present UAC2 or UAC3 audio function(s) in the same composite device (§8.5).

Devices MUST tolerate the host claiming the HARP interface without claiming audio interfaces, and vice versa. Devices MUST enumerate at USB 2.0 High Speed as a floor and SHOULD support SuperSpeed where channel counts warrant it.

### 4.4 TCP/IP binding (reserved)

A network binding (`_harp._tcp` via mDNS/DNS-SD, TLS 1.3 mandatory, same framed link) is reserved for a companion specification. Implementations MUST NOT ship a network binding under the `harp` identifier until that specification is stable, to protect the meaning of conformance claims.

---

## 5. Control plane

### 5.1 Encoding

All control messages are CBOR (RFC 8949). Schemas are given in CDDL (RFC 8610), consolidated in Appendix A. Wherever this specification requires hashing of an encoded structure, the structure MUST be encoded with RFC 8949 §4.2.1 Core Deterministic Encoding Requirements. Control messages that are not hashed MAY use non-deterministic encoding; receivers MUST accept both.

### 5.2 Envelope

```cddl
envelope = {
  0 => msgtype,        ; 0 request, 1 response, 2 error, 3 notification
  1 => uint,           ; request id (echoed in response/error; 0 for notifications)
  2 => tstr,           ; method, e.g. "state.refs"
  ? 3 => any,          ; body
}
msgtype = 0..3
```

Methods are namespaced strings. Namespaces `core`, `state`, `time`, `audio`, `evt`, `fw`, `diag` are standardized; `x.<vendor-domain>.*` (e.g. `x.example-instruments.scope`) is open for vendor extensions. Hosts MUST ignore unknown notifications and MUST NOT depend on vendor methods for any behavior required by a conformance class.

Requests are answered exactly once, with a response or an error, in any order (concurrent requests are permitted; `request id` correlates and is scoped to its sender's direction). Either side MAY send requests; the device-to-host direction is principally notifications.

### 5.3 Error model

```cddl
error-body = {
  0 => tstr,           ; code
  ? 1 => tstr,         ; human-readable message (English)
  ? 2 => any,          ; details
}
```

Standard codes: `unsupported`, `malformed`, `busy`, `denied`, `not-found`, `conflict` (CAS failure, §11.3), `incompatible` (version/engine mismatch), `storage` (device storage failure), `internal`. Vendor codes use the `x.` prefix. Errors MUST NOT be used where a defined success-with-status response exists.

### 5.4 Session establishment and version negotiation

After transport attach, the host sends `core.hello`:

```cddl
hello-req = { 0 => [uint, uint],            ; highest protocol [major, minor] supported
              1 => tstr,                    ; host runtime name/version (informative)
              ? 2 => [* tstr] }             ; host capability strings
hello-rsp = { 0 => [uint, uint],            ; protocol version selected by device
              1 => identity }               ; §6.2
```

The device selects the highest mutually supported `major` and replies; differing `minor` within a `major` is compatible by rule (§18). If no common `major` exists, the device replies with error `incompatible` including its supported range, and the host MUST surface a firmware/host-update prompt rather than failing silently. `core.hello` MUST complete before any other method; a device MUST reset all per-session state upon receiving it.

### 5.5 Core methods

| Method | Direction | Purpose |
|---|---|---|
| `core.hello` | H→D | establish session, negotiate version, fetch identity |
| `core.identify` | H→D | re-fetch identity (§6.2) without resetting session |
| `core.credit` | both (ntf) | grant `obj`-stream credit: `{0 => uint}` additional bytes |
| `core.ping` | both | liveness; echoes body |
| `core.bye` | H→D | orderly session end; device MAY release host-related locks |
| `core.changed` | D→H (ntf) | generic "re-query X" hint: `{0 => tstr}` topic, e.g. `"identity"` |

---

## 6. Identity, discovery, and capabilities

### 6.1 Discovery

On USB, hosts discover HARP devices by the BOS platform capability (§4.3) and MAY additionally probe by interface class triple. The runtime maintains the system-wide device list; shells consume it (§15.2). Devices are keyed by `(vendor id, product id, serial)`; a device whose serial is unavailable MUST be treated as a distinct unit per physical port, and certification requires a serial.

### 6.2 Identity descriptor

Returned in `core.hello`/`core.identify`:

```cddl
identity = {
  0  => vendor,          ; { 0 => uint (usb-vid or harp-vendor-id, App. C), 1 => tstr name }
  1  => product,         ; { 0 => uint id, 1 => tstr name }
  2  => tstr,            ; serial (stable, unique per unit)
  3  => semver,          ; firmware version
  4  => engine,          ; { 0 => tstr id, 1 => semver, 2 => hash param-map-hash }
  5  => [uint, uint],    ; protocol version in effect
  6  => [* tstr],        ; capability strings (conformance classes + features)
  7  => channel-map,     ; §6.3
  8  => latency-profile, ; §6.4
  ? 9  => tstr,          ; firmware build id (informative)
  ? 10 => uint,          ; boot count (diagnostics correlation)
}
semver = tstr            ; "MAJOR.MINOR.PATCH", SemVer 2.0.0
hash   = bstr .size 33   ; 1 algorithm byte (0x01 = SHA-256) + digest
```

Capability strings include conformance classes (`harp-core`, `harp-recall`, …) and granular features (`evt.ump`, `evt.param`, `state.autosnapshot`, `fw.ab-slots`, `diag.loopback.analog`, …). The complete registry is Appendix C. Hosts MUST treat absent capabilities as unsupported and degrade per §15.

The **engine** triple is central to recall validity. `engine.version` MUST change whenever device behavior that affects rendering of stored state changes — DSP algorithms, modulation ranges, voice allocation. Firmware MAY rev without an engine change (UI fixes, USB fixes). `param-map-hash` is the hash of the canonical parameter descriptor set (§9.2); hosts use it to validate automation lanes across firmware updates.

### 6.3 Channel map

```cddl
channel-map = [* channel]
channel = {
  0 => uint,             ; slot index within direction (see below)
  1 => uint,             ; direction: 0 = device→host, 1 = host→device
  2 => tstr,             ; name, e.g. "Track 3"
  ? 3 => tstr,           ; group, e.g. "Tracks"
  ? 4 => tstr,           ; kind: "main" / "track" / "cue" / "input" / "sidechain" / "return"
  ? 5 => bool,           ; host-paced capable (§8.3); default false
}
```

Slot indices are HARP stream slot numbers (§8.2), scoped per direction. When class-compliant audio coexists (§8.5), the correspondence between stream slots and the OS-visible UAC channels is declared via `audio.binding`. The channel map gives users *named* channels ("Track 3", not "Input 7/8") in every conforming host; devices MUST keep names stable across reboots and SHOULD keep them stable across firmware versions.

### 6.4 Latency profile

```cddl
latency-profile = [* {
  0 => uint,             ; sample rate, Hz
  1 => uint,             ; device input path latency, samples (analog-in → stream)
  2 => uint,             ; device output path latency, samples (stream → analog-out)
  ? 3 => uint,           ; engine processing latency, samples, if in audio path
}]
```

Reported figures are the device's *nominal, measured-at-design-time* values and MUST be accurate within ±1 ms; certification verifies them against loopback measurement (§14.3, §17.3). Runtimes compose these with stream depth and, in free-running mode, ASRC delay into a single constant DAW-domain figure for plugin delay compensation (§8.3), and MAY substitute loopback-measured totals (§14.3).

---

## 7. Time, clocking, and the Monotonic Sample Counter

### 7.1 The MSC

Every device maintains a **Monotonic Sample Counter**: a `u64` counting samples of the device master clock, never decreasing while powered, paired with a `u32` **epoch** that increments on any clock discontinuity (sample-rate change, clock-source change). The device time domain is the pair `(epoch, msc)`. Timestamps in this specification are always `(epoch, msc)` pairs; an event timestamped in a stale epoch MUST be discarded by the receiver and counted (`evt_stale_epoch`, §14.2).

On a rate or clock change the device MUST emit:

```
ntf time.epoch  { 0 => uint new-epoch, 1 => uint new-rate-hz,
                  2 => uint old-epoch, 3 => uint64 old-msc-final }
```

### 7.2 Host–device time correlation

`time.ping` (H→D) returns `{ 0 => (epoch, msc) at request receipt, 1 => (epoch, msc) at response transmit }`. The host timestamps its send and receive on a monotonic host clock and computes, NTP-style, an offset between host time and device `(epoch, msc)` with uncertainty `≤ (t_recv − t_send − device_turnaround)/2`. Runtimes MUST maintain a continuously refined correlation per session and expose it to shells with its current uncertainty. Over the USB binding, correlation uncertainty under 250 µs SHOULD be achievable and under 1 ms MUST be (verified under certification).

On the HARP stream, frame headers carry the stream timestamp directly (§8.2), anchoring the correlation exactly at the audio boundary; `harp-perf` sample-accuracy claims rest on this path.

### 7.3 Clock recovery and drift

Devices free-run their master clock unless synchronized externally — which means every free-running device attached to a host is its own clock domain, in addition to the DAW-interface domain the DAW is locked to. Runtimes MUST estimate drift between the DAW clock and each device's MSC continuously and expose it per device (`clock_drift_ppb`, §14.2). Reconciling the domains is the runtime's job under the clock-mode rules of §8.3; devices MUST NOT resample silently (a device MAY declare `audio.src` for explicit, host-enabled device-side conversion). Coexisting class-compliant audio (§8.5) inherits ordinary UAC clocking semantics and sits outside the plugin path.

---

## 8. Audio plane

### 8.1 The plugin path is not the OS audio path

A DAW's audio engine is bound to exactly one low-latency interface — the musician's main converter — and to that interface's clock. A plugin cannot open a second OS audio device without inheriting a second callback context, a second clock domain, and the aggregate-device failure modes this specification exists to abolish. Therefore:

1. Plugin-bound device audio MUST travel the HARP stream (§8.2), through the runtime, into the shell, and be produced/consumed inside the DAW's own processing context. It MUST NOT depend on the OS audio stack, on the device being selectable as an OS audio device, or on any aggregate-device construction.
2. Class-compliant audio is an OPTIONAL coexistence feature for standalone and interface use (§8.5) and MUST NOT be required for any plugin-path function.

`harp-stream` is therefore the audio class that carries the product promise; any device shipping a plugin experience claims it.

### 8.2 HARP stream transport (`harp-stream`)

Transport: a dedicated bulk IN and bulk OUT endpoint pair (USB binding), separate from the framed link, carrying only audio frames:

```
audio frame
  u8   fver        = 0x01
  u8   dirflags    bit 0: direction (0 D→H, 1 H→D); bit 1: discontinuity follows; rest 0
  u16  slots       channel count in this frame
  u32  epoch
  u64  ts          stream timestamp of first sample — device MSC (free-running)
                   or host SSI (host-paced), per §8.3
  u16  nsamples    samples per channel (per-frame block size)
  u16  fmt         0x0001 = float32 LE; 0x0002 = int24 LE packed; others reserved
  payload          interleaved by slot index, nsamples × slots
```

Requirements: devices MUST support `float32`; `nsamples` MUST be constant within an epoch and is negotiated at `audio.start`; D→H frames MUST be emitted in `ts` order without gaps except where the discontinuity bit is set (and counted); H→D frames carry the `ts` at which the device shall present (free-running) or render against (host-paced) the first sample, and frames arriving past their time are discarded and counted (`audio_late_frames`). The device reports instantaneous buffer depth in `diag.counters` so the host's pacing loop is closed.

```
req audio.start → { 0 => uint rate-hz, 1 => uint nsamples, 2 => uint target-depth-frames,
                    3 => [* uint] active-slots-in, 4 => [* uint] active-slots-out,
                    ? 5 => uint clock-mode }        ; 0 free-running (default), 1 host-paced
rsp             → { 0 => uint clock-mode-in-effect,
                    1 => uint device-pipeline-samples }   ; fixed engine+transport pipeline
req audio.stop
```

Bandwidth (informative): 16 channels of float32 at 48 kHz is ≈ 3.1 MB/s per direction per device; 64 channels at 96 kHz is ≈ 24.6 MB/s — comfortable on USB 3.x and feasible at High Speed for moderate counts. Aggregate budgeting across many devices is the runtime's job (§8.4). Bulk transfer is chosen over isochronous deliberately: retransmission preserves integrity, and the elastic buffering that clock-domain crossing requires anyway (§8.3) absorbs scheduling jitter.

### 8.3 Clock domains and delivery into the DAW

The stream runs in one of two clock modes, selected at `audio.start`; per-channel eligibility for host-paced operation is declared in the channel map (§6.3).

**Free-running (mode 0, default; REQUIRED wherever analog converters are in the streamed path).** The device master clock owns the stream; timestamps are device `(epoch, msc)`. The runtime MUST recover the device clock continuously (§7.3), maintain a fixed-depth elastic buffer, and resample asynchronously into the DAW clock domain — *including when nominal rates match*, because independent crystals always drift; "both at 48 kHz" is two clocks, not one. ASRC quality floors: passband ripple ≤ 0.01 dB, stopband attenuation ≥ 120 dB, and rate corrections slew-limited below audibility. Path latency MUST be constant for the session and reported in DAW-domain samples for plugin delay compensation. Buffer re-anchoring after starvation MUST be surfaced as an event and counted (`stream_reanchors`, host section of the diagnostic bundle) — never silent.

**Host-paced (mode 1, OPTIONAL; capability `audio.host-paced`; pure-digital paths only).** The host owns the stream index: `ts` is a host-defined **Stream Sample Index (SSI)** in the DAW's clock domain (the device's internal MSC remains available for diagnostics). H→D frames pace production: the device MUST produce output for exactly the sample ranges the host supplies as input, and channels with no input are paced by frames with `slots = 0`. All time-dependent engine behavior — LFOs, envelopes, sequencers, delays — MUST derive from the SSI, not wall-clock time. The result is a hardware engine that behaves like a plugin: no ASRC, exact sample alignment, and a fixed, deterministic latency (`device-pipeline-samples`).

Host-paced mode admits two further OPTIONAL capabilities: `audio.deterministic` — byte-identical output for identical state, event schedule, and SSI range, certifiable under T15 — and `audio.offline-rate` — willingness to be paced faster or slower than real time, which turns offline bounce *through hardware* into an ordinary host feature rather than a stunt.

Pipelining note (informative): host-paced does not mean synchronous RPC per DAW callback. The runtime keeps the device a small, fixed number of blocks ahead; the mode constrains *what* the device renders, while modest pipelining covers transport scheduling. Typical added latency is two to four blocks, constant and reported.

### 8.4 Many devices at once

Multi-device operation is a design center, not an afterthought:

1. The runtime MUST support at least 8 concurrent device sessions and SHOULD be limited only by transport bandwidth. Each session carries its own time correlation and, when free-running, its own independent ASRC instance — N boxes means N clock domains, reconciled per device, never via a shared aggregate.
2. **Admission control.** Before honoring `audio.start`, the runtime MUST verify that the requested channels fit the transport path — controller and hub chain, shared with the device's other planes and with every other active session — and MUST refuse explicitly, with the computed budget, rather than degrade silently. Per-controller utilization appears in the diagnostic bundle.
3. **Timeline alignment.** Per-device time correlation (§7.2) places every stream on the host timeline within stated uncertainty, so multitrack recordings from several free-running boxes land aligned within that bound; host-paced devices land exactly.
4. Topology guidance (informative): a High-Speed controller sustains roughly 30 MB/s of practical bulk throughput — several 16-channel devices — beyond which SuperSpeed devices or distribution across controllers is the answer. The runtime SHOULD surface placement advice ("this device shares a hub with X") in diagnostics rather than leaving users to cable roulette.

### 8.5 Class-compliant coexistence (`harp-class-audio`)

A device MAY additionally present standard UAC2/UAC3 functions in the same composite device, for standalone operation and ordinary interface duty. Rules: the device MUST declare, per channel, whether it can be active on both paths simultaneously (`audio.dual-path`; the default is exclusive — first-active wins, the other path reads silence, and the contention is counted); sample-rate coupling between the paths MUST be declared; and the device MUST support correlation of the OS-visible audio device with the HARP session:

```
req audio.binding → { 0 => tstr,    ; "uac2" | "uac3"
                      1 => tstr }   ; serial / unique id as presented to the OS audio stack
```

Nothing in a HARP session may alter standalone class-audio behavior except as declared. Coexistence exists so one box can be the user's interface at the kitchen table and a recallable plugin in the studio — not so the plugin path can lean on it.

### 8.6 Sample-rate changes

All rate changes pass through `audio.stop` → `time.epoch` notification → `audio.start`. In free-running mode the device clock changes and a new epoch begins; in host-paced mode the rate is simply the host's new pacing rate. Devices MUST preserve all state across rate changes; certification stresses the transition under load (T4).

---

## 9. Events, parameters, and automation

### 9.1 Division of labor — and why UMP alone is not enough

MIDI 2.0 UMP is the right carrier for *musical* events: notes with 16-bit velocity, per-note controllers and pitch, MPE, 32-bit controllers. Resolution is not the gap — 32 bits is plenty. The gap is semantic: UMP cannot express the contract a plugin enjoys with its host. It has no normalized parameter model with host-visible descriptors, no scheduled ramps, no notion of modulation as distinct from automation, no output parameters, and no block-synchronous transport/tempo/musical-position feed — 24 ppqn MIDI clock is not VST3's `ProcessContext`. HARP events carry exactly that layer natively; UMP rides inside them for the music. Coverage against the plugin APIs:

| plugin-API capability (VST3 / AU / CLAP) | HARP mechanism |
|---|---|
| normalized continuous parameters with descriptors, units, ranges | parameter descriptors (§9.3) |
| sample-accurate automation points | `param` events (§9.4) |
| scheduled ramped automation (AU `kParameterEvent_Ramped`) | `ramp` events (§9.4) |
| non-destructive modulation (CLAP `mod`) | `mod` events (§9.4) |
| per-voice modulation / note expression | per-voice addressing (§9.5) + UMP per-note controllers |
| notes, MPE, pitch, expression | UMP carriage (§9.10) |
| value↔string (`toString` / `fromString`) | mapping/enum descriptors + `evt.format` / `evt.parse` (§9.8) |
| output parameters, meters | readonly parameters streamed via echo (§9.9) |
| transport, tempo, bars/beats, loop (`ProcessContext`) | `transport` events (§9.7) |
| bypass | descriptor flag (§9.3) |
| program lists / preset browsing | state-model refs (§10–§11) |
| state save/restore (`getState`/`setState`) | Recall Bundle (§15.3) — stronger than any plugin API offers |

A shell mapping a HARP device into VST3, AU, or CLAP therefore loses nothing the format can express, and gains recall semantics none of them have.

### 9.2 Event frames

The `evt` stream carries timestamped event messages, each a CBOR array:

```cddl
event = [ (epoch: uint, msc: uint64),    ; presentation time; (0,0) = "now"
          etype: uint, body: any ]
etype: 0 = ump, 1 = param, 2 = txn-begin, 3 = txn-commit, 4 = txn-abort,
       5 = ramp, 6 = mod, 7 = transport
```

Receivers apply events at the stated timestamp. Devices claiming `harp-perf` MUST apply events within ±1 sample of the stated time when it is ≥ 1 ms in the future at receipt; late events are applied immediately and counted (`evt_late`). Hosts SHOULD schedule one audio block ahead. In host-paced mode (§8.3) timestamps are SSIs and therefore already live in the DAW's sample domain — sample-accurate automation by construction.

Bandwidth: devices declaring `evt.param` MUST sustain ≥ 2 000 events/s aggregate and declare their actual sustained rate (§9.3). Event-plane overload MUST never glitch audio: events go late and are counted; audio does not.

### 9.3 Parameter descriptors

Devices claiming `evt.param` expose their parameter set:

```
req evt.params → params
```

```cddl
params = { 0 => hash,                     ; param-map-hash (== identity engine field)
           1 => [* param],
           2 => uint,                     ; control rate, Hz (interpolation tick; SHOULD ≥ 1000)
           ? 3 => uint }                  ; max sustained event rate, events/s
param  = { 0 => uint,                     ; stable param id (u32)
           1 => tstr,                     ; name, e.g. "Filter Cutoff"
           ? 2 => tstr,                   ; group/path, e.g. "Track 1/Filter"
           ? 3 => tstr,                   ; unit, e.g. "Hz", "dB", "%"
           ? 4 => [float, float],         ; display range mapped from normalized [0,1]
           ? 5 => uint,                   ; step count (0 = continuous)
           ? 6 => tstr,                   ; curve hint: "lin" | "log" | "exp"
                                          ; (key 7 reserved)
           ? 8 => uint,                   ; flags: bit 0 automatable, 1 readonly (output),
                                          ;   2 hidden, 3 bypass, 4 periodic/wrap,
                                          ;   5 modulatable, 6 per-voice modulatable
           ? 9 => [* tstr],               ; enum labels (count == steps)
           ? 10 => [* [float32, float]],  ; piecewise-linear normalized→display map
           ? 11 => uint }                 ; meter rate hint, Hz (readonly params)
```

`param-map-hash` is the SHA-256 of the deterministic encoding of the full `param` array, flags and maps included. It MUST change iff the set changes in a way that invalidates stored automation (id meaning, range, removal). Shells persist it with projects; on mismatch at load, the shell MUST warn and map conservatively (matching ids only). Stability of parameter ids across firmware revisions is a SHOULD with teeth: breaking it without an engine major bump fails certification.

### 9.4 Parameter events: set, ramp, modulate

```cddl
param-event = { 0 => uint param-id, 1 => float32 normalized,    ; [0,1] — point set
                ? 2 => int raw, ? 3 => uint voice, ? 4 => uint txn-id }
ramp-event  = { 0 => uint param-id, 1 => float32 target,
                2 => tstamp end,                                 ; reach target at end
                ? 3 => uint voice, ? 4 => uint txn-id }
mod-event   = { 0 => uint param-id, 1 => float32 offset,        ; signed, normalized
                ? 2 => uint voice, ? 3 => uint txn-id }
```

**Set** applies a value at its timestamp. **Ramp** interpolates linearly in normalized space from the value in effect at the event's timestamp to `target` at `end`; the device interpolates at no less than its declared control rate, which is what makes automation zipper-free *and* cheap — a DAW curve becomes a handful of ramps per block instead of a point per tick. A new set or ramp on the same (param, voice) supersedes any ramp in flight. Devices claiming `evt.param` MUST implement ramps.

**Mod** (capability `evt.param.mod`) sets the current additive modulation offset on top of the base value, clamped after summation; it MUST NOT alter the stored base value and MUST NOT appear in base-value echo. This is CLAP's automation/modulation split, and it maps naturally onto hardware mod-matrix thinking: the knob position is sacred; modulation breathes around it. Devices SHOULD smooth offset changes at control rate.

**Echo**: devices MUST emit `param` events for front-panel and internally-driven base-value changes (`evt.param.echo`, REQUIRED for `harp-recall`) so shells reflect knob movements and record automation, timestamped by the device.

### 9.5 Per-voice addressing

The optional `voice` field is a packed uint aligned with UMP addressing: `(group << 12) | (channel << 8) | note`. Per-voice events affect only the addressed sounding voice; events addressing voices no longer sounding MAY be ignored. Eligibility is per-parameter (flags bit 6). Together with UMP per-note controllers this covers VST3 note expression and CLAP polyphonic modulation.

### 9.6 Transactions

Transactions group events for atomic application (kit loads, morph targets): `txn-begin {txn-id}` … events tagged with `txn-id` … `txn-commit {txn-id, tstamp}` applies all at one instant; `txn-abort` discards. Devices MUST bound open transactions (≥ 1 concurrent, ≥ 256 events) and report limits in capabilities.

### 9.7 Transport, tempo, and musical time

The largest single gap between MIDI and the plugin world is the host context feed. Devices declaring `evt.transport` receive it:

```cddl
transport-event = {
  0 => uint,                ; flags: bit 0 playing, 1 recording, 2 loop active,
                            ;   3 tempo valid, 4 timesig valid, 5 position valid
  ? 1 => float64,           ; tempo, BPM — constant until superseded
  ? 2 => [uint, uint],      ; time signature
  ? 3 => uint64,            ; song position in samples at this event's timestamp
  ? 4 => float64,           ; song position, PPQ, at this event's timestamp
  ? 5 => [float64, float64],; loop region, PPQ
  ? 6 => float64 }          ; PPQ of current bar start
```

Hosts feeding a declaring device MUST send a transport event on every change, on every locate or loop wrap (with position fields anchored at the jump's timestamp), and as a refresh at ≥ 1 Hz while playing. The mapping (timestamp, PPQ, tempo) defines musical time linearly until the next event, so a device sequencer derives bar/beat sample-exactly rather than phase-locking to 24 ppqn ticks. Under `harp-perf`, sequencers MUST realign at jumps within ±1 sample in host-paced mode and within the stated correlation bound in free-running mode. MIDI clock remains available over UMP for legacy chains; HARP-attached gear should never need it.

### 9.8 Value formatting

Hosts render values locally from range, curve, piecewise map, and enum labels — descriptor-driven display costs no round trip. For mappings a descriptor cannot express, devices SHOULD implement `evt.format { 0 => param-id, 1 => float32 } → { 0 => tstr }` and `evt.parse { 0 => param-id, 1 => tstr } → { 0 => float32 }`, the equivalents of `toString`/`fromString`; hosts use them as fallback, never per-frame.

### 9.9 Output parameters

Parameters flagged readonly are outputs: meters, gain reduction, voice counts, sequencer step position. The device streams them via echo at no more than the descriptor's meter rate hint; hosts present them in UI, MUST NOT offer them as automation targets, and MUST NOT record them as automation. This is VST3's read-only/`kIsReadOnly` and CLAP's readonly parameters — plugin-grade metering with zero host-side audio analysis.

### 9.10 UMP carriage

`etype 0` bodies are byte strings containing one Universal MIDI Packet (32/64/96/128 bits) verbatim. Devices declare `evt.ump` with a group map in capabilities. HARP does not redefine any UMP semantics; MIDI 2.0 per-note controllers, MPE via UMP, and SysEx all pass through. A device MAY expose the same controls both as UMP controllers and HARP params; if so it MUST declare the mapping in the param descriptor (`x-ump` extension key) so hosts avoid double application.

---

## 10. State model

This and §11 are the heart of HARP. The model is deliberately close to Git and to copy-on-write filesystems: immutable, content-addressed objects; cheap snapshots; named mutable refs updated only by compare-and-swap. The intent expressed as a slogan: **state synchronization should feel like Git, not like SysEx.**

### 10.1 Objects

Four object kinds, each an immutable CBOR structure identified by its hash:

```cddl
object   = blob / list / tree / snapshot

blob     = { 0 => 0, 1 => tstr,  2 => bstr }
           ; kind=0, media-type (e.g. "application/x.example.patch"), payload ≤ 16 MiB

list     = { 0 => 1, 1 => tstr,  2 => [* hash] }
           ; kind=1, media-type of the concatenation, ordered chunk hashes
           ; (large content: samples, firmware payloads)

tree     = { 0 => 2, 1 => { * tstr => entry } }
           ; kind=2, name → entry
entry    = [ hash, uint ]      ; referenced object hash, kind of referenced object

snapshot = { 0 => 3,
             1 => hash,        ; root tree
             2 => [* hash],    ; parent snapshot(s); empty for initial
             3 => uint,        ; unix time, seconds (device clock; informative)
             4 => tstr,        ; author: "device" | "host:<runtime name>" | free text
             5 => semver,      ; engine version that produced this state
             ? 6 => tstr }     ; message
```

### 10.2 Hashing

`hash` is one algorithm byte followed by the digest; algorithm `0x01` = SHA-256 over the object's RFC 8949 §4.2.1 deterministic encoding. SHA-256 is the only algorithm in protocol major 1; the byte exists for future agility, and implementations MUST reject unknown algorithm bytes rather than guessing. Devices MAY use hardware SHA engines; a mid-range MCU hashes a full device state in milliseconds, and devices SHOULD cache object hashes rather than rehash on query.

Content addressing yields the properties recall needs for free: identical state has an identical hash regardless of which side produced it; duplication is pointer assignment; verification after transfer is intrinsic; and a project's expected state can be compared to a device's actual state by comparing 33 bytes.

### 10.3 State roots (refs)

A **ref** is a named pointer:

```cddl
ref = { 0 => tstr,           ; name
        1 => hash / null,    ; current target (null = unborn)
        2 => uint64,         ; generation: increments on ANY change to the named state,
                             ;   including unsaved live edits
        3 => bool }          ; dirty: live engine state has diverged from target hash
```

Standard ref namespace (devices expose those that apply):

| ref | meaning |
|---|---|
| `live/project` | the complete currently-loaded project/kit/performance state |
| `bank/<id>` | stored banks/slots, device-defined ids |
| `lib/samples` | sample/wavetable library root |
| `sys/settings` | global device settings that affect sound or recall |
| `archive/*` | free area for host-created safety snapshots (§11.4) |

`live/project` is REQUIRED for `harp-recall` and MUST cover *all* state needed to reproduce the device's sonic behavior except `lib/samples` content referenced by hash and `sys/settings`. The dirty flag plus generation counter is how hosts detect front-panel edits: any knob turn on stored state sets `dirty=true` and bumps `generation`, and the device MUST notify:

```
ntf state.changed { 0 => tstr ref-name, 1 => hash/null, 2 => uint64 generation, 3 => bool dirty }
```

Notification rate MAY be coalesced (≥ 2 Hz under continuous editing is sufficient); the terminal state after edits stop MUST always be notified.

### 10.4 Snapshot-on-demand

`harp-recall` devices MUST implement:

```
req state.snapshot { 0 => tstr ref-name, ? 1 => tstr message }
rsp                { 0 => hash snapshot-hash, 1 => uint64 generation }
```

The device serializes current live state of the named ref into objects, creates a `snapshot` whose parent is the ref's previous target, points the ref at it, clears `dirty`. This is the primitive that makes *pull* possible mid-edit and makes "duplicate before overwrite" honest. Devices SHOULD complete a full-project snapshot in ≤ 2 s and MUST NOT glitch audio while doing so.

---

## 11. State synchronization

### 11.1 Inventory

```
req state.refs  → { 0 => [* ref] }
req state.have  { 0 => [* hash] } → { 0 => [* bool] }     ; per-hash possession
```

### 11.2 Transfer

Object transfer runs on stream `obj` as a CBOR sequence of `object` items, preceded on `ctl` by intent:

```
req state.want { 0 => [* hash] }                ; ask peer to send objects (recursive
                                                ;   closure NOT implied; requester walks)
req state.send { 0 => [* hash], 1 => uint }     ; announce push: hashes + total bytes
```

Receivers MUST verify each object's hash on receipt and MUST discard non-verifying objects with error `malformed`. Both sides maintain object stores; devices MUST retain all objects reachable from any ref and MAY garbage-collect unreachable objects at will (hosts re-send on demand). Requesters walk trees: fetch root, then `state.want` children not already held — the have/want dance keeps re-sync after small edits proportional to the diff, not the state size.

### 11.3 Ref update — compare-and-swap

The only way to change device state from the host:

```
req state.refset { 0 => tstr ref-name,
                   1 => hash / null,     ; expect: required current target
                   2 => hash,            ; new target (objects must be fully present)
                   ? 3 => uint flags }   ; bit 0 create-if-unborn; bit 1 force (see below)
rsp              { 0 => uint64 new-generation }
err conflict     { details: { 0 => hash/null actual, 1 => uint64 generation, 2 => bool dirty } }
```

Semantics: the device atomically verifies that the ref's current target equals `expect` **and** `dirty == false`; on success it loads the new target into the live engine (for `live/*` refs) or storage and replies; otherwise it replies `conflict` with the actual state and changes nothing. The `force` flag overrides both checks; hosts MUST NOT set it except on an explicit, informed user action, and MUST archive the displaced state first (§11.4). CAS is the entire concurrency story: a knob turned between the host's read and write surfaces as `conflict`, never as silent loss.

**Atomic apply.** Activation of a new target MUST be transactional with respect to power loss and error: the device validates the full object closure, stages, then commits via a journaled step such that interruption at any point leaves either the old or the new state fully intact — never a hybrid, never corruption. Activation of a typical project SHOULD complete in ≤ 2 s; the device MAY mute outputs during the swap but MUST NOT emit unbounded transients. Certification T10 power-cycles devices mid-apply, repeatedly.

### 11.4 The four safe actions

When a shell holding saved state attaches to a device and finds `live/project` differing from expectation (different hash, or dirty), conforming hosts MUST offer exactly these resolutions, and no destructive default:

1. **Push** — host → device. The host first archives the device's current state: `state.snapshot` if dirty, then `state.refset archive/<timestamp>` pointing at it (O(1) — content addressing makes the duplicate a pointer), then transfers missing objects and CAS-updates `live/project`.
2. **Pull** — device → host. `state.snapshot` if dirty, fetch closure, update the project's Recall Bundle.
3. **Open read-only** — no writes; the shell subscribes to `state.changed` and `evt.param.echo`, displays live state, disables automation *write*, and marks the session read-only in UI.
4. **Duplicate, then push** — as Push, but the archive ref is presented to the user as a named project copy on the device (device-visible, not merely host-side).

Hosts MUST NOT auto-resolve mismatches without a persisted user preference, and even with one, Push MUST always perform the archive step. The rationale is the protocol's founding asymmetry: *recall errors are recoverable only if every overwrite is preceded by a free snapshot* — content addressing makes the snapshot free, so the specification spends it every time.

### 11.5 Sample and large-content sync

`lib/samples` follows the same model; `list` objects chunk large payloads (RECOMMENDED chunk size 1 MiB) so partial transfers resume by skipping already-held chunks (`state.have`). Devices MUST report storage capacity and free space in `diag.counters` so hosts can pre-check pushes; pushes exceeding free space fail with `storage` *before* transfer begins, based on the `state.send` byte announcement.

---

## 12. Session lifecycle and recovery

### 12.1 Session states

```
DETACHED → ATTACHED → NEGOTIATED → SYNCED → STREAMING
     ↑         |            |          |         |
     └─────────┴────────────┴──────────┴─────────┘   (detach / error → DETACHED)
```

`ATTACHED`: transport up. `NEGOTIATED`: `core.hello` complete. `SYNCED`: shell has reconciled state (§11.4 if needed). `STREAMING`: audio running. Runtimes MUST expose the state machine to shells and surface transitions in diagnostics.

### 12.2 The canonical project-open flow

This sequence is normative for shells claiming recall:

```
DAW opens project
  shell loads Recall Bundle: identity expectation + refs + objects (§15.3)
  runtime: device present?
    no  → shell renders offline view from bundle; waits; subscribes to discovery
    yes → core.hello → identity
  shell compares identity:
    serial differs        → "different unit" flow: offer push-to-new-unit (with archive) or read-only
    engine.major differs  → MUST default to read-only; offer push only with explicit
                            "engine versions differ; sound may change" consent
    engine.minor/patch    → proceed; note in session log
    param-map-hash differs→ warn; conservative automation mapping (§9.2)
  shell reads state.refs:
    live/project hash == bundle hash and !dirty → SYNCED, silently
    else → present the four safe actions (§11.4)
  shell starts audio: audio.start on the HARP stream — never the OS audio stack;
                      coexisting class audio, if any, is untouched
  shell applies latency profile / measured latency to PDC
  → STREAMING
```

The silent path is the common path: matched hash means *zero dialogs*. Every dialog in HARP is the system refusing to guess.

### 12.3 Unplug, replug, sleep, wake

On detach during any state, runtimes MUST: keep the session object alive for ≥ 60 s awaiting the same `(vendor, product, serial)`; on reattach, re-run `core.hello`, re-verify refs, and resume — including restarting audio and re-arming subscriptions — without shell or DAW restart. Shells MUST render a defined "device offline" state rather than erroring. Devices MUST treat host sleep (bus suspend) as detach-equivalent and MUST NOT lose state across it. Certification T2/T3 exercise both, hundreds of times, during streaming.

### 12.4 Session reset

On framing violations, hash verification failure on `ctl`, or negotiated-invariant violations, either side MUST reset: device returns to pre-hello state; runtime re-establishes from `core.hello`. Resets are counted (`session_resets`) and logged with cause; more than zero unexplained resets in a 24 h soak fails certification (T8).

---

## 13. Firmware management (`harp-fw`)

### 13.1 Model

```
req fw.manifest → { 0 => [* slot], 1 => uint active-slot }
slot = { 0 => uint id, 1 => semver fw, 2 => semver engine, 3 => hash image-hash,
         4 => uint state }   ; 0 empty, 1 valid, 2 staged, 3 active, 4 failed
```

A/B slots are RECOMMENDED; single-slot devices MUST still satisfy the power-loss rule below.

### 13.2 Update

Firmware images travel as `list`-chunked objects on stream `obj` (resumable via `state.have`), then:

```
req fw.stage  { 0 => uint slot, 1 => hash image-hash, 2 => bstr manifest-sig }
req fw.commit { 0 => uint slot }     ; reboot into staged slot
req fw.revert {}                     ; reboot into previous slot (A/B)
```

Devices MUST verify the vendor signature (Ed25519 RECOMMENDED) over the image manifest before `stage` succeeds, MUST verify image hash before boot, and MUST remain bootable into a valid image if power is lost at any point in the process. Hosts MUST NOT initiate updates without explicit user action.

### 13.3 Compatibility surface

`fw.manifest` plus identity gives hosts everything needed to gate features by version. Hosts MUST NOT fingerprint behavior by version string for anything covered by a capability flag — capabilities are the truth; versions are for humans and for engine-recall gating.

### 13.4 Engine version and recall validity

The contract restated as requirements: state objects record the `engine` version that produced them (snapshot field 5). A device asked to load a snapshot whose engine **major** differs from its own MUST refuse with `incompatible` unless the request carries the explicit-consent flag, in which case it MAY migrate (declaring `state.migrate` capability) or load best-effort with a `state.changed` note. Engine **minor** differences MUST load exactly or refuse — "loads but sounds different" without consent is the single outcome this specification exists to prevent.

---

## 14. Diagnostics and observability

Diagnostics are a conformance requirement, not a vendor courtesy. The support reality this addresses: users blame "the bridge" for every cable, hub, firmware, and buffer problem; the only durable answer is evidence.

### 14.1 Principles

Devices count everything abnormal; counters are cheap, permanent, and queryable. Hosts aggregate device counters with their own and can export a single bundle a support thread can act on.

### 14.2 Mandatory counters (`harp-core`)

`req diag.counters → { tstr => uint/int }`, monotonic since boot, including at minimum:

| counter | meaning |
|---|---|
| `usb_errors` | transport-level errors seen by device |
| `frame_errors` | framed-link violations received |
| `audio_underruns` / `audio_overruns` | engine-side stream starvation / overflow |
| `audio_late_frames` | H→D stream frames past their timestamp |
| `msc_discontinuities` | unplanned MSC gaps |
| `clock_drift_ppb` | current estimate vs declared rate (gauge) |
| `evt_late` / `evt_stale_epoch` | events applied late / discarded |
| `session_resets` | §12.4 |
| `storage_bytes_total` / `storage_bytes_free` | state store capacity |
| `temperature_centi_c` | SHOULD, if sensed |

Vendor counters use `x.` prefixes. `diag.subscribe` streams deltas at a host-chosen interval.

### 14.3 Loopback latency test

Devices declaring `diag.loopback.analog` and/or `diag.loopback.digital`:

```
req diag.loopback.start { 0 => uint in-slot, 1 => uint out-slot, 2 => tstr mode }
req diag.loopback.stop
```

In digital mode the device routes the host's output stream back as input with no analog stage; in analog mode the user patches a cable and the path includes converters. The host emits a measurement signal, computes round-trip in samples, subtracts known transport buffering, and compares against the latency profile (§6.4). Shells SHOULD offer one-click measurement and persist results per device/rate; measured values, when present, take precedence for PDC.

### 14.4 Logs and the diagnostic bundle

Stream `log` carries structured records `{ 0 => uint64 msc-or-0, 1 => uint level, 2 => tstr tag, 3 => tstr msg }` from a device ring buffer (≥ 64 KiB RECOMMENDED). `req diag.bundle` triggers a full dump; the runtime then assembles the **HARP Diagnostic Bundle** — a CBOR file containing: identity, capabilities, counters (device + host), session state-machine history, recent logs (device + runtime), audio configuration, USB topology as visible to the host (controller, hub chain), correlation/drift statistics, and an optional anonymization pass that strips serials and names. The bundle schema is normative (Appendix A) so vendor support tooling can rely on it. The user-facing promise: *one button produces the file that ends the guessing*.

---

## 15. Host runtime and plugin shell requirements

### 15.1 Runtime

One runtime per machine owns transports, discovery, sessions, time correlation, the host object store, and diagnostics. It MUST support multiple simultaneous devices and multiple shells, MUST survive shell crashes without dropping device sessions, and SHOULD run user-space (no kernel components beyond OS-provided class drivers; Windows runtimes MAY additionally expose HARP stream devices through an ASIO driver for non-plugin use, outside this specification's scope). The runtime–shell IPC is implementation-defined; the *behavior* contract here is what conformance tests observe. For audio, the runtime owns a real-time transport path per device and delivers samples to shells over lock-free shared memory; the shell's audio-thread contract is hard — no blocking calls, no allocation, constant reported latency — and starvation at any boundary is counted and surfaced, never absorbed silently. The runtime performs the admission control of §8.4 on behalf of all shells.

### 15.2 Arbitration

Exactly one shell at a time holds **write** access to a device's state and events (the *controller*); any number MAY hold read access (meters, librarians). The runtime arbitrates; controller handoff requires the new shell's explicit request and MUST be surfaced in both shells' UI. Audio streams are independent of controllership (coexisting class audio is OS-arbitrated as usual).

### 15.3 The Recall Bundle

What a shell persists inside the DAW project (or as a sidecar, format identical):

```cddl
recall-bundle = { 0 => tstr "harpb", 1 => uint version,
                  2 => identity-expectation,        ; vendor, product, serial, engine, param-map-hash
                  3 => [* ref],                     ; expected refs (names + hashes)
                  4 => [* object] / null,           ; embedded object closure, or null if external
                  ? 5 => latency-measured,
                  ? 6 => tstr }                     ; user note
```

Shells MUST embed the full closure of `live/project` (typical sizes are small; patches are kilobytes) and SHOULD embed `sys/settings`. For `lib/samples`, shells embed hashes always and content optionally (user setting; sample libraries can be gigabytes) — a project with hash-only samples reopens with verification and a "fetch from device / locate library" flow rather than blind trust. Because bundles are content-addressed, a project moved to another machine, or another *unit* of the same model, restores exactly: that is the portability story, and it falls out of §10 with no extra machinery.

### 15.4 Shell behavior

Shells MUST: implement §12.2 verbatim; expose device parameters as host automation using descriptors (§9.2) with `param-map-hash` validation; record `evt.param.echo` as automation when armed; apply PDC from §6.4/§14.3; render offline state from the bundle when the device is absent; and present the four safe actions UI per §11.4. Shells MUST NOT write device state outside `state.refset` semantics — no SysEx side doors for anything HARP covers.

### 15.5 Offline editing

OPTIONAL capability `host.offline-edit`: a shell MAY allow editing the bundle's state while the device is absent, producing new objects host-side; on reattach this is simply a mismatch resolved by Push. No special protocol is needed — another deliberate consequence of content addressing.

---

## 16. Security considerations

**Firmware** is the highest-value target: signature verification (§13.2) is mandatory for certification, and devices SHOULD support rollback-protection policy declared in capabilities.

**Parsing**: devices face hostile USB hosts; all CBOR and frame parsing MUST be bounds-checked, allocation-bounded, and fuzz-tested (certification T9 fuzzes `ctl`, `evt`, and `obj` inputs; a crash or hang is a fail).

**Destructive operations**: `force` refsets and firmware operations are host-gated to explicit user action (§11.3, §13.2); devices MAY additionally require physical confirmation for `force` (declare `state.force-confirm`).

**Privacy**: serials identify units; runtimes MUST NOT transmit identity off-machine except in user-initiated exports, and the diagnostic bundle's anonymization mode (§14.4) MUST strip serials, names, and log free-text. State hashes reveal *whether* two parties hold identical content; deployments treating patch content as confidential should note that hashes of well-known content are linkable.

**No code execution**: nothing in HARP delivers executable content to a device except signed firmware; devices MUST NOT interpret state objects as code.

**Denial of service**: credit (§4.2.1) bounds memory; devices SHOULD rate-limit `state.snapshot` and `diag.bundle`.

---

## 17. Conformance classes and certification

### 17.1 Device conformance

Requirements per class are the union of clauses marked for it throughout; a device declares classes in capabilities and MUST satisfy every MUST in each declared class. `harp-core` ⊂ every claim.

### 17.2 Host conformance

A conforming host implements `harp-core` client behavior plus, per supported class, the host-side MUSTs of §7, §8, §9, §11.4, §12, §14.4, §15. Hosts MUST degrade gracefully against devices lacking optional classes (feature absence is never an error dialog).

### 17.3 HARP Certified

Certification is per (device model, firmware line) × (runtime version, OS matrix) and is granted on passing the public test suite at an authorized lab, including at minimum:

| test | summary |
|---|---|
| T1 | enumeration, hello, identity, capability coherence |
| T2 | 1 000 × unplug/replug during streaming; session resume; zero host crashes; zero state loss |
| T3 | 100 × sleep/wake during streaming; same criteria |
| T4 | sample-rate change matrix under load; state preserved; epochs correct |
| T5 | 100 × DAW project reopen; recall verified by hash equality, silently (no dialogs on match) |
| T6 | firmware/engine mismatch flows: refuse-gracefully and read-only paths |
| T7 | operation through two chained hubs at minimum supported speed |
| T8 | 24 h streaming soak: zero unexplained `session_resets`, underrun budget per declared profile |
| T9 | protocol fuzzing on all inbound streams: no crash, hang, or memory fault |
| T10 | 50 × power loss during state apply and during firmware stage/commit: device boots, state is old-or-new, never hybrid |
| T11 | latency profile accuracy vs loopback (±1 ms; `harp-perf`: exact at the stream timestamp) |
| T12 | four-safe-actions UX audit (hosts): archive-before-push verified on the wire |
| T13 | four devices streaming concurrently (≥ 64 aggregate channels) for 24 h: independent per-device clock recovery verified; zero cross-session interference |
| T14 | free-running clock torture: ±200 ppm offsets and slow drift sweeps; ASRC output within quality floors; path latency constant; re-anchors occur only under induced starvation and are always evented |
| T15 | (`audio.deterministic`) two host-paced renders of identical state, events, and SSI range are byte-identical; (`audio.offline-rate`) likewise at 0.5× and 4× pacing |
| T16 | automation parity: point, ramp, mod, and per-voice events applied within ±1 sample (`harp-perf`); supersede rules honored; value↔display round-trips against descriptors |
| T17 | (`evt.transport`) sequencer lock: tempo changes, time-signature changes, locates, and loop wraps followed; realignment within ±1 sample host-paced, within stated correlation bound free-running |

The suite is open source; self-testing is encouraged and free; only the *mark* requires the lab. Fees fund the lab, the public compatibility matrix, and maintenance of the reference implementation — see §1.4's note and §19.

---

## 18. Specification versioning and extensibility

The specification and the wire protocol version together as `MAJOR.MINOR`: a `MINOR` bump adds optional capabilities and methods (old peers unaffected — unknown capabilities are unsupported, unknown notifications ignored, unknown methods answer `unsupported`); a `MAJOR` bump may change semantics and is negotiated in `core.hello`. Anything not gated by a capability string MUST remain stable within a `MAJOR`.

Changes are proposed as **HARP Enhancement Proposals** (HEPs): a numbered document with motivation, normative diff, capability strings introduced, and test additions. Two independent interoperating implementations are REQUIRED before a HEP merges into a release. Vendor extension namespaces (`x.` methods, counters, capability strings; streams 128–255) need no HEP but MUST NOT be required for conformant operation — the escape valve is real, and fenced.

---

## 19. Governance, licensing, and intellectual property

**Specification text**: CC BY 4.0. **CDDL schemas, test suite, reference implementations**: Apache-2.0 (chosen for its explicit patent grant). **Patents**: contributors and working-group members sign a royalty-free non-assertion covenant covering implementations of the specification (Open Web Foundation OWFa 1.0 model). **Trademark**: the protocol name and the "HARP Certified" mark are held by the stewarding entity; use of the *protocol* is unrestricted; use of the *mark* requires certification. This split is the funding answer to the standard open-protocol failure mode (§1.4): no one pays for the spec, someone pays for the proof.

**Stewardship**: bootstrap as a maintainer group with public process (HEPs, public issue tracker, released test suites); commit in the charter to transfer the marks and repositories to an independent foundation or an existing umbrella (e.g., a Linux Foundation / Software Freedom Conservancy structure) once three independent vendors ship certified devices. The charter MUST include a continuity covenant: if stewardship lapses, marks release to public use and the last test suite stands as the certification basis — vendors betting a decade of product on this protocol deserve that promise in writing on day one.

---

## Appendix A — CDDL definitions (normative)

Consolidated machine-readable schema (`harp.cddl` in the specification repository). Key root rules, gathered from the body:

```cddl
; --- framing (binary, not CBOR; given here for completeness) ---
; frame = fver(u8) stream(u8) flags(u16le) length(u32le) payload

; --- control envelope ---
envelope = { 0 => 0..3, 1 => uint, 2 => tstr, ? 3 => any }
error-body = { 0 => tstr, ? 1 => tstr, ? 2 => any }

; --- common ---
semver = tstr .regexp "\\d+\\.\\d+\\.\\d+(-[0-9A-Za-z.-]+)?"
hash   = bstr .size 33
tstamp = [uint, uint64]            ; (epoch, msc)

; --- identity ---
identity = { 0 => vendor, 1 => product, 2 => tstr, 3 => semver, 4 => engine,
             5 => [uint, uint], 6 => [* tstr], 7 => channel-map, 8 => latency-profile,
             ? 9 => tstr, ? 10 => uint }
vendor   = { 0 => uint, 1 => tstr }
product  = { 0 => uint, 1 => tstr }
engine   = { 0 => tstr, 1 => semver, 2 => hash }
channel-map = [* { 0 => uint, 1 => 0..1, 2 => tstr, ? 3 => tstr, ? 4 => tstr, ? 5 => bool }]
latency-profile = [* { 0 => uint, 1 => uint, 2 => uint, ? 3 => uint }]

; --- events ---
event = [ tstamp, etype: 0..7, any ]
param-event = { 0 => uint, 1 => float32, ? 2 => int, ? 3 => uint, ? 4 => uint }
ramp-event  = { 0 => uint, 1 => float32, 2 => tstamp, ? 3 => uint, ? 4 => uint }
mod-event   = { 0 => uint, 1 => float32, ? 2 => uint, ? 3 => uint }
transport-event = { 0 => uint, ? 1 => float64, ? 2 => [uint, uint], ? 3 => uint64,
                    ? 4 => float64, ? 5 => [float64, float64], ? 6 => float64 }
param  = { 0 => uint, 1 => tstr, ? 2 => tstr, ? 3 => tstr, ? 4 => [float, float],
           ? 5 => uint, ? 6 => tstr, ? 8 => uint, ? 9 => [* tstr],
           ? 10 => [* [float32, float]], ? 11 => uint }
params-rsp = { 0 => hash, 1 => [* param], 2 => uint, ? 3 => uint }

; --- state ---
object   = blob / list / tree / snapshot
blob     = { 0 => 0, 1 => tstr, 2 => bstr }
list     = { 0 => 1, 1 => tstr, 2 => [* hash] }
tree     = { 0 => 2, 1 => { * tstr => [hash, uint] } }
snapshot = { 0 => 3, 1 => hash, 2 => [* hash], 3 => uint, 4 => tstr,
             5 => semver, ? 6 => tstr }
ref      = { 0 => tstr, 1 => hash / null, 2 => uint64, 3 => bool }

recall-bundle = { 0 => "harpb", 1 => uint, 2 => identity-expectation,
                  3 => [* ref], 4 => [* object] / null,
                  ? 5 => any, ? 6 => tstr }
identity-expectation = { 0 => vendor, 1 => product, ? 2 => tstr,
                         3 => engine }
```

## Appendix B — Worked example: the project-open handshake (informative)

CBOR shown in diagnostic notation; `h'…'` truncated.

```
H→D ctl  {0:0, 1:1, 2:"core.hello", 3:{0:[1,0], 1:"harpd 0.4 (macOS)"}}
D→H ctl  {0:1, 1:1, 2:"core.hello", 3:{0:[1,0], 1:{
            0:{0:0x1209, 1:"Example Instruments"},
            1:{0:0x0042, 1:"EX-8"},
            2:"EX8-00231",
            3:"2.3.1", 4:{0:"ex8-fm", 1:"2.1.0", 2:h'01ab12…'},
            5:[1,0],
            6:["harp-core","harp-recall","harp-stream","harp-class-audio","audio.host-paced",
               "evt.param","evt.param.echo","evt.param.mod","evt.transport",
               "evt.ump","diag.loopback.digital","fw.ab-slots"],
            7:[{0:0,1:0,2:"Main L",3:"Mix",4:"main"}, …],
            8:[{0:48000,1:21,2:39},{0:96000,1:23,2:43}]}}}

H→D ctl  {0:0, 1:2, 2:"state.refs"}
D→H ctl  {0:1, 1:2, 2:"state.refs", 3:{0:[
            {0:"live/project", 1:h'01c4f9…', 2:8812, 3:true},      ; dirty!
            {0:"sys/settings", 1:h'017a02…', 2:14,   3:false}]}}

; bundle expects live/project = h'019be7…'  → mismatch AND dirty → four actions.
; user picks Push. Archive first:

H→D ctl  {0:0, 1:3, 2:"state.snapshot", 3:{0:"live/project", 1:"pre-push archive"}}
D→H ctl  {0:1, 1:3, 2:"state.snapshot", 3:{0:h'01d002…', 1:8813}}
H→D ctl  {0:0, 1:4, 2:"state.refset", 3:{0:"archive/2026-06-10T14:02:11Z",
                                          1:null, 2:h'01d002…', 3:1}}   ; create
D→H ctl  {0:1, 1:4, 2:"state.refset", 3:{0:1}}

; negotiate objects for the bundle's root:
H→D ctl  {0:0, 1:5, 2:"state.have", 3:{0:[h'019be7…', h'01aa31…', h'01b2c0…']}}
D→H ctl  {0:1, 1:5, 2:"state.have", 3:{0:[false,true,false]}}
H→D ctl  {0:0, 1:6, 2:"state.send", 3:{0:[h'019be7…', h'01b2c0…'], 1:18204}}
D→H ctl  {0:3, 1:0, 2:"core.credit", 3:{0:262144}}
H→D obj  …two objects, hash-verified on receipt…

H→D ctl  {0:0, 1:8, 2:"state.refset", 3:{0:"live/project",
                                          1:h'01d002…',          ; expect = post-snapshot head
                                          2:h'019be7…'}}
D→H ctl  {0:1, 1:8, 2:"state.refset", 3:{0:8814}}
D→H ctl  {0:3, 1:0, 2:"state.changed", 3:{0:"live/project", 1:h'019be7…', 2:8814, 3:false}}

; SYNCED. Shell starts audio, applies PDC, project sounds exactly as saved —
; and the state the musician had dialed in on the front panel survives in archive/.
```

## Appendix C — Registries (informative until first stable release)

Maintained in the specification repository: capability strings; standard method namespaces; standard counters; standard ref names; frame stream ids; the BOS platform UUID (final assignment at 1.0); media-type conventions for blobs (`application/x.<vendor>.<kind>` pending IANA registration of a `harp` tree); and HARP vendor ids for non-USB transports (USB devices use their USB VID — no parallel registry where one already exists). Registry additions are fee-free and PR-based; the steward's only veto is collision and incoherence.

## Appendix D — User-facing feature mapping (informative)

What the classes mean in the language users already speak:

| user expectation | HARP mechanism |
|---|---|
| "Every channel shows up, with names" | channel map over the HARP stream (§6.3) |
| "Every knob automates, sample-accurately" | point/ramp/mod/per-voice events + `harp-perf` timestamps (§9) |
| "My project reopens *exactly*" | Recall Bundle + CAS refset + hash equality |
| "It never eats the patch I had going" | dirty detection + archive-before-push + four safe actions |
| "My hardware sequencer follows the DAW like a plugin" | transport/tempo/musical-time events (§9.7) |
| "Firmware updates don't change old songs" | engine version gating (§13.4) |
| "No aggregate-device nonsense" | dedicated plugin↔device stream, independent of the DAW's interface (§8.1) |
| "When it breaks, support can actually help" | counters + loopback + diagnostic bundle |
| "All my boxes at once, in tune and in time" | per-device sessions, clocks, and ASRC; admission control; aligned timelines (§8.4) |
| "Survives unplug, sleep, hubs, Tuesdays" | §12.3 + certification gauntlet |

## Appendix E — Reference implementation and repository layout (informative)

```
harp/
  spec/            this document, harp.cddl, HEPs/
  conformance/     test suite (host-side harness + device exerciser), T1–T12
  runtime/         host runtime: transport, sessions, object store, correlation
  shell/           VST3 first; AU/CLAP next; thin over runtime IPC
  device-sdk/      portable C device stack (framing, CBOR, objects, CAS apply,
                   journaled storage) with ports: bare-metal MCU, Linux/Zynq
  reference-device/ open hardware dev-board firmware used by the conformance lab
  bundles/         recall-bundle library (read/write), MIT-friendly for DAW vendors
```

Suggested proving milestone, matching the protocol's own priorities: a VST3 shell streaming 16 channels from a dev board over the HARP stream (free-running), full recall with hash-verified reopen, unplug/replug survival, and one-click diagnostic bundle — the pipe, the safety, and the evidence, before any flagship instrument.

---

*End of Draft 0.3.0. Earlier drafts of the ideas herein circulated as a product brief titled "Hardware Plugin Runtime"; this document is the open-standard formulation of that work.*
