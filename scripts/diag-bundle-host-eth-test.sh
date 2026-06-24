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
#   - §14.4 host-context-C: key 11 (clock-stats) ALWAYS present + well-formed with a
#     recovery mode; over the §8.7 loopback the binding is ETHERNET, so key 13
#     (net-topology, host:port present) is emitted and key 10 (usb-topology) is
#     ABSENT (real-USB-hardware only — the test MUST NOT require it). audio-config
#     key 12 (transport enum) reads 1 (ethernet).
#   - §16: a SECOND, --diag-bundle-anon capture clears state-transition.4 (detail)
#     and log-record.3 (msg) to "" while RETAINING the log tag (reveal whether, not
#     what), proving the anon pass reaches the new keys 6/8 leaves; AND clears
#     net-topology.0 (host:port) to "" while RETAINING clock-stats drift + recovery.
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
    $b->{1} == 4       or die "key 1 (version) != 4 (v4 adds §8.4 path-utilization key 14): $b->{1}\n";
    exists $b->{4}     or die "key 4 (device-section) absent\n";
    ref $b->{4} eq "HASH" or die "device-section is not a map\n";
    exists $b->{4}{1}  or die "device-section key 1 (counters) absent\n";
    exists $b->{4}{1}{usb_errors} or die "device-section counters missing usb_errors\n";
    # §16 device-section identity (key 4 -> 0): the PLAIN bundle carries every PII
    # leaf with its real value. Assert each is PRESENT and non-empty so the anon
    # pass below has something concrete to clear (the §16 gate is plain-PRESENT +
    # anon-CLEARED on the SAME leaf). The retained leaves (engine, vid/pid, hash,
    # caps) are spot-checked here and re-checked unchanged in the anon block.
    my $id = $b->{4}{0};
    ref $id eq "HASH" or die "device-section identity (key 4.0) is not a map\n";
    length($id->{2} // "") or die "identity serial (key 2) empty in PLAIN bundle\n";
    length($id->{0}{1} // "") or die "identity vendor name (vendor.1) empty in PLAIN bundle\n";
    length($id->{1}{1} // "") or die "identity product name (product.1) empty in PLAIN bundle\n";
    length($id->{9} // "") or die "identity build-id (key 9) empty in PLAIN bundle\n";
    ref $id->{7} eq "ARRAY" && @{$id->{7}} or die "identity channel-map (key 7) empty in PLAIN bundle\n";
    length($id->{7}[0]{2} // "") or die "channel-map entry name (7[0].2) empty in PLAIN bundle\n";
    # RETAINED-leaf baseline: engine sub-map (key 4) incl. engine-id (4.0) +
    # param-map-hash (4.2), and the numeric VID/PID live in usb-topology — but on
    # the Ethernet loopback usb-topology is absent, so the device-section identity
    # vendor/product sub-maps carry the numeric VID/PID (vendor.0 / product.0).
    ref $id->{4} eq "HASH" or die "identity engine (key 4) is not a map\n";
    length($id->{4}{0} // "") or die "identity engine-id (4.0) empty — must be RETAINED\n";
    defined $id->{0}{0} or die "identity vendor VID (vendor.0) absent — must be RETAINED\n";
    defined $id->{1}{0} or die "identity product PID (product.0) absent — must be RETAINED\n";
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
    my $sawNeg = 0;
    for my $t (@{$b->{6}}) {
      ref $t eq "HASH" or die "state-transition is not a map\n";
      ref $t->{0} eq "ARRAY" && @{$t->{0}} == 2 or die "state-transition key 0 (tstamp) not [epoch,msc]\n";
      exists $t->{1} && exists $t->{2} or die "state-transition missing from/to state\n";
      exists $t->{3} or die "state-transition missing reason (key 3)\n";
      exists $t->{4} or die "state-transition missing detail (key 4)\n";
      $sawStream = 1 if $t->{2} == 4;   # to-state STREAMING
      $sawNeg = 1 if $t->{2} == 2;      # to-state NEGOTIATED (§12.1)
    }
    $sawStream or die "session-history never reached STREAMING (to-state 4)\n";
    $sawNeg or die "session-history never reached NEGOTIATED (to-state 2) — §12.1\n";
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
    # key 9 key 12: transport enum. Over the §8.7 loopback the binding is ETHERNET (1).
    exists $b->{9}{12} or die "audio-config missing transport enum (key 12) — host-context-C not emitted?\n";
    $b->{9}{12} == 1   or die "audio-config transport (key 12) != 1 (ethernet) over §8.7 loopback: $b->{9}{12}\n";
    # §6.4 key 9 key 6: DAW-domain PDC latency, samples — the real composed figure (ethTargetFrames
    # + event headroom over the rate-locked §8.7 loopback), now reported (was a stale literal 0).
    exists $b->{9}{6} or die "audio-config missing PDC latency (key 6)\n";
    $b->{9}{6} =~ /^\d+$/ && $b->{9}{6} > 0 or die "audio-config PDC latency (key 6) not a positive int: $b->{9}{6}\n";

    # §14.4 host-context-C — key 11: clock-stats, ALWAYS present, with a recovery mode.
    # Over §8.7 the device rate-locks (bit-exact) => recovery 2 (rate-lock) + ratelock-stats(6);
    # a non-rate-lock device => recovery 1 (asrc) + asrc-stats(5). Accept either, but the
    # mode MUST be present and well-formed.
    exists $b->{11}     or die "key 11 (clock-stats) absent — host-context-C clock-stats not emitted (it is ALWAYS required)\n";
    ref $b->{11} eq "HASH" or die "clock-stats (key 11) is not a map\n";
    exists $b->{11}{0}  or die "clock-stats missing clock_drift_ppb (key 0)\n";
    exists $b->{11}{3}  or die "clock-stats missing recovery mode (key 3)\n";
    my $rec = $b->{11}{3};
    $rec == 1 || $rec == 2 or die "clock-stats recovery (key 3) not asrc(1)/rate-lock(2) on a free-running session: $rec\n";
    if ($rec == 2) { ref $b->{11}{6} eq "HASH" && exists $b->{11}{6}{0} or die "clock-stats rate-lock but ratelock-stats (key 6) malformed\n"; }
    if ($rec == 1) { ref $b->{11}{5} eq "HASH" && exists $b->{11}{5}{0} or die "clock-stats asrc but asrc-stats (key 5) malformed\n"; }

    # §14.4 host-context-C — key 13: net-topology, Ethernet binding ONLY. host:port present.
    exists $b->{13}     or die "key 13 (net-topology) absent — Ethernet binding must emit it over §8.7\n";
    ref $b->{13} eq "HASH" or die "net-topology (key 13) is not a map\n";
    defined $b->{13}{0} && length $b->{13}{0} or die "net-topology host:port (key 0) empty — host never resolved the dial target\n";
    $b->{13}{0} =~ /127\.0\.0\.1/ or die "net-topology host:port (key 0) not the loopback dial target: $b->{13}{0}\n";
    exists $b->{13}{2}  or die "net-topology missing jitter-buffer depth (key 2)\n";

    # §14.4 host-context-C — key 10: usb-topology is REAL-USB-HARDWARE ONLY. The §8.7
    # loopback is an Ethernet binding, so key 10 MUST be ABSENT (the test must not require it).
    !exists $b->{10}    or die "key 10 (usb-topology) present on the §8.7 Ethernet loopback — it is USB-hardware-only\n";

    # §8.4 — key 14: path-utilization from the admission ledger. Over the §8.7 loopback the
    # path is "eth:global"; reserved/capacity/per-mille are present + numeric. Captured while
    # streaming, so the live session audio reservation is in the ledger (reserved > 0).
    exists $b->{14}     or die "key 14 (§8.4 path-utilization) absent — admission ledger not emitted\n";
    ref $b->{14} eq "HASH" or die "path-utilization (key 14) is not a map\n";
    defined $b->{14}{0} && $b->{14}{0} eq "eth:global" or die "path-util path-id (key 0) != eth:global on loopback: $b->{14}{0}\n";
    exists $b->{14}{1} && exists $b->{14}{2} && exists $b->{14}{3} or die "path-util missing reserved/capacity/per-mille (keys 1/2/3)\n";
    $b->{14}{1} > 0     or die "path-util reserved (key 1) == 0 while streaming — the audio reservation is not in the ledger\n";
    $b->{14}{2} > 0     or die "path-util capacity (key 2) == 0\n";

    print "   decode OK: key0 harpd, key1 v4, key4.device-section(usb_errors+serial), key5.host-counters(0..6), key6.session-history(", scalar(@{$b->{6}}), " transitions ->STREAMING), key8.runtime-logs(", scalar(@{$b->{8}}), " records), key9.audio-config(rate=48000,block=256,transport=ethernet,out-slots=[@{$b->{9}{3}}]), key11.clock-stats(recovery=$rec,drift=$b->{11}{0}), key13.net-topology(host:port=$b->{13}{0},jitter=$b->{13}{2}), key14.path-util(path=$b->{14}{0},reserved=$b->{14}{1},cap=$b->{14}{2}), key10.usb-topology ABSENT(eth)\n";

    # ── COMPREHENSIVE §16 anon assertion ──────────────────────────────────────
    # The SECOND bundle must clear EVERY emitted PII leaf to "" IN PLACE while
    # RETAINING the numeric/tag/hash structure. We re-decode the anon bundle and
    # walk the SAME leaves the plain block asserted PRESENT, demanding each is now
    # cleared — and every retained leaf is byte-for-meaning unchanged.
    open my $afh, "<:raw", $ARGV[1] or die "open anon: $!";
    my $a = CBOR::XS->new->decode(<$afh>);
    $a->{3} or die "anon bundle key 3 (anonymized flag) not true\n";

    # (1) device-section identity (key 4.0): serial(2), vendor name(0.1), product
    # name(1.1), build-id(9), channel-map per-entry name/group/path(7[*].{2,3,4})
    # ALL cleared to ""; engine(4) incl. engine-id(4.0) + param-map-hash(4.2),
    # numeric VID(0.0)/PID(1.0), caps(6), firmware(3) RETAINED unchanged.
    my $aid = $a->{4}{0};
    ref $aid eq "HASH" or die "anon: device-section identity (key 4.0) is not a map\n";
    $aid->{2} eq "" or die "anon: identity serial (key 2) not cleared to \"\" (§16)\n";
    $aid->{0}{1} eq "" or die "anon: identity vendor name (vendor.1) not cleared to \"\" (§16)\n";
    $aid->{1}{1} eq "" or die "anon: identity product name (product.1) not cleared to \"\" (§16)\n";
    $aid->{9} eq "" or die "anon: identity build-id (key 9) not cleared to \"\" (§16)\n";
    ref $aid->{7} eq "ARRAY" && @{$aid->{7}} == @{$id->{7}} or die "anon: channel-map (key 7) length changed — structure not preserved\n";
    for my $e (@{$aid->{7}}) {
      $e->{2} eq "" or die "anon: channel-map entry name (7[*].2) not cleared to \"\" (§16)\n";
      $e->{3} eq "" or die "anon: channel-map entry group (7[*].3) not cleared to \"\" (§16)\n";
      $e->{4} eq "" or die "anon: channel-map entry path (7[*].4) not cleared to \"\" (§16)\n";
      defined $e->{0} or die "anon: channel-map entry slot (7[*].0) dropped — must be RETAINED\n";
    }
    # RETAINED identity leaves unchanged (reveal device class/type, not its name):
    $aid->{4}{0} eq $id->{4}{0} or die "anon: engine-id (4.0) was altered — must be RETAINED verbatim\n";
    $aid->{4}{2} eq $id->{4}{2} or die "anon: param-map-hash (4.2) was altered — must be RETAINED verbatim (§16)\n";
    $aid->{0}{0} == $id->{0}{0} or die "anon: vendor VID (vendor.0) was altered — must be RETAINED\n";
    $aid->{1}{0} == $id->{1}{0} or die "anon: product PID (product.0) was altered — must be RETAINED\n";
    $aid->{3} eq $id->{3} or die "anon: firmware (key 3) was altered — must be RETAINED\n";
    # device-section counters (key 4.1): the anon pass copies key 1 verbatim (no
    # PII), so the SAME set of counter keys must survive — but these are LIVE
    # numeric gauges captured from two independent sessions, so assert KEY-SET +
    # numeric-TYPE retention, NOT cross-capture value-equality (evt_late et al.
    # legitimately differ between the plain and anon captures). A text value here
    # would be the leak signal.
    my $numlike = sub { my $v = shift; defined $v && !ref($v) && $v =~ /^-?\d+(?:\.\d+)?$/ };
    for my $k (keys %{$b->{4}{1}}) {
      exists $a->{4}{1}{$k} or die "anon: device-section counter '$k' (key 4.1) was dropped — must be RETAINED\n";
      $numlike->($a->{4}{1}{$k}) or die "anon: device-section counter '$k' (key 4.1) is non-numeric after anon — must be RETAINED verbatim\n";
    }
    keys %{$a->{4}{1}} == keys %{$b->{4}{1}} or die "anon: device-section counters (key 4.1) key-set changed — must be RETAINED verbatim\n";

    # (2) host rings: state-transition.4 (detail) + log-record.3 (msg) cleared;
    # tstamp/from/to/reason + tag/level/wall-stamp RETAINED.
    my $retainedTag = 0;
    @{$a->{6}} == @{$b->{6}} or die "anon: session-history (key 6) length changed — structure not preserved\n";
    for my $t (@{$a->{6}}) {
      defined $t->{4} && $t->{4} eq "" or die "anon: state-transition detail (key 4) not cleared to empty string\n";
      exists $t->{1} && exists $t->{2} && exists $t->{3} or die "anon: transition lost states/reason (over-redacted)\n";
      ref $t->{0} eq "ARRAY" && @{$t->{0}} == 2 or die "anon: transition tstamp (key 0) dropped — must be RETAINED\n";
    }
    @{$a->{8}} == @{$b->{8}} or die "anon: runtime-logs (key 8) length changed — structure not preserved\n";
    for my $l (@{$a->{8}}) {
      defined $l->{3} && $l->{3} eq "" or die "anon: log-record msg (key 3) not cleared to empty string\n";
      defined $l->{2} && length $l->{2} or die "anon: log-record tag (key 2) was cleared — tag MUST be retained (§16)\n";
      defined $l->{1} or die "anon: log-record level (key 1) dropped — must be RETAINED\n";
      ref $l->{4} eq "ARRAY" && @{$l->{4}} == 2 or die "anon: log-record wall-stamp (key 4) dropped — must be RETAINED\n";
      $retainedTag = 1;
    }
    $retainedTag or die "anon: no log record to prove tag retention\n";

    # (3) net-topology (key 13): host:port(0) cleared; jitter depth(2) + net.offline(5) RETAINED.
    exists $a->{13} or die "anon: key 13 (net-topology) absent — anon must preserve structure\n";
    defined $a->{13}{0} && $a->{13}{0} eq "" or die "anon: net-topology host:port (key 0) not cleared to \"\" (§16)\n";
    $a->{13}{2} == $b->{13}{2} or die "anon: net-topology jitter depth (key 2) changed — must be RETAINED\n";
    exists $a->{13}{5} or die "anon: net-topology net.offline (key 5) dropped — must be RETAINED\n";

    # (4) §8.4 path-utilization (key 14): path-id(0) cleared; reserved(1)/capacity(2)/per-mille(3)
    # RETAINED (reveal how-much, not the controller name — §16).
    exists $a->{14} or die "anon: key 14 (path-utilization) absent — anon must preserve structure\n";
    defined $a->{14}{0} && $a->{14}{0} eq "" or die "anon: path-util path-id (key 0) not cleared to \"\" (§16)\n";
    $a->{14}{1} == $b->{14}{1} or die "anon: path-util reserved (key 1) changed — must be RETAINED\n";
    $a->{14}{2} == $b->{14}{2} or die "anon: path-util capacity (key 2) changed — must be RETAINED\n";

    # (4) clock-stats (key 11): NO PII — the anon pass leaves it untouched. Its
    # leaves are LIVE numeric gauges (drift, recovery, asrc/ratelock sub-stats), so
    # assert the key-set + numeric type survive, NOT cross-capture value-equality.
    exists $a->{11} or die "anon: key 11 (clock-stats) absent — anon must preserve it (no PII)\n";
    exists $a->{11}{0} or die "anon: clock-stats drift (key 0) dropped — must be RETAINED (§16: reveal whether, not what)\n";
    exists $a->{11}{3} or die "anon: clock-stats recovery mode (key 3) dropped — must be RETAINED\n";
    for my $k (keys %{$b->{11}}) {
      exists $a->{11}{$k} or die "anon: clock-stats key $k dropped — no PII, must be RETAINED\n";
      ref $b->{11}{$k} eq "HASH"
        ? (ref $a->{11}{$k} eq "HASH" or die "anon: clock-stats sub-map (key $k) flattened — must be RETAINED\n")
        : ($numlike->($a->{11}{$k}) or die "anon: clock-stats key $k is non-numeric after anon — must be RETAINED\n");
    }

    # (5) host-counters (key 5): all numeric, no PII — anon leaves them untouched.
    # Live session gauges, so assert key-set 0..6 + numeric type, not value-equality.
    for my $k (0..6) {
      exists $a->{5}{$k} or die "anon: host-counter key $k dropped — must be RETAINED\n";
      $numlike->($a->{5}{$k}) or die "anon: host-counter key $k non-numeric after anon — must be RETAINED\n";
    }

    print "   anon OK: identity{serial,vendor.name,product.name,build-id,chan-map name/group/path}=\"\"; transition.4 detail + log.3 msg + net.host:port=\"\"; RETAINED verbatim: engine-id, param-map-hash, VID/PID, caps, firmware, counters, tags/levels/states/wall-stamps, clock-stats(drift/recovery), net jitter/offline\n";
  ' "$BUNDLE" "$BUNDLE_ANON" || fail "CBOR decode/assert failed"
else
  echo "   (CBOR::XS not installed — EXHAUSTIVE §16 grep gate; CI also runs the full decode)"
  # ── COMPREHENSIVE §16 grep gate (no CBOR::XS dependency) ───────────────────
  # Walk EVERY PII string the refdev actually emits over the §8.7 loopback and
  # assert PRESENT-in-plain / ABSENT-in-anon, then assert the RETAINED tokens are
  # PRESENT-in-BOTH. CBOR text leaves are stored literally, so `grep -aF` (fixed
  # string, binary-as-text) finds them by raw bytes.
  #
  # Tokens are SUBSTRING-COLLISION-SAFE — each is unique to its leaf and never a
  # substring of a RETAINED token (which would make an ABSENT assertion a false
  # pass). Notably the bare product name "harp-refdev" is a substring of the
  # RETAINED counter keys "x.harp-refdev.*" and the cap "x.harp-refdev.sim", so it
  # CANNOT prove product-name clearing; instead we grep the host transition/log
  # context "harp-refdev (serial" (produced only by the hello-ok detail/msg, which
  # the anon pass clears). Likewise the channel-map name uses the full "Part 1 L"
  # (not "part1"), and the engine-id "refdev-synth" (retained, hyphen) does not
  # collide with the build-id "refdev sim " (cleared, space).
  #
  # EMITTED PII (device-section identity + host rings + net-topology), PRESENT in
  # plain, ABSENT in anon:
  #   serial                 SIM-0001                         (identity key 2)
  #   vendor name            HARP Reference Project           (identity vendor.1)
  #   product name           harp-refdev (serial              (identity product.1, via hello detail/msg)
  #   channel-map name       Part 1 L                         (identity key 7 entry.2)
  #   build-id               refdev sim                       (identity key 9)
  #   host:port              127.0.0.1:$PORT                  (net-topology key 0)
  #   log msg                audio stream negotiated          (key 8 log-record.3)
  #   transition detail      reader + event pump up           (key 6 state-transition.4)
  pii_present_absent() { # $1 = token, $2 = human label
    grep -aqF "$1" "$BUNDLE"      || fail "plain bundle missing PII '$1' ($2) — ring/section not emitted?"
    grep -aqF "$1" "$BUNDLE_ANON" && fail "anon bundle STILL carries PII '$1' ($2) — §16 clearing did not run"
    echo "   §16 PII cleared: $2 ('$1') — PRESENT in plain, ABSENT in anon"
  }
  pii_present_absent "SIM-0001"                "device serial (identity key 2)"
  pii_present_absent "HARP Reference Project"  "vendor name (identity vendor.1)"
  pii_present_absent "harp-refdev (serial"     "product name (identity product.1, via hello detail/msg)"
  pii_present_absent "Part 1 L"                "channel-map name (identity key 7 entry.2)"
  pii_present_absent "refdev sim "             "build-id (identity key 9)"
  pii_present_absent "127.0.0.1:$PORT"         "net-topology host:port (key 13.0)"
  pii_present_absent "eth:global"              "§8.4 path-util path-id (key 14.0)"
  pii_present_absent "audio stream negotiated" "runtime-log msg (key 8 log-record.3)"
  pii_present_absent "reader + event pump up"  "state-transition detail (key 6 transition.4)"
  # RETAINED tokens (reveal whether/type, NOT value) — PRESENT in BOTH bundles:
  #   session            log-record tag (key 8 log-record.2) — greppable identifier
  #   usb_errors         device-section counter key (key 4 -> device-section key 1)
  #   refdev-synth       engine-id (identity key 4.0) — class id, the "vid/pid"-grade
  #                      device-section text token (the numeric VID 0x1209 is not
  #                      ASCII-greppable; the decode path asserts vid/pid + clock
  #                      recovery numerics)
  #   x.harp-refdev.sim  capability token (identity key 6) — also retained
  retained_both() { # $1 = token, $2 = human label
    grep -aqF "$1" "$BUNDLE"      || fail "plain bundle missing RETAINED token '$1' ($2)"
    grep -aqF "$1" "$BUNDLE_ANON" || fail "anon bundle dropped RETAINED token '$1' ($2) — over-redacted (§16: reveal whether, not what)"
    echo "   §16 retained: $2 ('$1') — PRESENT in BOTH"
  }
  retained_both "session"           "log tag (key 8 log-record.2)"
  retained_both "usb_errors"        "device counter key (key 4 device-section.1)"
  retained_both "refdev-synth"      "engine-id (identity key 4.0, vid/pid-grade text token)"
  retained_both "x.harp-refdev.sim" "capability token (identity key 6)"
  echo "   anon grep OK: all 9 emitted PII strings cleared in anon; all 4 retained tokens kept in both"
fi

kill -9 "$DP" 2>/dev/null; DP=""
echo "DIAG-BUNDLE-HOST PASS (host getDiagBundle v4: 'harpd' + verbatim device-section (usb_errors+$SERIAL) + host-counters key5 + session-history key6 + runtime-logs key8 + audio-config key9(transport=ethernet) + clock-stats key11(recovery) + net-topology key13(host:port) + §8.4 path-utilization key14(path/reserved/cap); key10 usb-topology ABSENT on the Ethernet loopback (real-hardware-only); §16 anon clears detail/msg + net host:port + path-id, keeps tags + clock-stats drift/recovery + util gauges; $(wc -c <"$BUNDLE") bytes)"
