#!/bin/bash
# diag-bundle-eth-test — §14.4 diag-bundle + §16 anonymization, over the §8.7 loopback.
# harp-probe diag-bundle is CAP-GATED: after do_hello it scans identity capabilities for
# "diag.bundle". harp-deviced ADVERTISES it (device/session.c:248), so the plain run takes
# the DEVICE-ASSEMBLED path — the host issues `req diag.bundle` and embeds the device's full
# device-section (identity keys 0-12 incl channel-map + latency-profile, plus counters)
# VERBATIM under top key 4. (A device WITHOUT the cap would fall back to the v0 host-synth
# subset — that path is unchanged but is not exercised here, since the refdev has the cap.)
#   Plain run: assert the CBOR CONTAINS the magic "harpd", a counter key "usb_errors", the
#     device serial "SIM-0001", the "device-assembled" provenance marker (bundle-meta key 1),
#     AND a FULL-IDENTITY token the host-synth path lacked — the channel-map entry NAME
#     "Part 1 L" (device/session.c encode_identity emits "Part %d %c" for per-part output
#     slots, identity key 7 entry key 2; the v0 host-synth identity carries no channel-map).
#     Grep raw bytes — CBOR text strings are stored literal.
#   --anonymize: assert "SIM-0001" + the vendor NAME + the channel-map name "Part 1 L" are
#     all ABSENT — the §16 decode-reencode seam exception cleared the identity serial,
#     vendor/product names, build-id, AND the channel-map entry name/group/path leaves to
#     "" in place — while "harpd" + "usb_errors" + the device-assembled marker are STILL
#     PRESENT (top structure / path / counters preserved; only PII LEAVES cleared, not
#     omitted — §16).
# This is the CI assertion the schema mandates: CDDL alone cannot prove the §16 pass ran
# (a "" and a real string are both tstr).
#
# §4.2 / §14.4 device LOG STREAM (device-section key 2): the daemon routes its own log
# lines through a bounded (>= 64 KiB) ring (device/log_ring.c) and drains a recent window
# into device-section key 2 as [* log-record]. The daemon's startup line logs the serial +
# the --state-dir PATH, so the PLAIN bundle carries that path token (proving a real device
# log line landed) plus the machine-greppable log TAG "daemon"; the --anonymize pass clears
# each log-record msg (key 3) to "" (§16 log free-text), so the PATH token DISAPPEARS while
# the TAG (key 2, reveal-whether-not-what) is RETAINED.
#
# Co-existence: unique port + kills ONLY its own device by pid; hard perl-alarm watchdog.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17983}"
SERIAL="${SERIAL:-SIM-0001}"
VNAME="${VNAME:-HARP Reference Project}"   # device vendor NAME (§16: cleared under --anonymize; vendor_id is retained)
FULLID="${FULLID:-Part 1 L}"   # FULL-identity token: channel-map (key 7) entry NAME (entry key 2)
                            # for part 1's left output, emitted by device/session.c encode_identity
                            # ("Part %d %c"). Present ONLY on the device-assembled section (the v0
                            # host-synth subset has no channel-map), and UNIQUE to the channel-map —
                            # it is not a substring of any retained field (caps/counters/engine/fw/
                            # build-id/serial/vendor/product). The §16 pass CLEARS channel-map
                            # name/group/path leaves, so this token DISAPPEARS under --anonymize.
LOGTAG="${LOGTAG:-daemon}"   # §4.2/§14.4 device log-record TAG (key 2): the daemon lifecycle tag.
                            # RETAINED under --anonymize (machine-greppable, reveals whether not what);
                            # unique to the log stream — not a substring of any retained field.
DEVDIR=diagbundle-eth-state   # workspace-RELATIVE (Git Bash /tmp->C:\ trips the device mkdir; see eth-tests.sh)
                            # ALSO the §4.2/§14.4 log MSG token: the daemon startup line logs the
                            # --state-dir PATH, unique to a device log-record msg (key 3) — cleared to
                            # "" under --anonymize (§16 log free-text).
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

echo "── harp-probe diag-bundle -> $BUNDLE (device-assembled: refdev advertises diag.bundle)"
perl -e 'alarm 15; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" diag-bundle "$BUNDLE" >"$PROBELOG" 2>&1 \
    || { cat "$PROBELOG"; fail "diag-bundle command failed (rc=$?)"; }
[ -s "$BUNDLE" ] || fail "no bundle written to $BUNDLE"

