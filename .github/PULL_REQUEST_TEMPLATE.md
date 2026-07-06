<!--
Keep the spec, the CDDL, and the reference in sync — a change to one without the others
is incomplete. For a protocol change, this PR should carry (or implement) a HEP: see
CONTRIBUTING.md §18.
-->

## What & why

Brief description of the change and the problem it solves.

## Checklist

- [ ] **Tests added or updated.** New behavior has a test that fails without the change;
      the relevant suite is green (`ci.yml` / `eth.yml` via `scripts/eth-suite.sh`, and
      `hw.yml` if hardware-affecting).
- [ ] **Goldens.** No pinned render hash moved — *or*, if a DSP/engine change moved one,
      I re-pinned every per-OS variant **in this PR** and the commit note says **what
      changed and why** (see CONTRIBUTING.md, "Golden-pin discipline"). A golden that
      moved without a stated DSP reason is a bug, not a re-pin.
- [ ] **Wire / spec / CDDL.** If the wire format or semantics changed, `spec/harp.cddl`
      and the spec prose are updated together, this is (or implements) a **HEP** (§18),
      any new behavior is behind a capability string, and — per §18 — two interoperating
      implementations exist. If the wire did **not** change, tick this and say so.
- [ ] **Safety contracts.** Changes near CAS conflict (§11.3), archive-before-push
      (§11.4), param-map-hash (§9.3), or the event fence (§8.3.1) keep those
      regression tests green.
- [ ] **Docs updated** (README, `docs/`, or the changelog atop the spec) if behavior,
      build, or claims changed.
- [ ] Commit trailer added when AI-assisted:
      `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`

## Notes for the reviewer

Anything worth calling out — a moved golden and its reason, a rig-only test that
self-skips in the cloud lane, a deferred follow-up.
