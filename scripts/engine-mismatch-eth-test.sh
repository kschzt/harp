#!/bin/bash
# engine-mismatch-eth-test — §12.2 engine-major-change read-only default, over the §8.7 loopback.
#
# Per §12.2, when a device's ENGINE MAJOR differs from what the host last saw across a
# (re)connect, the staged project state may not fit the new engine: the runtime must
# RECORD the change (a state-transition with reason ENGINE_MAJOR_MISMATCH=4) and HOLD
# the project state read-only (skip the connect-time auto-push) until the user re-applies
# it explicitly. This proves the read-only-default half of §12.2 (the serial-differs flow
# is enforced upstream by the host pinning boundSerial_ at selectDevice).
#
# We can't hot-swap the refdev's engine mid-test, so the runtime exposes ONE seam:
# HARP_FORCE_ENGINE_MAJOR seeds the "last-seen" major to a value that differs from the
# device's real one, forcing the mismatch on a single connect. The test asserts:
#   - the host LOGS "engine major <seed> -> <real>: project state held read-only"
#   - the captured diag-bundle's session-history (key 6) carries a transition whose
#     reason (key 3) is ENGINE_MAJOR_MISMATCH (4).
# A CONTROL run WITHOUT the seam asserts NEITHER fires — the normal (matching-engine)
# path stays clean, so the seam can't silently corrupt ordinary operation.
#
# Co-existence: unique port; kills ONLY its own pids; perl-alarm watchdog; workspace-
# RELATIVE state dir (Git Bash /tmp->C:\ trips the device mkdir on Windows; see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47991}"
SERIAL="${SERIAL:-SIM-0001}"
DEVDIR=engine-mismatch-eth-state   # workspace-RELATIVE (see header)
DEVLOG=/tmp/engine-mismatch-eth-dev.log
HOSTLOG=/tmp/engine-mismatch-eth-host.log
BUNDLE=/tmp/engine-mismatch.cbor
fail() { echo "ENGINE-MISMATCH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

# Run the host once with a given HARP_FORCE_ENGINE_MAJOR ("" = control, no seam).
# $1 = forced major (or ""), $2 = outfile. Hang-proof: 30s perl-alarm, waited by pid.
run_host() {
  local force="$1" out="$2"
  rm -f "$out"; : > "$HOSTLOG"
  echo "── run the host (§8.7 Ethernet), HARP_FORCE_ENGINE_MAJOR='${force:-<unset>}' -> $out"
  HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" HARP_FORCE_ENGINE_MAJOR="$force" \
    perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 2 --realtime \
      --diag-bundle "$out" >"$HOSTLOG" 2>&1 & HP=$!
  wait "$HP"; local rc=$?; HP=""
  [ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "host HUNG (perl-alarm watchdog fired)"; }
  [ "$rc" -eq 0 ] || { cat "$HOSTLOG"; fail "host exited rc=$rc"; }
  grep -q "connected:" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host never connected"; }
  [ -s "$out" ] || { cat "$HOSTLOG"; fail "no diag bundle written to $out (capture didn't run?)"; }
}

# Assert the bundle's session-history (key 6) HAS / HASN'T a reason-4 transition.
# $1 = bundle, $2 = want (1=must have, 0=must not), $3 = failure label.
assert_reason4() {
  perl -MCBOR::XS -e '
    local $/; open my $fh, "<:raw", $ARGV[0] or die "open: $!";
    my $b = CBOR::XS->new->decode(<$fh>);
    ref $b->{6} eq "ARRAY" or die "session-history (key 6) absent/not array\n";
    my $saw = 0;
    for my $t (@{$b->{6}}) { $saw = 1 if ref $t eq "HASH" && ($t->{3} // -1) == 4; }
    my $want = $ARGV[1];
    if ($want && !$saw) { die "no ENGINE_MAJOR_MISMATCH (reason 4) transition in history\n"; }
    if (!$want && $saw) { die "UNEXPECTED ENGINE_MAJOR_MISMATCH (reason 4) on a matching engine\n"; }
    print "   history reason-4 present=$saw (want=$want) — OK\n";
  ' "$1" "$2" || fail "$3"
}

rm -rf "$DEVDIR"; : > "$DEVLOG"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# ── 1) FORCED mismatch: seed a different major; the change must be logged + recorded.
run_host 99 "$BUNDLE"
grep -q "engine major 99 ->" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host did not detect the forced engine-major change"; }
grep -q "project state held read-only" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host did not hold project state read-only on engine-major change"; }
if perl -MCBOR::XS -e 1 >/dev/null 2>&1; then assert_reason4 "$BUNDLE" 1 "forced-mismatch history missing reason 4"
else echo "   (CBOR::XS absent — host-log gate only; CI runs the decode)"; fi
echo "   ✓ forced engine-major change: logged + held read-only + recorded (reason 4)"

# ── 2) CONTROL: no seam => the device's own major matches the baseline => NO mismatch.
run_host "" "$BUNDLE.ctl"
grep -q "project state held read-only" "$HOSTLOG" && { cat "$HOSTLOG"; fail "control run wrongly held state read-only (false positive on a matching engine)"; }
if perl -MCBOR::XS -e 1 >/dev/null 2>&1; then assert_reason4 "$BUNDLE.ctl" 0 "control run wrongly recorded reason 4"; fi
echo "   ✓ control (matching engine): clean — no read-only, no reason-4"

kill -9 "$DP" 2>/dev/null; DP=""
echo "ENGINE-MISMATCH PASS (§12.2: engine-major change -> ENGINE_MAJOR_MISMATCH(4) recorded + project state held read-only; matching engine stays clean)"
