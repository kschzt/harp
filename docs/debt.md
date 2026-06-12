# Technical debt ledger

Living document; items leave by being fixed, not forgotten.
(Status as of 2026-06-11 overnight quality block.)

| # | item | severity | status |
|---|------|----------|--------|
| 1 | `harp-deviced.c` monolith (2,137 lines, 14 globals) | high | DONE — split into engine/state/session/panel + device.h (mechanical move, A/B-verified byte-identical render vs old binary). Follow-up: engine globals behind accessors |
| 2 | protocol client duplicated (probe vs shell runtime) | high | DONE — `host/client.{h,c}` (hello/identity, refs, snapshot, closure fetch/push, refset CAS, rid correlation); probe and shell both consume it. Net −700 lines |
| 3 | parsers never fuzzed (spec §16/T9 requires it) | high | DONE — fuzz/ targets for cbor/envelope/object/link/audio; libFuzzer+ASan smoke in CI (Linux), ASan corpus replay locally (Apple clang has no fuzzer runtime); gen-corpus seeds from canonical encoders; scripts/abuse-test.sh slams a live daemon (T9-lite). Follow-up: continuous fuzzing (OSS-Fuzz-style) once public |
| 4 | shell: no reconnect on device replug; log-and-continue error paths; `staged*` naming is stale | medium | DONE — supervisor thread owns session lifecycle: hot-plug attach, unplug -> silence + 1 s retry, replug -> hello + bundle re-assert + stream (scripts/replug-test.sh proves it on hardware); staged* renamed bundle* |
| 5 | debt tracked informally | low | this file |
| 6 | §15.1 runtime/shell process split (full client unification) | medium | deferred — item 2 is the stepping stone |
| 7 | free-running mode unexercised by any DAW path (no analog device yet) | low | deferred until a converter HAT / real analog device |
| 8 | recall-test transient failure after cold boot (one occurrence, error codes now logged) | watch | monitoring |
| 9 | sub-ms PDC accuracy: reported latency vs ring-occupancy mean differs by ≲1 block | low | acceptable; revisit with async transport work |
| 10 | macOS RT path is sync libusb at user QoS; async transfers + CoreAudio workgroups would cut latency floor | medium | DONE (async) — usb_io rebuilt on async libusb: dedicated event thread, always-pending IN transfers both pipes, fire-and-forget pacing slots, drain-on-stall gone by construction. Cushion 5 -> 2 blocks, full flood matrix 64-1024 green: **16 ms total reported latency at DAW blocks <= 256** (was 32 ms); offline bounce 2.5x faster. CoreAudio workgroup join remains deferred: VST3 exposes no DAW workgroup token (AU does — revisit with the AU port) |
| 13 | block-64 event timing was structurally broken and unmeasured: ramp-thinning flush emitted stale-END ramps (~1100/s), and event timestamps could be born into already-paced ranges | high | DONE — gesture-aware pend flush (flush = "now" set after a silent pacing block); frontier cap (frame END bounded by read + target + headroom - dawBlock) makes the invariant strict at every block size: no event is ever born behind the pacing frontier. Block-64 floods: evt_late 0, ramp_late 0, fence_timeouts 0 |
| 11 | timing-test pitch-arrival spread sits exactly at its 5 ms limit (wall-clock realtime test) — occasional at-limit failures under load | watch | monitoring; tighten the engine or loosen the limit with data, not vibes |
| 12 | evt_late margin at block 256 (one late event per ~8 min of flood) | high | DONE — event pump (dedicated event->wire thread) + event fence (spec 0.3.4 §8.3.1: pacing frames carry an event-sequence the device must consume before rendering). Order is structural now, not a timing budget. Flood-verified: evt_late 0 over 3×120 s at block 256, ramp_late rate down 10×, zero added latency. fence_timeouts counter is expected overshoot noise (fence counts future-timestamped events); evt_late stays the truth metric |
