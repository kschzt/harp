# Ethernet/IP transport — design plan (pre-session notes, 2026-06-17)

Goal: a second first-class transport binding alongside USB (§4.3), for the
case where USB stops scaling. One USB instrument is great; past ~5 devices
you hit hub chains, bandwidth admission (§8.4), and enumeration pain — that
is the wall Ethernet exists to clear, not "USB but a longer cable." This
fills the §4.4 reservation (network binding) with a concrete shape.

Nothing here is built yet. These are the conclusions of a design pass;
the companion spec (§4.4 says the network binding lives in its own spec to
protect the meaning of conformance claims) is written against them.

## The premise that drives every decision

Two axes, **orthogonal**:

- **Transport**: USB (normative, §4.3) vs Ethernet/IP (this doc).
- **Clock mode**: host-paced (§8.3 mode 1) vs free-running (§8.3 mode 0).

The mistake to avoid is coupling them. Clock mode is chosen by **whether
analog converters sit in the streamed path**, *not* by the transport:

|                              | USB                              | Ethernet                          |
|------------------------------|----------------------------------|-----------------------------------|
| pure-digital synth, real-time| host-paced (native frame, bulk)  | free-running (RTP/UDP + PTP)       |
| pure-digital synth, offline  | host-paced                       | host-paced over the reliable link |
| FX / live-input (converters) | **free-running** (native + ASRC) | **free-running** (RTP/UDP + PTP)   |

Because free-running is the same DSP subsystem regardless of transport
(see "Free-running is shared" below), the two bindings do **not** fork the
audio engine — they share its hard part and differ only in framing and
clock source.

## Scope decisions (made; revisit only with reason)

- **No QUIC.** It was the obvious "modern" pick (one TLS 1.3 handshake,
  per-plane streams with no head-of-line blocking, unreliable datagrams
  for audio in one connection) and it genuinely maps well onto the four
  planes. Rejected on **device weight**: the realistic device population is
  MCU-class (Cortex-M + RTOS/bare-metal, internal SRAM), not Linux-on-
  Cortex-A. A full TLS 1.3 + QUIC stack in userspace is a real burden there
  and is not a productized, boring thing on bare metal. The entire pro-audio-
  over-IP world (Dante/AES67 endpoints on single chips/FPGAs) runs on
  **RTP/UDP** for exactly this reason. QUIC's wins also shrink once audio
  leaves the reliable channel (below). Linux-class devices *could* do QUIC;
  not worth a second normative transport for them.

- **Audio plane = RTP framing over UDP, free-running, PTP-clocked.** Over
  Ethernet, the §8.1 reason HARP owns its own audio transport (a DAW can't
  open the device as a second OS audio device / aggregate) **weakens**: an
  RTP receiver living *inside the HARP runtime*, handing PCM to the shell,
  is not an OS audio device and triggers none of those failure modes — same
  shape as today's USB HARP stream, RTP frames instead of bulk frames. And
  networking **inverts** the USB clock-mode preference: host-paced over a
  network inherits an ugly real-time latency floor (RTT + pipeline depth),
  while free-running with ASRC gives constant low latency — which is why all
  networked audio is free-running. So free-running is both the *natural* and
  the *eventually-required* (converters, §8.3) Ethernet mode. Build it once,
  properly.

- **Control/state/events = the existing framed link (§4.2) over TCP.** It is
  already transport-agnostic (`harp_io` in `core/include/harp/link.h`) and
  already runs over TCP today as the dev transport (`harp-deviced --port`,
  `harp-probe -d host:port`). ctl/obj/evt/log need reliable+ordered; that is
  TCP. The §4.2.1 bulk-pair deadlock is a USB artifact — TCP's socket
  buffering makes it disappear; `core.credit` (§5.5) still governs `obj`.

- **PTP (IEEE 1588 / PTPv2) for clock correlation, not `time.ping`.** NTP-
  style `time.ping` (§7.2) tops out ~1 ms on a quiet link and will not hold
  the §7.2 correlation MUST through a loaded switch. PTP is the only real
  answer and is what every audio-over-IP standard uses. Its pain is mostly
  multi-hop installs with PTP-unaware switches — which the trusted-segment
  posture below largely defuses.

- **Don't reinvent RTP — but only where it pays.** The §8.2 audio frame is
  ~80% of RTP already; the one thing it lacks is a **sequence number**, which
  RTP has and UDP delivery *requires* (you cannot detect a silently dropped
  datagram without it; `audio_late_frames` becomes uncountable otherwise).
  Adopt RTP framing on the Ethernet plane. Do **not** push RTP onto USB
  (below).

