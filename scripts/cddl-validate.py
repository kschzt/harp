#!/usr/bin/env python3
# CDDL wire-validation gate — schema-as-contract for the normative CDDL.
#
# spec/harp.cddl is a MACHINE-READABLE schema, but nothing in CI machine-CHECKED it:
# a rule could drift from the prose / Appendix A / the implementation and no test would
# notice. This gate closes that: it (1) proves the schema is well-formed, and (2) validates
# representative on-the-wire CBOR artifacts against their normative rules with a real CDDL
# validator (pycddl). Behavioral wire tests (corrupt-cbor, round-trip, fuzz) prove the code
# ACCEPTS/REJECTS bytes; this proves the SCHEMA itself matches the wire format.
#
# It gates TWO surfaces, not one: the consolidated spec/harp.cddl AND the NORMATIVE Appendix A
# of spec/harp-spec.md (the spec document is what a second implementer builds from). The two
# MUST define the same rules and validate the same wire — otherwise the normative document can
# carry a bug the harp.cddl gate would have caught. That is not hypothetical: Appendix A was
# missing `uint64 = uint` while using it (tstamp/ref/generation), the exact undefined-rule bug
# this gate exists to catch, sitting in the normative surface a vendor reads.
#
# pycddl roots validation at the schema's first rule, so each sample is checked against a
# tiny "start = <rule>" prepended to the schema (CDDL allows the forward reference). Note that
# a bare Schema(cddl) parse is LENIENT about an undefined rule name — the drift only surfaces
# when a sample is validated against a rule that transitively uses it (hence the wire suite).
import struct, sys, os, re

try:
    from pycddl import Schema
    import cbor2
except ImportError:
    sys.exit("cddl-validate: need `pip install pycddl cbor2`")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CDDL = open(os.path.join(ROOT, "spec", "harp.cddl")).read()
SPEC_MD = open(os.path.join(ROOT, "spec", "harp-spec.md")).read()

def extract_appendix_a(md):
    # the normative consolidated schema: the first ```cddl block under "## Appendix A".
    m = re.search(r'^##\s+Appendix A\b.*$', md, re.M)
    if not m:
        sys.exit("cddl-validate: '## Appendix A' heading not found in spec/harp-spec.md")
    b = re.search(r'```cddl\n(.*?)\n```', md[m.end():], re.S)
    if not b:
        sys.exit("cddl-validate: no ```cddl block found under Appendix A")
    return b.group(1)

APPA = extract_appendix_a(SPEC_MD)

def rule_bodies(cddl):
    # {rule-name: normalized body}. A rule spans from its `name =` line to the line before
    # the next rule start; ;-comments are stripped and whitespace collapsed so the compare is
    # on SEMANTICS, not formatting. A name-set-only parity would miss a rule whose *definition*
    # (not just its name) drifted between harp.cddl and Appendix A.
    out, name, buf = {}, None, []
    for raw in cddl.splitlines():
        line = re.sub(r';.*$', '', raw)
        m = re.match(r'^([a-z][a-z0-9-]*)\s*=(.*)$', line)
        if m:
            if name is not None:
                out[name] = ' '.join(' '.join(buf).split())
            name, buf = m.group(1), [m.group(2)]
        elif name is not None:
            buf.append(line)
    if name is not None:
        out[name] = ' '.join(' '.join(buf).split())
    return out

# sub-component / primitive rules that never appear as a standalone top-level wire artifact —
# they are validated TRANSITIVELY through a parent sample (identity embeds vendor/product/engine/
# channel-map/latency-profile/txn-limits/semver; recall-bundle embeds identity-expectation; object
# is the blob/list/tree/snapshot group-choice, each sampled; uint64 rides tstamp/ref).
TRANSITIVE = {"vendor", "product", "engine", "channel-map", "latency-profile",
              "identity-expectation", "txn-limits", "semver", "uint64", "object"}

