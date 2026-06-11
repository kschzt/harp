# Technical debt ledger

Living document; items leave by being fixed, not forgotten.
(Status as of 2026-06-11 overnight quality block.)

| # | item | severity | status |
|---|------|----------|--------|
| 1 | `harp-deviced.c` monolith (2,137 lines, 14 globals) | high | DONE — split into engine/state/session/panel + device.h (mechanical move, A/B-verified byte-identical render vs old binary). Follow-up: engine globals behind accessors |
| 2 | protocol client duplicated (probe vs shell runtime) | high | IN PROGRESS — extracting shared host client library |
| 3 | parsers never fuzzed (spec §16/T9 requires it) | high | IN PROGRESS — libFuzzer targets + CI smoke runs |
| 4 | shell: no reconnect on device replug; log-and-continue error paths; `staged*` naming is stale | medium | IN PROGRESS — reconnect loop, renames |
| 5 | debt tracked informally | low | this file |
| 6 | §15.1 runtime/shell process split (full client unification) | medium | deferred — item 2 is the stepping stone |
| 7 | free-running mode unexercised by any DAW path (no analog device yet) | low | deferred until a converter HAT / real analog device |
| 8 | recall-test transient failure after cold boot (one occurrence, error codes now logged) | watch | monitoring |
| 9 | sub-ms PDC accuracy: reported latency vs ring-occupancy mean differs by ≲1 block | low | acceptable; revisit with async transport work |
| 10 | macOS RT path is sync libusb at user QoS; async transfers + CoreAudio workgroups would cut latency floor | medium | deferred, documented in architecture.md |
