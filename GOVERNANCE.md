# Governance

HARP is two things kept deliberately separate: an **open specification** (the product)
and a **reference implementation** (the proof). This document says how each evolves and
who decides. It is the operational companion to spec §18 (versioning) and §19
(governance, licensing, IP).

## Decisions are HEP-driven

Changes to the protocol — wire formats, semantics, conformance requirements — are made
through **HARP Enhancement Proposals** (§18), not by direct commit. A HEP carries its
motivation, a normative diff to the spec and `spec/harp.cddl`, any capability strings it
introduces, and its conformance test additions. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the mechanics.

The binding rule, from §18: **two independent, interoperating implementations are
REQUIRED before a HEP merges into a release.** A proposal is not accepted because it is
elegant; it is accepted because two parties have built it and they interoperate. This is
what keeps "implementable from the spec alone" true.

Vendor extensions (`x.` methods/counters/capabilities, streams 128–255) need no HEP, but
MUST NOT be required for conformant operation. Everything not gated by a capability
string is frozen within a MAJOR version and can only move by HEP.

## Maintainers

The project bootstraps as a **maintainer group operating in public** — HEPs, a public
issue tracker, and released test suites (§19). Maintainers:

- review and merge changes to the reference implementation and test suites;
- shepherd HEPs, and confirm the two-implementation interop bar before a protocol HEP
  merges;
- steward the spec text and the CDDL schema, keeping them and the reference in sync;
- hold the trademarks and the certification mark in trust for the project.

Maintainership is earned through sustained, high-quality contribution and is granted by
the existing maintainers. The current maintainers are listed in the repository metadata;
the spec's editor line (§ front matter) names the working group as it forms.

## Spec vs. reference implementation

- **The spec leads.** It is normatively complete (§1–§19) and implementable from the
  document alone, against the wire pinned in `spec/harp.cddl`. A conforming device or
  host is built from the spec, not from this repo's code.
- **The reference tracks the spec.** `core/`, `device/`, `host/`, `shell/`, `tools/`
  exist to prove the spec implementable end-to-end and byte-identically. When the two
  disagree, the spec is normative and the implementation is the bug — unless the
  implementation taught us something, in which case the fix is a HEP (or, for a
  non-wire clarification, an editorial spec revision recorded in the changelog, as the
  0.3.x–1.1.x errata were).
- **Certification** (future) is granted on passing the public test suite (§17). Use of
  the *protocol* is unrestricted; use of the certification *mark* requires passing the
  suite.

## Continuity

Per the §19 charter commitment, stewardship transfers to an independent foundation (or
an existing umbrella) once three independent vendors ship certified devices, and a
continuity covenant guarantees that if stewardship lapses, the marks release to public
use and the last released test suite stands as the certification basis. Vendors betting
a product line on HARP get that promise in writing.

## Licensing and IP

Specification text is **CC BY 4.0**; schemas, reference implementation, and test suite
are **Apache-2.0** (for its explicit patent grant). Contributors and working-group
members sign a royalty-free non-assertion covenant covering implementations of the
specification (§19). See [LICENSING.md](LICENSING.md).
