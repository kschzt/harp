# Security Policy

## Threat model (spec §16)

HARP devices face hostile input by design: on USB a device trusts nothing from the
host, and on the §4.4 Ethernet/IP binding the surface widens to any node on the
(dedicated, trusted) segment. The defenses are structural — every CBOR and frame
parser is bounds-checked, allocation-bounded, and fuzz-tested (certification T9
fuzzes `ctl`/`evt`/`obj`; a crash or hang is a fail, and libFuzzer + ASan run in CI on
every push); credit (§4.2.1) bounds memory; and denial of service is shed **per peer,
not globally** — a source that repeatedly fails before completing `core.hello` is
penalized for a short window (a bounded penalty ring keyed by source IP), half-opens
that never send hello are dropped on a deadline, and a connection that completes hello
is never penalized, so no single source can deny service to others. Firmware is the
highest-value target and is signature-verified (§13.2); nothing in HARP delivers
executable content to a device except signed firmware, and devices never interpret
state objects as code. The Ethernet/IP binding relies on segment isolation rather than
transport encryption (as AES67/Dante/AVB do) — HARP on a shared, untrusted LAN is out
of model, and the answer there is USB.

## Reporting a vulnerability

Please report suspected vulnerabilities **privately** — do not open a public issue,
and do not include a working exploit in any public channel.

> **Maintainer:** set a real disclosure address before publishing this repo.

Email **security@** *(the project's domain — placeholder; the maintainer must set the
real address)* with:

- the affected component (spec section, `core`/`device`/`host`/`shell`, or transport
  binding) and version/commit;
- a description of the impact and the conditions to reproduce it;
- a minimal proof of concept if you have one (a crashing input, a captured frame).

Expect an acknowledgement, a coordinated fix, and credit in the release notes unless
you ask otherwise. Because a parser crash or hang is itself a conformance failure (T9),
a reproducible malformed-input crash is always in scope.

## Supported versions

Security fixes track the current stable specification line, **1.1.x**, and its
reference implementation on the default branch. Older editor's drafts (0.x) are
superseded and unsupported.