## RTP stays off USB (the load-bearing non-change)

RTP and host-paced have **opposite control directions**: RTP is sender-owns-
the-clock fire-and-forward streaming; host-paced is receiver(host)-owns-the-
clock request/response pacing (the host's SSI is the master index, `slots=0`
frames pace silence, the event fence orders events vs ranges). Forcing RTP
onto host-paced means inheriting RTP's conventions (timestamp offset/wrap,
SSRC, RTCP expectations) while using none of its machinery and bolting the
fence on as a header extension. And the two things RTP buys are exactly what
USB host-paced does not need:

1. sequence/loss detection — USB bulk is reliable and ordered.
2. interop — a host-paced USB stream is a private DAW↔device pacing channel;
   it can never appear on a Dante network.

So: **USB keeps the native §8.2 frame; RTP lives on the Ethernet plane
only.** "80% of RTP" is conceptual reuse for building the Ethernet plane,
not a mandate to unify the two wires.

## Offline bounce survives over Ethernet (recovering the "bummer")

Host-paced is not a USB feature — it is a **real-time-vs-offline**
distinction. Host-paced over Ethernet is bad *only* for the real-time
latency floor; offline bounce (`audio.offline-rate`) is latency-immune by
definition — you page SSI ranges as fast as the wire allows. §15.1 already
draws this line ("offline contexts MAY block on stream progress; real-time
contexts MUST NOT"). So:

- **Ethernet real-time** → free-running (RTP/UDP/PTP).
- **Ethernet offline** → host-paced, SSI ranges over the **reliable framed
  link (TCP)** — no UDP, no RTP, no real-time constraint, byte-identical
  (T15). This is request/response over a reliable pipe, i.e. reusing the
  control channel for a batch op — **not** a second real-time media stack,
  so it doesn't reintroduce the two-transports problem.

Deterministic offline-bounce-through-hardware is therefore kept over
Ethernet. Devices without a host-paced engine (pure RTP/MCU interop boxes)
simply don't declare `audio.deterministic`; it's a capability, not a
universal promise.

## Free-running is shared, not duplicated

Free-running is one ASRC subsystem — elastic buffer, drift tracking, the
§8.3 quality floors (≤0.01 dB passband ripple, ≥120 dB stopband, slew-
limited corrections) — fed by **two clock-correlation front-ends**:

- USB: in-band frame `(epoch, msc)` timestamps + `time.ping` (§7.2).
- Ethernet: PTP.

Same DSP back-end, two small adapters. This matters beyond Ethernet:
free-running is **REQUIRED wherever converters are in the path** (§8.3) —
FX units, vocoders, sampling-in, interfaces — so USB FX devices need it too.
The reference impl is host-paced-only purely because the refdev has no
converters (architecture.md), making USB free-running the *under-exercised*
path, not a missing one. Building the Ethernet binding builds the ASRC core
that USB FX devices also need. The hard part is shared; the transports are
not forked.

## Security posture: trusted segment, signed firmware, nothing else mandatory

The pro-audio-over-IP world (AES67/AVB/Dante) ships **media in the clear**
and secures by network isolation, not crypto — for latency, for multicast
(you can't TLS a multicast stream), and because the wire is physically
trusted. HARP's earlier instinct was to mandate TLS 1.3 (§4.4) on the theory
of a hostile shared LAN. **Reversed:**

- **Default deployment = a dedicated, isolated switch/segment** for the
  instruments, removed from the studio/house LAN. Switches are ~$15; this is
  the actual industry deployment model. Users who don't want to are not
  forced onto Ethernet at all — USB stays the answer until they hit the
  many-device wall.
- **No transport encryption, no pairing/PKI in the base binding.** A
  "secure shared-LAN mode" was considered and **dropped** — it doesn't make
  sense: if you need that, you're on the wrong network.
- **Signed firmware (Ed25519, §13.2) is the one constant** — transport-
  independent, the genuine high-value target (§16), kept regardless.
- Bonus synergy: the dedicated-switch posture also **tames PTP** (few hops,
  pick a PTP-capable switch — the multi-hop unaware-switch pain doesn't
  arise) and makes the mDNS TXT-record privacy worry moot (no adversary on
  the segment to read it).
- Honest caveat: the segment is only as isolated as the DAW PC, which is
  dual-homed (audio switch + WiFi/internet). That's the host's firewall job,
  not the device's. Worth a sentence in the spec; not a device requirement.

## Discovery

`_harp._tcp` via mDNS/DNS-SD (as §4.4 already names). Discovery only —
untrusted/spoofable by nature, but under the trusted-segment posture that's
fine. Maps onto §4.1 transport requirements: resolve name→address is "stable
addressing"; connection close + `core.ping`/keepalive + idle timeout is the
real "detach notification" (mDNS goodbye packets are unreliable). §12.3
unplug/replug resilience carries over unchanged — the network just swaps the
detach *trigger* from USB unplug to connection-drop/timeout.

## DAWless is a first-class state

**HARP is additive, never subtractive.** The device is an instrument first;
HARP is the integration layer that lights up *when a runtime connects* and
must never make the box worse when one doesn't. Standalone:

- via MIDI clock (UMP §9.7/§9.10, or DIN) + the device's own analog line
  outs — needs no HARP at all, which is the point. No DAW ⇒ no recall (there
  is no project to recall into); the instrument still plays.
- via Dante/AES67 — see below. This is the *primary justification* for the
  reserved bridge: a HARP device on a fabric with no computer is an AES67
  citizen wearing a HARP identity.

## AES67 interop: reserved, optional, firmware-deliverable

AES67 = RTP framing + PTP + SDP/SAP session management + a 48 kHz-centric
profile. The baggage is the **session management and profile strictness**,
not the RTP header. So we already use the good part (RTP framing + PTP) and
**drop SDP/SAP in favor of HARP's own control plane** for stream setup.

Full AES67 conformance — the thing that makes the device a Dante/AES67
*citizen* (visible on the fabric) — is **reserved as an optional capability
flag**, not baseline. Reserving is honest now precisely because RTP framing
is already locked, so the bridge is a cheap, real door. It can ship as a
**firmware update** (capability advertised on next `core.hello`, §6.2 +
§13): hosts that don't care ignore it; a Dante facility lights up.

What Dante visibility actually buys (and why it's *reserved*, not built):
nearly all of it is the **live / install / facility / DAWless** world —
computer-less rigs, FOH/monitoring/broadcast one-to-many routing (which HARP
is point-to-point by design, §15.1, and explicitly cedes — routing is a
§1.3 non-goal), and dual-citizenship in a studio already wired for Dante.
None of that is the core studio+DAW+recall user, whose audio already arrives
in the session via the plugin shell. Adjacent market, not core requirement.

## The shape, in one breath

- **USB binding** (§4.3, normative): native §8.2 frame; host-paced for
  pure-digital (real-time + offline), free-running for converters; bulk.
- **Ethernet binding** (this doc, fills §4.4): framed link over TCP for
  ctl/obj/evt/log; **RTP/UDP + PTP, free-running** for real-time audio;
  host-paced offline bounce over the reliable link; trusted-segment posture;
  signed firmware the only constant security measure; AES67/Dante bridge
  reserved as an optional, firmware-deliverable capability.
- **No QUIC.** Clock mode tracks converter-presence, not transport, so the
  free-running ASRC core is shared across both bindings.

## Open questions (deferred, low-cost)

1. **RTP framing vs. a HARP-native UDP frame** for the Ethernet media —
   *resolved: RTP framing*, for the cheap interop option and the tooling
   (Wireshark/test gear). Recorded here so it isn't relitigated.
2. Whether to **reserve** the AES67-full bridge in the spec now or leave it
   wholly future — *resolved: reserve* (one sentence, idiomatic to §4.4/
   §1.3, backed by the already-chosen RTP path).
3. PTP profile specifics (default profile vs. a HARP profile; software vs.
   hardware timestamping floor; what correlation bound to *require* over
   Ethernet vs. the §7.2 USB numbers) — genuinely open, needs measurement on
   a real dedicated segment before the companion spec commits a MUST.
4. RTP payload mapping for HARP's slot/interleave model and the `fmt` set
   (float32 required; int24 for AES67-bridge compatibility) — detail for the
   companion spec.

## What this touches when built (not now)

- `core/`: the framed link already abstracts the pipe; TCP path exists.
  New: an RTP/UDP receiver+sender and a PTP client (likely a host/runtime
  concern, with a device-side counterpart). Keep the ASRC core transport-
  agnostic so USB free-running and Ethernet free-running share it.
- Spec: a §4.4 companion document; small errata to §7.2 (PTP correlation
  over the network binding), §8.3 (clock-mode-by-converter wording), and a
  reserved capability string for the AES67 bridge.
- `docs/architecture.md`: a second transport column once it's real.
