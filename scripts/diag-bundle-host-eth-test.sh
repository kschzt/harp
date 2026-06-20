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
#   - §14.4 host-context-B: key 6 (session-history) + key 8 (runtime logs) present
#     and well-formed — the §12.1 DETACHED->...->STREAMING lifecycle and the
#     runtime log records the rings captured.
#   - §16: a SECOND, --diag-bundle-anon capture clears state-transition.4 (detail)
#     and log-record.3 (msg) to "" while RETAINING the log tag (reveal whether, not
#     what), proving the anon pass reaches the new keys 6/8 leaves.
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
BUNDLE_ANON=/tmp/db-host-anon.cbor   # §16: a second capture with --diag-bundle-anon
fail() { echo "DIAG-BUNDLE-HOST FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

# Run the host once, capturing a bundle. $1 = the --diag-bundle* flag, $2 = outfile.
# Hang-proof: each run is wrapped in a 30s perl-alarm and waited on by pid.
run_host() {
  local flag="$1" out="$2"
  rm -f "$out"; : > "$HOSTLOG"
  echo "── run the host (§8.7 Ethernet) with $flag -> $out (a few blocks rendered, then captured)"
  HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" \
    perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 2 --realtime \
      "$flag" "$out" >"$HOSTLOG" 2>&1 & HP=$!
  wait "$HP"; local rc=$?; HP=""
  [ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "host HUNG (perl-alarm watchdog fired)"; }
  [ "$rc" -eq 0 ] || { cat "$HOSTLOG"; fail "host exited rc=$rc"; }
  grep -q "connected:" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host never connected"; }
  [ -s "$out" ] || { cat "$HOSTLOG"; fail "no diag bundle written to $out (capture didn't run?)"; }
}

rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"; rm -f "$BUNDLE" "$BUNDLE_ANON"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# Plain capture, then a §16 anonymized capture (same device, sequential — one port,
# never concurrent, so the device stays single-owner).
run_host --diag-bundle "$BUNDLE"
run_host --diag-bundle-anon "$BUNDLE_ANON"

# CBOR text strings are stored literal, so grep on raw bytes finds them. -a = treat binary as text.
grep -aq "harpd"      "$BUNDLE" || fail "bundle missing magic 'harpd' (top key 0)"
grep -aq "usb_errors" "$BUNDLE" || fail "bundle missing device-section counter 'usb_errors' (key 4 not embedded verbatim?)"
grep -aq "$SERIAL"    "$BUNDLE" || fail "bundle missing device serial '$SERIAL' (device-section identity not embedded?)"
# HOST-section marker: bundle-meta key 1 names the host assembler — present ONLY
# because the HOST layered its own sections on top of the device body.
grep -aq "harp-runtime" "$BUNDLE" || fail "bundle missing host assembler marker 'harp-runtime' (host-section not layered — just a device re-emit?)"
# §14.4 host-context-B token grep: the runtime-log "session" tag + the audio.start
# tag are CBOR text leaves only the host rings produce (keys 6/8). Their presence
# proves the rings were drained (the decode block below proves the shape).
grep -aq "session"    "$BUNDLE" || fail "bundle missing 'session' tag (key 6/8 rings not drained?)"
grep -aq "audio.start" "$BUNDLE" || fail "bundle missing 'audio.start' tag (key 6/8 rings not drained?)"

# Decode-level assertion: keys 5/6/8/9 PRESENT and well-formed. Prefer a CBOR
# decoder; fall back to a structural sanity check if no Perl CBOR module is
# installed (CI has it; dev boxes may not).
if perl -MCBOR::XS -e 1 >/dev/null 2>&1; then
  perl -MCBOR::XS -e '
    local $/; open my $fh, "<:raw", $ARGV[0] or die "open: $!";
    my $b = CBOR::XS->new->decode(<$fh>);
    ref $b eq "HASH" or die "top-level is not a CBOR map\n";
    $b->{0} eq "harpd" or die "key 0 != harpd\n";
    $b->{1} == 2       or die "key 1 (version) != 2 (host-context-B adds keys 6/8): $b->{1}\n";
    exists $b->{4}     or die "key 4 (device-section) absent\n";
    ref $b->{4} eq "HASH" or die "device-section is not a map\n";
    exists $b->{4}{1}  or die "device-section key 1 (counters) absent\n";
    exists $b->{4}{1}{usb_errors} or die "device-section counters missing usb_errors\n";
    # key 5: host-counters — keys 0..6 present, all numeric
    exists $b->{5}     or die "key 5 (host-counters) absent\n";
    ref $b->{5} eq "HASH" or die "host-counters is not a map\n";
    for my $k (0..6) { exists $b->{5}{$k} or die "host-counters key $k absent\n"; }
    # key 6: session-history — an array of well-formed state-transition maps. A
    # streaming session has at least the DETACHED->...->STREAMING lifecycle.
    exists $b->{6}     or die "key 6 (session-history) absent (transition ring not drained?)\n";
    ref $b->{6} eq "ARRAY" or die "session-history (key 6) is not an array\n";
    @{$b->{6}} >= 1    or die "session-history (key 6) is empty\n";
    my $sawStream = 0;
    for my $t (@{$b->{6}}) {
      ref $t eq "HASH" or die "state-transition is not a map\n";
      ref $t->{0} eq "ARRAY" && @{$t->{0}} == 2 or die "state-transition key 0 (tstamp) not [epoch,msc]\n";
      exists $t->{1} && exists $t->{2} or die "state-transition missing from/to state\n";
      exists $t->{3} or die "state-transition missing reason (key 3)\n";
      exists $t->{4} or die "state-transition missing detail (key 4)\n";
      $sawStream = 1 if $t->{2} == 4;   # to-state STREAMING
    }
    $sawStream or die "session-history never reached STREAMING (to-state 4)\n";
    # key 8: runtime logs — an array of well-formed log-record maps, tags non-empty.
    exists $b->{8}     or die "key 8 (runtime logs) absent (log ring not drained?)\n";
    ref $b->{8} eq "ARRAY" or die "runtime logs (key 8) is not an array\n";
    @{$b->{8}} >= 1    or die "runtime logs (key 8) is empty\n";
    for my $l (@{$b->{8}}) {
      ref $l eq "HASH" or die "log-record is not a map\n";
      exists $l->{0} && exists $l->{1} or die "log-record missing msc/level\n";
      defined $l->{2} && length $l->{2} or die "log-record tag (key 2) empty\n";
      exists $l->{3} or die "log-record missing msg (key 3)\n";
    }
    # key 9: audio-config — present while a session is up; spot-check the shape
    exists $b->{9}     or die "key 9 (audio-config) absent (session not up at capture?)\n";
    ref $b->{9} eq "HASH" or die "audio-config is not a map\n";
    $b->{9}{0} == 48000 or die "audio-config sample-rate (key 0) != 48000: $b->{9}{0}\n";
    $b->{9}{1} == 256   or die "audio-config block (key 1) != 256: $b->{9}{1}\n";
    ref $b->{9}{3} eq "ARRAY" or die "audio-config out-slots (key 3) not an array\n";
    print "   decode OK: key0 harpd, key1 v2, key4.device-section(usb_errors+serial), key5.host-counters(0..6), key6.session-history(", scalar(@{$b->{6}}), " transitions ->STREAMING), key8.runtime-logs(", scalar(@{$b->{8}}), " records), key9.audio-config(rate=48000,block=256,out-slots=[@{$b->{9}{3}}])\n";

    # §16 anon assertion: the SECOND bundle must clear every state-transition.4
    # (detail) and log-record.3 (msg) to "" while RETAINING tags/levels/states.
    open my $afh, "<:raw", $ARGV[1] or die "open anon: $!";
    my $a = CBOR::XS->new->decode(<$afh>);
    $a->{3} or die "anon bundle key 3 (anonymized flag) not true\n";
    my $retainedTag = 0;
    for my $t (@{$a->{6}}) {
      defined $t->{4} && $t->{4} eq "" or die "anon: state-transition detail (key 4) not cleared to empty string\n";
      exists $t->{1} && exists $t->{2} && exists $t->{3} or die "anon: transition lost states/reason (over-redacted)\n";
    }
    for my $l (@{$a->{8}}) {
      defined $l->{3} && $l->{3} eq "" or die "anon: log-record msg (key 3) not cleared to empty string\n";
      defined $l->{2} && length $l->{2} or die "anon: log-record tag (key 2) was cleared — tag MUST be retained (§16)\n";
      $retainedTag = 1;
    }
    $retainedTag or die "anon: no log record to prove tag retention\n";
    print "   anon OK: every transition.4 detail + log.3 msg cleared to \"\"; tags/levels/states RETAINED\n";
  ' "$BUNDLE" "$BUNDLE_ANON" || fail "CBOR decode/assert failed"
else
  echo "   (CBOR::XS not installed — structural grep only; CI runs the full decode)"
  # Structural fallback for the §16 anon clearing without a CBOR decoder: the
  # PLAIN bundle carries the descriptive detail/msg text ("streaming",
  # "audio stream negotiated"); the ANON bundle must NOT — while BOTH keep the
  # greppable "session" tag. (Token-level, but proves the leaf clearing ran.)
  grep -aq "streaming"            "$BUNDLE"      || fail "plain bundle missing 'streaming' detail/msg (rings not drained?)"
  grep -aq "audio stream negotiated" "$BUNDLE"   || fail "plain bundle missing 'audio stream negotiated' log msg"
  grep -aq "session"              "$BUNDLE_ANON" || fail "anon bundle lost the 'session' tag — tag MUST be retained (§16)"
  grep -aq "audio stream negotiated" "$BUNDLE_ANON" && fail "anon bundle still carries the log msg 'audio stream negotiated' — §16 msg clearing did not run"
  echo "   anon grep OK: 'session' tag retained, descriptive detail/msg gone after --diag-bundle-anon"
fi

kill -9 "$DP" 2>/dev/null; DP=""
echo "DIAG-BUNDLE-HOST PASS (host getDiagBundle v2: 'harpd' + verbatim device-section (usb_errors+$SERIAL) + host-counters key5 + session-history key6 + runtime-logs key8 + audio-config key9, well-formed; §16 anon clears detail/msg keeps tags; $(wc -c <"$BUNDLE") bytes)"