def f32(x):  # CBOR float32 (major 7, ai 26) — the wire encodes param values as float32
    return b"\xfa" + struct.pack(">f", x)

H33 = b"\x00" * 33          # hash = bstr .size 33 (1 algo byte + 32-byte SHA-256)

# representative wire artifacts -> their normative rules (checked against BOTH schemas)
vendor  = {0: 0x1209, 1: "HARP Project"}
product = {0: 0x4852, 1: "harp-refdev"}
engine  = {0: "refdev-synth", 1: "2.1.0", 2: H33}
identity = {0: vendor, 1: product, 2: "PI4B-0002", 3: "0.1.0", 4: engine,
            5: [48000, 2], 6: ["harp-core", "harp-recall"],
            7: [{0: 0, 1: 1, 2: "main"}], 8: [{0: 48000, 1: 256, 2: 768}],
            13: {0: 4, 1: 256}}
idexp = {0: vendor, 1: product, 3: engine}
ARTIFACTS = [
    ("envelope",    cbor2.dumps({0: 0, 1: 1, 2: "core.hello"}),            "core.hello request"),
    ("envelope",    cbor2.dumps({0: 1, 1: 1, 2: "core.hello", 3: {}}),     "hello response (with body)"),
    ("error-body",  cbor2.dumps({0: "denied", 1: "pre-hello"}),           "error body"),
    ("tstamp",      cbor2.dumps([1, 480]),                                 "tstamp [epoch,msc]"),
    ("param-event", b"\xa2\x00\x07\x01" + f32(0.6),                        "param-event {id,f32}"),
    ("param-event", b"\xa3\x00\x07\x01" + f32(0.6) + b"\x04\x05",          "param-event +txn-id"),
    ("mod-event",   b"\xa2\x00\x07\x01" + f32(0.25),                       "mod-event"),
    ("txn-begin",   cbor2.dumps({0: 5}),                                   "txn-begin"),
    ("txn-commit",  cbor2.dumps({0: 5, 1: [2, 4800]}),                     "txn-commit +tstamp"),
    ("txn-abort",   cbor2.dumps({0: 5}),                                   "txn-abort"),
    ("blob",        cbor2.dumps({0: 0, 1: "text", 2: b"hello"}),           "blob object"),
    ("list",        cbor2.dumps({0: 1, 1: "snap", 2: [H33, H33]}),         "list object"),
    ("tree",        cbor2.dumps({0: 2, 1: {"params": [H33, 3]}}),          "tree object"),
    ("snapshot",    cbor2.dumps({0: 3, 1: H33, 2: [H33], 3: 7, 4: "refdev-synth", 5: "2.1.0"}), "snapshot object"),
    ("ref",         cbor2.dumps({0: "live", 1: H33, 2: 3, 3: True}),       "ref (live)"),
    ("ref",         cbor2.dumps({0: "project", 1: None, 2: 0, 3: False}),  "ref (null hash)"),
    ("identity",    cbor2.dumps(identity),                                 "identity (hello response)"),
    ("recall-bundle",
     cbor2.dumps({0: "harpb", 1: 1, 2: idexp, 3: [{0: "live", 1: H33, 2: 3, 3: True}], 4: None}),
     "recall-bundle"),
    # standalone message/event/object rules — each carries its own positive wire sample so the
    # gate's "all conform" is honest (a rule used on the wire but never sampled is a silent gap):
    ("event",           cbor2.dumps([[1, 480], 0, {0: 7}]),                "event [tstamp,etype,body]"),
    ("ramp-event",      b"\xa3\x00\x07\x01" + f32(0.8) + b"\x02" + cbor2.dumps([2, 4800]), "ramp-event"),
    ("transport-event", cbor2.dumps({0: 0, 1: 120.0, 2: [4, 480], 3: 96000}), "transport-event (tempo/pos)"),
    ("param",           cbor2.dumps({0: 7, 1: "Cutoff", 4: [0.0, 1.0], 5: 1}), "param descriptor"),
    ("params-rsp",      cbor2.dumps({0: H33, 1: [{0: 7, 1: "Cutoff", 4: [0.0, 1.0]}], 2: 12}), "params-rsp (evt.params)"),
    ("rt-profile",      cbor2.dumps({0: 320, 1: 64}),                      "rt-profile (§8.7 setpoints)"),
    ("ump-group-map",   cbor2.dumps([{0: 0, 1: "notes"}]),                 "ump-group-map (§9.10)"),
    ("hash",            cbor2.dumps(H33),                                  "hash (bstr .size 33)"),
]

