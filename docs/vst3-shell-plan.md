# VST3 shell — design plan (pre-session notes, 2026-06-10)

Goal: the Pi appears as an instrument track in a DAW. Recall bundle in the
project file, knobs as automation lanes, audio through the HARP stream.
This is the Appendix E proving milestone.

## Scope decisions (made; revisit only with reason)

- **Runtime embedded in the plugin process** for milestone 2. The §15.1
  one-runtime-per-machine daemon + IPC split is real work with no payoff
  until a second shell exists. The behavior contract (§15) is what matters;
  keep the runtime code in a `runtime/` library so the split is a move,
  not a rewrite.
- **Host-paced audio only** (mode 1). The refdev is pure-digital, so the
  DAW clock can own the stream: no ASRC, exact sample alignment, fixed
  latency. Free-running + ASRC matters for analog devices; defer until a
  device with converters exists (HAT on the Pi, or the KR260).
- **macOS / VST3 first.** SDK is pre-cloned at `external/vst3sdk` (502 MB,
  gitignored). AU/CLAP later via the same shell core.
- **C++ only in the shell**; everything reusable stays in C (`harpcore`,
  runtime library).

## Threading architecture (the part worth thinking about in advance)

The DAW audio thread may not block, allocate, or do USB I/O. The probe's
`render_host_paced` is the prototype of the *feeder*, not of `process()`:

- **Feeder thread** owns all USB I/O: keeps an IN read always pending
  (spec §4.2.1 0.3.1 rule), sends pacing frames, maintains N blocks of
  pipeline (adaptive depth — learned, not assumed; see probe).
- **Two lock-free SPSC rings** between feeder and audio thread: rendered
  audio in, pacing/SSI requests out (for an instrument with no input,
  the audio thread just consumes; the feeder paces ahead on its own).
- `process()` consumes rendered blocks; if the ring is empty, output
  silence and count starvation (§15.1: "counted and surfaced, never
  absorbed silently") — never block.
- Latency = pipeline depth × block size, constant, reported via
  `setLatencySamples` (PDC, §6.4/§8.3).
- SSI mapping: a session-local sample counter, NOT the DAW project
  position (locates/loops would rewind it). Transport events (§9.7)
  carry musical position separately, later.

## Device prerequisites (refdev work inside this milestone)

The event plane (§9) does not exist yet on either side. Minimum for a
useful shell:

1. `evt.params` — descriptor set for the 8 params (names, ranges, flags;
   the param-map-hash already exists in identity and must match, §9.3).
2. `evt` stream carrying `param` set events H→D (etype 1), applied at
   "now" first; sample-accurate timestamps once it works (`harp-perf`
   later).
3. **Echo** D→H (`evt.param.echo`): front-panel knob turns become events
   the shell can record as automation. Today's `x.harp-refdev.knob` should
   set values via the same internal path so echo fires.
4. Ramps (§9.4) after point events work — VST3 sends point automation
   per block; ramps are the optimization, not the MVP.

## Shell behavior checklist (§15.4)

- getState/setState ⇄ Recall Bundle (§15.3): reuse probe's save/restore
  logic; bundle = identity expectation + refs + embedded state closure.
- §12.2 flow on project open, four-safe-actions UI (minimal: a text
  status + buttons; polish later).
- Parameters from descriptors; param-map-hash mismatch → conservative
  mapping + warning.
- Device offline → render silence, show "offline" state, resume on
  reattach (runtime watches USB arrival; §12.3).

## Testing without a DAW

`external/vst3sdk` ships a **validator** (`bin/validator`) — run the shell
through it in CI fashion before any DAW. For interactive testing, ask the
user which DAW they run (Reaper is the most scriptable if they have no
preference).

## Open questions for the user

1. Which DAW to target for the first in-DAW test?
2. Plugin identity: vendor string/ids to use ("HARP Reference Project"?)
   — placeholder GUIDs fine for dev, decide before anything ships.
