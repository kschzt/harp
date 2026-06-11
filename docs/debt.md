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
| 10 | macOS RT path is sync libusb at user QoS; async transfers + CoreAudio workgroups would cut latency floor | medium | deferred, documented in architecture.md |
| 11 | timing-test pitch-arrival spread sits exactly at its 5 ms limit (wall-clock realtime test) — occasional at-limit failures under load | watch | monitoring; tighten the engine or loosen the limit with data, not vibes |
| 12 | evt_late margin at block 256: one late event in ~8 min of triple-ramp+notes+panel flood (3×120 s clean, 1×120 s caught one; blocks ≥512 measured zero over 180 s). Candidate fix: event headroom 1 -> 2 DAW blocks — but that shifts reported latency AND the golden render timeline, so decide with ears + fresh oracle | watch | measured 2026-06-12 overnight; revisit in a daytime session |
