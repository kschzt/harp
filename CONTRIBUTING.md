# Contributing to HARP

HARP is a protocol first. The spec is the product, the reference implementation
is the proof, and the test suite is the contract between them. Contributions fall
into two tracks with different bars:

- **The protocol** (`spec/`, `spec/harp.cddl`) changes through the HEP process.
- **The reference implementation** (`core/`, `device/`, `host/`, `shell/`, `tools/`,
  `tests/`, `scripts/`) tracks the spec and is held to the same "prove it with a
  test" bar the spec sets for itself.

## Changing the protocol — the HEP process (§18)

Wire-format, semantics, or conformance changes are proposed as **HARP Enhancement
Proposals**. Per spec §18, a HEP is a numbered document carrying:

- **motivation** — the problem, and why the escape valves below don't already cover it;
- **normative diff** — the exact spec + `harp.cddl` changes;
- **capability strings** introduced (a MINOR addition is gated behind a capability so
  old peers are unaffected; a MAJOR change is negotiated at `core.hello`, §5.4);
- **test additions** — the conformance test(s) that pin the new behavior.

**Two independent, interoperating implementations are REQUIRED before a HEP merges**
into a release. This is the load-bearing rule: the spec is only "implementable from
the document alone" if someone other than the reference has done it.

You do **not** need a HEP for a vendor extension. The `x.` method/counter/capability
namespaces and streams 128–255 are reserved for exactly this (§18) — but a vendor
extension MUST NOT be required for conformant operation. If you find yourself wanting
to make an `x.` capability mandatory, that's a HEP.

Anything not gated by a capability string MUST stay stable within a MAJOR version;
touching it is by definition a HEP.

## Building and running the tests

The five-minute, no-hardware path is in the README ("Getting started", Path 1):

```sh
cmake -B build && cmake --build build
./build/harp-tests                 # unit tests: core, device codec, safety contracts
```

The plugin shells and their CLI test hosts build from `tools/vst3-host` (see README
Path 3 for the VST3/CLAP SDK + libusb prerequisites).

**Match the CI you'll be graded against.** Every push runs:

- **`ci.yml`** — the sandboxed matrix: builds + unit tests on macOS/Windows/Linux,
  `pluginval` strictness 10 (all three OSes), `auval` (macOS), the fuzzed parsers
  (libFuzzer + ASan, §16/T9), a live protocol-abuse test, and the **`cert-harness`**
  gate.
- **`eth.yml`** — the §8.7 Ethernet/IP conformance suite against a localhost daemon on
  three OSes. **`scripts/eth-suite.sh` is the single source of truth** — add a new
  protocol-level test there, not in a per-OS fork.
- **`hw.yml`** — the real-hardware suite on the Pi rig (USB, plus the §8.7 suite over a
  real network hop). Runs when the self-hosted rig is online; the cross-format AU legs
  self-skip off macOS.
- **`reaper-e2e.yml`**, **`perf-gate.yml`**, and the nightly **`soak.yml`**.

Before a protocol claim ships, run it through the certification battery by number:

```sh
scripts/cert-harness.sh            # run the cloud-capable T1–T17 subset, print the report
DRY_RUN=1 scripts/cert-harness.sh  # print the run/skip plan for this host
```

`cert-harness.sh` re-uses the existing test scripts unchanged and reports pass / skip
(rig- or build-gated) / uncovered per T#. It is a pure orchestrator — a new
conformance test belongs in its covering script and gets indexed in
`scripts/cert-tests.tsv`, not reimplemented in the harness.

## Golden-pin discipline

Several tests pin a rendered-byte hash as a regression oracle — `engine-golden-test.sh`
(the raw device DSP, pinned **per OS** because libm `sin`/`exp` differ across platforms),
`offline-golden-eth.sh` (run-to-run determinism of the host-paced bounce), and the
per-format oracles the hardware suite drives. These hashes exist to catch **unintended**
DSP drift, so:

- A golden that changes **without** a deliberate DSP change is a bug — find it, don't
  re-pin over it.
- An **intentional** engine/DSP change re-baselines the affected pins **in the same
  commit**, with the commit note stating *what* changed and *why* the render moved
  (the scripts say this out loud: "re-pin DELIBERATELY, with why"). Update every
  per-OS variant of the pin, not just the one your machine printed.
- A new platform prints its hash as `UNPINNED` and passes on determinism + non-silence
  alone; capture the value from the CI log and pin it to arm the oracle there.

The four "no silent state loss" safety contracts — CAS conflict (§11.3),
archive-before-push (§11.4), param-map-hash (§9.3), the event fence (§8.3.1) — each
have a regression-catching test. A change near any of them must keep its test green,
and a new invariant of that kind needs a test that provably fails on regression.

## Commits and pull requests

- Keep the reference implementation and the spec in sync: a wire change that lands in
  `harp.cddl` without the matching spec prose (or vice versa) is incomplete.
- When an AI assistant co-authored a change, add the trailer:

  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  ```

- The PR template's checklist (tests added/updated, goldens re-pinned *with a reason*
  if DSP changed, spec/CDDL updated if the wire changed, docs updated) is the minimum
  bar — fill it honestly.

## Building a device or a host

If you're here to implement HARP rather than change it, start from the spec, not this
repo's code. Per the README's "Building on HARP" section, the spec (§1–§19) is
normatively complete: build a device from §4–§16 and a host from §15, validate your
wire against `spec/harp.cddl`, and check your claim against the conformance classes in
§17 (`harp-core` ⊂ every claim). The reference device (`device/`, ~3k lines of C11) and
runtime (`shell/runtime.cpp`) are worked examples to read, not a framework to depend on.
When your implementation interoperates with the reference, you have the second
implementation a HEP needs.
