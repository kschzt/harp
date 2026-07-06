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

def rules(cddl):  # rule names defined (LHS of `name =`), ignoring comment lines
    return set(re.findall(r'^([a-z][a-z0-9-]*)\s*=', cddl, re.M))

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
    ("identity",    cbor2.dumps(identity),                                 "identity (hello)"),
    ("recall-bundle",
     cbor2.dumps({0: "harpb", 1: 1, 2: idexp, 3: [{0: "live", 1: H33, 2: 3, 3: True}], 4: None}),
     "recall-bundle"),
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

# (0) rule-set parity: Appendix A IS the normative schema; harp.cddl is its extract. They MUST
# define the same rules, or one has drifted — exactly how `uint64` slipped out of Appendix A.
r_cddl, r_appa = rules(CDDL), rules(APPA)
if r_cddl != r_appa:
    sys.exit("cddl-validate: harp.cddl and spec Appendix A DEFINE DIFFERENT RULES — "
             f"only in harp.cddl: {sorted(r_cddl - r_appa) or '—'}; "
             f"only in Appendix A: {sorted(r_appa - r_cddl) or '—'}")
print(f"rule-set parity: harp.cddl ≡ spec Appendix A ({len(r_cddl)} rules) ✓")

# (1) well-formedness + (2) wire-artifact conformance, against BOTH normative surfaces
fails = run_suite(CDDL, "harp.cddl") + run_suite(APPA, "spec/harp-spec.md Appendix A (normative)")

if fails:
    sys.exit(f"\ncddl-validate: {len(fails)} sample(s) do NOT conform: {', '.join(fails)}")
print("\ncddl-validate: harp.cddl + spec Appendix A — rule-parity, well-formed, all wire samples conform ✓")
