---
name: Bug report
about: A conformance failure, crash, or wrong behavior in the reference implementation
title: ""
labels: bug
assignees: ""
---

<!--
For a *protocol* change or ambiguity in the spec, don't file this — use the HEP
process (see the links on the "New issue" chooser and CONTRIBUTING.md §18).
A parser crash or hang on malformed input is a security issue: report it privately
per SECURITY.md, not here.
-->

## What happened

A clear description of the bug and what you expected instead.

## Component

- [ ] `core/` (framing, CBOR, object store)
- [ ] `device/` (harp-deviced / engine)
- [ ] `host/` (harp-probe)
- [ ] `shell/` (VST3 / AU / CLAP / FX)
- [ ] `tools/` or `scripts/` (test hosts, conformance suites)
- [ ] docs / spec prose

Spec sections involved (if known): §

## Environment

- OS + version:
- Transport: USB / §8.7 Ethernet-IP (loopback or real hop) / TCP sim
- Device: reference Pi / simulated daemon / other
- DAW + version (if a shell bug):
- Commit / release:

## Reproduction

Steps, and the shortest command that shows it — e.g. a `harp-probe` invocation, a
`scripts/*.sh` run, or a captured frame. If a golden/oracle changed, paste the
`got != pinned` line.

```
```

## Logs / counters

Relevant output: session counters (§14.2), a diag bundle (§14.4), or the failing test's
stdout.
