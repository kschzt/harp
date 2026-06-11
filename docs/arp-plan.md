# Arpeggiator + §9.7 transport — session plan

The last unimplemented §9 slice, and the launch-demo centerpiece: the
hardware follows Ableton's timeline the way plugins do (T17 in
miniature). Status: planned; floor verified (hw-tests 3/3, timing
proven to ±1 sample, evt_late zero-tolerance enforced).

## Shell: transport events (§9.7)

- `ProcessContext` arrives every block (Live provides tempo, PPQ
  position, play state, loop region, bar start).
- Send etype-7 transport events on every CHANGE (play/stop, tempo,
  locate, loop wrap — position fields anchored at the jump's timestamp)
  plus the ≥1 Hz refresh while playing (spec MUST).
- Timestamps: stream domain as always (streamPos + offset + latency);
  the (timestamp, PPQ, tempo) triple defines musical time linearly
  until the next event.

## Device: the arp engine

- Note latch: held notes accumulate into a small ordered buffer (≤ 8);
  all released = latch clears (or hold-mode param later). The mono
  voice steps through the latch — a true arp.
- Step clock derived from the transport mapping: musical position at
  SSI x = ppq + (x - ts) * tempo / (60 * rate); step fires where
  position crosses division boundaries — sample-exact on Live's grid,
  by construction. Loop wraps/locates arrive as new anchor events;
  realign within ±1 sample (T17).
- New recallable params (param-map-hash WILL change — first real
  exercise of the §9.3 mismatch path; old bundles apply onto matching
  ids gracefully):
  - 9: Arp Mode (steps: off / up / down / up-down / as-played)
  - 10: Arp Division (steps: 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32)
  - 11: Arp Gate (0..1 of step length)
  - 12: Arp Octaves (steps: 1..4)
- Shell controller: param list grows to 12 (same names/ids).

## Tests

- timing-test grows a tempo-lock assertion: render with transport
  events at a known BPM + latched chord, verify step onsets land at
  division boundaries (the T17 check, automatable with --notes plus a
  CLI-host transport feed — add --bpm flag emitting ProcessContext).
- Loop-wrap test: CLI host simulates a loop (projectTimeSamples jumps
  back); assert the arp realigns (onset at the wrap lands on grid).
- hw-tests gains both.

## Demo choreography (launch video material)

Live playing, arp locked to the grid → drag Live's tempo, hardware
follows instantly → loop wraps, arp wraps → tweak Division from the
web panel mid-playback (echoed into Live as automation) → save the
set, reopen: the groove itself recalls.
