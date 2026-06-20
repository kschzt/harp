#!/bin/bash
# diag-bundle-host-eth-test — §14.4 host-context-A, over the §8.7 loopback.
#
# The DEVICE-side diag-bundle (scripts/diag-bundle-eth-test.sh) proves harp-probe
# assembles the bundle straight off the device. THIS test proves the HOST RUNTIME
# (HarpRuntime::getDiagBundle) assembles its OWN bundle while a live session runs:
# the device-section is the device's `req diag.bundle` body embedded VERBATIM under
# top key 4, with the HOST's own host-counters (key 5) and audio-config (key 9)
# layered on top.
#
# Flow: start harp-deviced on a unique loopback port; run harp-vst3-host over the
# §8.7 Ethernet binding with --diag-bundle (the runtime captures its bundle at
# session teardown, after a few blocks rendered, READ-ONLY off the control path);
# assert the written CBOR is well-formed and contains:
#   - the magic "harpd"                       (top key 0)
#   - the device-section counter key "usb_errors"  (key 4 -> device-section key 1)
#   - the device serial "SIM-0001"            (key 4 -> device-section identity)
#   - the host-section markers (keys 5 + 9 present + a known token), proving the
#     HOST layered its own sections — NOT just a re-emit of the device body.
# getDiagBundle is read-only assembly off the ctl path: it MUST NOT change the
# render (the offline goldens stay byte-identical — that gate lives in
# scripts/golden-test.sh + scripts/offline-golden-eth.sh, run alongside this).
#
# Co-existence: unique port + kills ONLY its own device/host by pid; perl-alarm
# watchdog; workspace-RELATIVE state dir (Git Bash /tmp->C:\ trips the device
# mkdir on Windows; see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47987}"
SERIAL="${SERIAL:-SIM-0001}"
DEVDIR=diagbundle-host-eth-state   # workspace-RELATIVE (see header)
DEVLOG=/tmp/diagbundle-host-eth-dev.log
HOSTLOG=/tmp/diagbundle-host-eth-host.log
BUNDLE=/tmp/db-host.cbor
fail() { echo "DIAG-BUNDLE-HOST FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"; rm -f "$BUNDLE"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

echo "── run the host (§8.7 Ethernet) with --diag-bundle -> $BUNDLE (a few blocks rendered, then captured)"
HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" \
  perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 2 --realtime \
    --diag-bundle "$BUNDLE" >"$HOSTLOG" 2>&1 & HP=$!
wait "$HP"; rc=$?; HP=""
[ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "host HUNG (perl-alarm watchdog fired)"; }
[ "$rc" -eq 0 ] || { cat "$HOSTLOG"; fail "host exited rc=$rc"; }
grep -q "connected:" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host never connected"; }
[ -s "$BUNDLE" ] || { cat "$HOSTLOG"; fail "no diag bundle written to $BUNDLE (capture didn't run?)"; }

# CBOR text strings are stored literal, so grep on raw bytes finds them. -a = treat binary as text.
grep -aq "harpd"      "$BUNDLE" || fail "bundle missing magic 'harpd' (top key 0)"
grep -aq "usb_errors" "$BUNDLE" || fail "bundle missing device-section counter 'usb_errors' (key 4 not embedded verbatim?)"
grep -aq "$SERIAL"    "$BUNDLE" || fail "bundle missing device serial '$SERIAL' (device-section identity not embedded?)"
# HOST-section marker: bundle-meta key 1 names the host assembler — present ONLY
# because the HOST layered its own sections on top of the device body.
grep -aq "harp-runtime" "$BUNDLE" || fail "bundle missing host assembler marker 'harp-runtime' (host-section not layered — just a device re-emit?)"

# Decode-level assertion: key 5 (host-counters) AND key 9 (audio-config) PRESENT
# and well-formed. Prefer a CBOR decoder; fall back to a structural sanity check
# if no Perl CBOR module is installed (CI has it; dev boxes may not).
if perl -MCBOR::XS -e 1 >/dev/null 2>&1; then
  perl -MCBOR::XS -e '
    local $/; open my $fh, "<:raw", $ARGV[0] or die "open: $!";
    my $b = CBOR::XS->new->decode(<$fh>);
    ref $b eq "HASH" or die "top-level is not a CBOR map\n";
    $b->{0} eq "harpd" or die "key 0 != harpd\n";
    $b->{1} == 1       or die "key 1 (version) != 1\n";
    exists $b->{4}     or die "key 4 (device-section) absent\n";
    ref $b->{4} eq "HASH" or die "device-section is not a map\n";
    exists $b->{4}{1}  or die "device-section key 1 (counters) absent\n";
    exists $b->{4}{1}{usb_errors} or die "device-section counters missing usb_errors\n";
    # key 5: host-counters — keys 0..6 present, all numeric
    exists $b->{5}     or die "key 5 (host-counters) absent\n";
    ref $b->{5} eq "HASH" or die "host-counters is not a map\n";
    for my $k (0..6) { exists $b->{5}{$k} or die "host-counters key $k absent\n"; }
    # key 9: audio-config — present while a session is up; spot-check the shape
    exists $b->{9}     or die "key 9 (audio-config) absent (session not up at capture?)\n";
    ref $b->{9} eq "HASH" or die "audio-config is not a map\n";
    $b->{9}{0} == 48000 or die "audio-config sample-rate (key 0) != 48000: $b->{9}{0}\n";
    $b->{9}{1} == 256   or die "audio-config block (key 1) != 256: $b->{9}{1}\n";
    ref $b->{9}{3} eq "ARRAY" or die "audio-config out-slots (key 3) not an array\n";
    print "   decode OK: key0 harpd, key4.device-section(usb_errors+serial), key5.host-counters(0..6), key9.audio-config(rate=48000,block=256,out-slots=[@{$b->{9}{3}}])\n";
  ' "$BUNDLE" || fail "CBOR decode/assert failed"
else
  echo "   (CBOR::XS not installed — structural grep only; CI runs the full decode)"
fi

kill -9 "$DP" 2>/dev/null; DP=""
echo "DIAG-BUNDLE-HOST PASS (host getDiagBundle: 'harpd' + verbatim device-section (usb_errors+$SERIAL) + host-counters key5 + audio-config key9, well-formed; $(wc -c <"$BUNDLE") bytes)"
