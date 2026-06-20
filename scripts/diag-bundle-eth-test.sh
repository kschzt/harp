#!/bin/bash
# diag-bundle-eth-test — §14.4 diag-bundle v0 + §16 anonymization, over the §8.7 loopback.
# Host-only export (ZERO device changes): harp-probe diag-bundle assembles the required
# top-level keys 0..4 from the live do_hello identity + the verbatim diag.counters body.
#   Plain run: assert the CBOR CONTAINS the magic "harpd", a counter key "usb_errors", and
#     the device serial "SIM-0001" (grep raw bytes — CBOR text strings are stored literal).
#   --anonymize: assert "SIM-0001" is ABSENT (the serial leaf was cleared to "" in place)
#     while "harpd" + "usb_errors" are STILL PRESENT (structure/counters preserved, §16).
# This is the CI assertion the schema mandates: CDDL alone cannot prove the §16 pass ran
# (a "" and a real string are both tstr).
#
# Co-existence: unique port + kills ONLY its own device by pid; hard perl-alarm watchdog.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-47983}"
SERIAL="${SERIAL:-SIM-0001}"
VNAME="${VNAME:-HARP Reference Project}"   # device vendor NAME (§16: cleared under --anonymize; vendor_id is retained)
DEVDIR=diagbundle-eth-state   # workspace-RELATIVE (Git Bash /tmp->C:\ trips the device mkdir; see eth-tests.sh)
DEVLOG=/tmp/diagbundle-eth-dev.log
PROBELOG=/tmp/diagbundle-eth-probe.log
BUNDLE=/tmp/db.cbor
ANONBUNDLE=/tmp/db-anon.cbor
fail() { echo "DIAG-BUNDLE FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"; rm -f "$BUNDLE" "$ANONBUNDLE"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

echo "── harp-probe diag-bundle -> $BUNDLE (v0: identity + verbatim counters)"
perl -e 'alarm 15; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" diag-bundle "$BUNDLE" >"$PROBELOG" 2>&1 \
    || { cat "$PROBELOG"; fail "diag-bundle command failed (rc=$?)"; }
[ -s "$BUNDLE" ] || fail "no bundle written to $BUNDLE"

# CBOR text strings are stored literal, so grep on raw bytes finds them. -a = treat binary as text.
grep -aq "harpd"   "$BUNDLE" || fail "bundle missing magic 'harpd'"
grep -aq "usb_errors" "$BUNDLE" || fail "bundle missing counter key 'usb_errors'"
grep -aq "$SERIAL" "$BUNDLE" || fail "bundle missing device serial '$SERIAL' (identity not embedded?)"
grep -aq "host-synthesized" "$BUNDLE" || fail "bundle missing the v0 host-synth provenance marker (bundle-meta key 1)"
grep -aq "$VNAME" "$BUNDLE" || fail "bundle missing vendor name '$VNAME' (identity not embedded?)"
echo "   plain bundle OK: 'harpd' + 'usb_errors' + serial + vendor-name + host-synth marker all present ($(wc -c <"$BUNDLE") bytes)"

echo "── harp-probe diag-bundle --anonymize -> $ANONBUNDLE (§16 serial-clearing pass)"
perl -e 'alarm 15; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" diag-bundle --anonymize "$ANONBUNDLE" >"$PROBELOG" 2>&1 \
    || { cat "$PROBELOG"; fail "diag-bundle --anonymize command failed (rc=$?)"; }
[ -s "$ANONBUNDLE" ] || fail "no anonymized bundle written to $ANONBUNDLE"

# §16: the serial leaf MUST be cleared to "" in place. Structure (magic) + counters retained.
grep -aq "$SERIAL"    "$ANONBUNDLE" && fail "anonymized bundle STILL contains serial '$SERIAL' — §16 pass did not clear it"
grep -aq "$VNAME"     "$ANONBUNDLE" && fail "anonymized bundle STILL contains vendor name '$VNAME' — §16 did not clear the name"
grep -aq "harpd"      "$ANONBUNDLE" || fail "anonymized bundle lost magic 'harpd' — structure not preserved"
grep -aq "usb_errors" "$ANONBUNDLE" || fail "anonymized bundle lost counter 'usb_errors' — counters wrongly stripped"
echo "   anonymized bundle OK: serial + vendor-name ABSENT, 'harpd' + 'usb_errors' still present ($(wc -c <"$ANONBUNDLE") bytes)"

kill -9 "$DP" 2>/dev/null; DP=""
echo "DIAG-BUNDLE PASS (v0 envelope + verbatim counters; §16 serial clear, structure preserved)"