def run_suite(cddl, name):
    fails = []
    try:
        Schema(cddl)
        print(f"{name}: well-formed ✓")
    except Exception as e:
        sys.exit(f"{name}: PARSE FAILED — {e}")
    for rule, data, label in ARTIFACTS:
        try:
            Schema(f"start = {rule}\n{cddl}").validate_cbor(data)
            print(f"  ✓ {label:32s} conforms to `{rule}`")
        except Exception as e:
            print(f"  ✗ {label:32s} FAILED `{rule}`: {str(e).splitlines()[0][:120]}")
            fails.append(f"{name}:{label}")
    return fails

# (0) parity: Appendix A IS the normative schema; harp.cddl is its extract. They MUST define the
# same rules with the same BODIES, or one has drifted — exactly how `uint64` slipped out of
# Appendix A. Body-level (not just name-level) so a changed definition can't pass unnoticed.
b_cddl, b_appa = rule_bodies(CDDL), rule_bodies(APPA)
if set(b_cddl) != set(b_appa):
    sys.exit("cddl-validate: harp.cddl and spec Appendix A DEFINE DIFFERENT RULES — "
             f"only in harp.cddl: {sorted(set(b_cddl) - set(b_appa)) or '—'}; "
             f"only in Appendix A: {sorted(set(b_appa) - set(b_cddl)) or '—'}")
body_diffs = [r for r in sorted(b_cddl) if b_cddl[r] != b_appa[r]]
if body_diffs:
    sys.exit(f"cddl-validate: harp.cddl and Appendix A AGREE on rule names but DISAGREE on bodies: {body_diffs}\n"
             + "\n".join(f"    {r}:\n      harp.cddl   = {b_cddl[r]}\n      Appendix A  = {b_appa[r]}" for r in body_diffs))
print(f"rule parity: harp.cddl ≡ spec Appendix A — {len(b_cddl)} rules, names AND bodies match ✓")

# (0b) coverage honesty: every standalone (non-transitive) rule MUST carry a positive wire sample,
# so "all wire samples conform" is not a blanket over an unsampled rule. A new standalone rule with
# no sample fails the gate rather than passing silently.
sampled = {a[0] for a in ARTIFACTS}
allrules = set(b_cddl)
unsampled = allrules - sampled - TRANSITIVE
if unsampled:
    sys.exit(f"cddl-validate: standalone rule(s) with NO positive wire sample: {sorted(unsampled)} — "
             "add a sample to ARTIFACTS, or classify it TRANSITIVE if it only rides a parent")
print(f"rule coverage: {len(sampled & allrules)}/{len(allrules)} rules directly sampled, "
      f"{len(TRANSITIVE & allrules)} validated transitively via a parent artifact ✓")

# (1) well-formedness + (2) wire-artifact conformance, against BOTH normative surfaces
fails = run_suite(CDDL, "harp.cddl") + run_suite(APPA, "spec/harp-spec.md Appendix A (normative)")

if fails:
    sys.exit(f"\ncddl-validate: {len(fails)} sample(s) do NOT conform: {', '.join(fails)}")
print(f"\ncddl-validate: harp.cddl + spec Appendix A — body-parity, {len(sampled & allrules)}/{len(allrules)} rules "
      f"directly sampled (rest transitive), well-formed, all samples conform ✓")
