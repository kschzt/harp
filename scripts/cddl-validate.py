#!/usr/bin/env python3
# CDDL wire-validation gate — schema-as-contract for the normative CDDL.
#
# spec/harp.cddl is a MACHINE-READABLE schema, but nothing in CI machine-CHECKED it:
# a rule could drift from the prose / Appendix A / the implementation and no test would
# notice. This gate closes that: it (1) proves harp.cddl is well-formed, and (2) validates
# representative on-the-wire CBOR artifacts against their normative rules with a real CDDL
# validator (pycddl). Behavioral wire tests (corrupt-cbor, round-trip, fuzz) prove the code
# ACCEPTS/REJECTS bytes; this proves the SCHEMA itself matches the wire format.
#
# pycddl roots validation at the schema's first rule, so each sample is checked against a
# tiny "start = <rule>" prepended to harp.cddl (CDDL allows the forward reference).
import struct, sys, os

try:
    from pycddl import Schema
    import cbor2
except ImportError:
    sys.exit("cddl-validate: need `pip install pycddl cbor2`")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CDDL = open(os.path.join(ROOT, "spec", "harp.cddl")).read()

def f32(x):  # CBOR float32 (major 7, ai 26) — the wire encodes param values as float32
    return b"\xfa" + struct.pack(">f", x)

H33 = b"\x00" * 33          # hash = bstr .size 33 (1 algo byte + 32-byte SHA-256)
fails = []

# (1) well-formedness
try:
    Schema(CDDL)
    print("harp.cddl: well-formed ✓")
except Exception as e:
    sys.exit(f"harp.cddl: PARSE FAILED — {e}")

def check(rule, data, label):
    try:
        Schema(f"start = {rule}\n{CDDL}").validate_cbor(data)
        print(f"  ✓ {label:32s} conforms to `{rule}`")
    except Exception as e:
        print(f"  ✗ {label:32s} FAILED `{rule}`: {str(e).splitlines()[0][:120]}")
        fails.append(label)

# (2) representative wire artifacts -> their normative rules
check("envelope",    cbor2.dumps({0: 0, 1: 1, 2: "core.hello"}),            "core.hello request")
check("envelope",    cbor2.dumps({0: 1, 1: 1, 2: "core.hello", 3: {}}),     "hello response (with body)")
check("error-body",  cbor2.dumps({0: "denied", 1: "pre-hello"}),           "error body")
check("tstamp",      cbor2.dumps([1, 480]),                                 "tstamp [epoch,msc]")
# events
check("param-event", b"\xa2\x00\x07\x01" + f32(0.6),                        "param-event {id,f32}")
check("param-event", b"\xa3\x00\x07\x01" + f32(0.6) + b"\x04\x05",          "param-event +txn-id")
check("mod-event",   b"\xa2\x00\x07\x01" + f32(0.25),                       "mod-event")
# §9.6 txn control bodies (my Appendix-A addition)
check("txn-begin",   cbor2.dumps({0: 5}),                                   "txn-begin")
check("txn-commit",  cbor2.dumps({0: 5, 1: [2, 4800]}),                     "txn-commit +tstamp")
check("txn-abort",   cbor2.dumps({0: 5}),                                   "txn-abort")
# state objects + refs
check("blob",        cbor2.dumps({0: 0, 1: "text", 2: b"hello"}),           "blob object")
check("list",        cbor2.dumps({0: 1, 1: "snap", 2: [H33, H33]}),         "list object")
check("tree",        cbor2.dumps({0: 2, 1: {"params": [H33, 3]}}),          "tree object")
check("snapshot",    cbor2.dumps({0: 3, 1: H33, 2: [H33], 3: 7, 4: "refdev-synth", 5: "2.1.0"}), "snapshot object")
check("ref",         cbor2.dumps({0: "live", 1: H33, 2: 3, 3: True}),       "ref (live)")
check("ref",         cbor2.dumps({0: "project", 1: None, 2: 0, 3: False}),  "ref (null hash)")
# identity (the hello-response identity) + recall bundle — the richest artifacts
vendor  = {0: 0x1209, 1: "HARP Project"}
product = {0: 0x4852, 1: "harp-refdev"}
engine  = {0: "refdev-synth", 1: "2.1.0", 2: H33}
identity = {0: vendor, 1: product, 2: "PI4B-0002", 3: "0.1.0", 4: engine,
            5: [48000, 2], 6: ["harp-core", "harp-recall"],
            7: [{0: 0, 1: 1, 2: "main"}], 8: [{0: 48000, 1: 256, 2: 768}],
            13: {0: 4, 1: 256}}
check("identity",    cbor2.dumps(identity),                                 "identity (hello)")
idexp = {0: vendor, 1: product, 3: engine}
check("recall-bundle",
      cbor2.dumps({0: "harpb", 1: 1, 2: idexp, 3: [{0: "live", 1: H33, 2: 3, 3: True}], 4: None}),
      "recall-bundle")

if fails:
    sys.exit(f"\ncddl-validate: {len(fails)} sample(s) do NOT conform to harp.cddl: {', '.join(fails)}")
print("\ncddl-validate: harp.cddl well-formed + all wire samples conform ✓")