# CBOR text strings are stored literal, so grep on raw bytes finds them. -a = treat binary as text.
grep -aq "harpd"   "$BUNDLE" || fail "bundle missing magic 'harpd'"
grep -aq "usb_errors" "$BUNDLE" || fail "bundle missing counter key 'usb_errors'"
grep -aq "$SERIAL" "$BUNDLE" || fail "bundle missing device serial '$SERIAL' (identity not embedded?)"
grep -aq "device-assembled" "$BUNDLE" || fail "bundle missing the device-assembled provenance marker (bundle-meta key 1) — cap gate didn't engage?"
grep -aq "$VNAME" "$BUNDLE" || fail "bundle missing vendor name '$VNAME' (identity not embedded?)"
# FULL-identity token: only the device-assembled section carries the channel-map (key 7).
grep -aq "$FULLID" "$BUNDLE" || fail "bundle missing full-identity channel-map token '$FULLID' (device-section not embedded verbatim?)"
echo "   plain bundle OK: 'harpd' + 'usb_errors' + serial + vendor-name + device-assembled marker + full-identity '$FULLID' all present ($(wc -c <"$BUNDLE") bytes)"

# §4.2/§14.4 device LOG STREAM (device-section key 2): the daemon routed its own startup log
# line through the >= 64 KiB ring and drained it into the bundle. Assert the machine-greppable
# log TAG landed AND the startup-log --state-dir PATH token landed (unique to a log-record msg).
grep -aq "$LOGTAG" "$BUNDLE" || fail "bundle missing device log tag '$LOGTAG' — device-section key 2 log stream not emitted (§4.2/§14.4)?"
grep -aq "$DEVDIR" "$BUNDLE" || fail "bundle missing device log msg path token '$DEVDIR' — daemon startup log line not captured in the ring?"
echo "   device log stream OK: tag '$LOGTAG' + startup-log path token '$DEVDIR' embedded at device-section key 2"

echo "── harp-probe diag-bundle --anonymize -> $ANONBUNDLE (§16 serial-clearing pass)"
perl -e 'alarm 15; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" diag-bundle --anonymize "$ANONBUNDLE" >"$PROBELOG" 2>&1 \
    || { cat "$PROBELOG"; fail "diag-bundle --anonymize command failed (rc=$?)"; }
[ -s "$ANONBUNDLE" ] || fail "no anonymized bundle written to $ANONBUNDLE"

# §16 seam exception: the device-section was decode-reencoded, clearing the serial + vendor-name
# + channel-map name/group/path leaves to "" in place while PRESERVING top structure (magic),
# the device-assembled path marker, and the counters map.
grep -aq "$SERIAL"    "$ANONBUNDLE" && fail "anonymized bundle STILL contains serial '$SERIAL' — §16 pass did not clear it"
grep -aq "$VNAME"     "$ANONBUNDLE" && fail "anonymized bundle STILL contains vendor name '$VNAME' — §16 did not clear the name"
grep -aq "$FULLID"    "$ANONBUNDLE" && fail "anonymized bundle STILL contains channel-map name '$FULLID' — §16 did not clear the channel-map name/group/path leaves"
grep -aq "harpd"      "$ANONBUNDLE" || fail "anonymized bundle lost magic 'harpd' — structure not preserved"
grep -aq "usb_errors" "$ANONBUNDLE" || fail "anonymized bundle lost counter 'usb_errors' — counters wrongly stripped"
grep -aq "device-assembled" "$ANONBUNDLE" || fail "anonymized bundle lost the device-assembled marker — seam exception path not taken"
# §16 device LOG STREAM: the anon pass clears each log-record msg (key 3) to "", so the daemon
# startup-log --state-dir PATH token DISAPPEARS; the log TAG (key 2) is RETAINED (whether-not-what).
grep -aq "$DEVDIR" "$ANONBUNDLE" && fail "anonymized bundle STILL contains device log msg path token '$DEVDIR' — §16 did not clear log-record msg (key 3)"
grep -aq "$LOGTAG" "$ANONBUNDLE" || fail "anonymized bundle lost device log tag '$LOGTAG' — the log tag (key 2) must be RETAINED (§16: reveal whether, not what)"
echo "   anonymized bundle OK: serial + vendor-name + channel-map name '$FULLID' + device-log path '$DEVDIR' ABSENT; 'harpd' + 'usb_errors' + device-assembled marker + log tag '$LOGTAG' still present ($(wc -c <"$ANONBUNDLE") bytes)"

kill -9 "$DP" 2>/dev/null; DP=""
echo "DIAG-BUNDLE PASS (device-assembled embed + verbatim counters; §4.2/§14.4 device log stream at key 2; §16 clears serial + vendor-name + channel-map name/group/path + log msg, retains log tag, structure preserved)"
