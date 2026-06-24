#!/bin/bash
# engine-mismatch-eth-test — §12.2 engine-major-change read-only default, over the §8.7 loopback.
#
# Per §12.2, when a device's ENGINE MAJOR differs from what the host last saw across a
# (re)connect, the staged project state may not fit the new engine: the runtime must
#   (1) RECORD the change — a state-transition with reason ENGINE_MAJOR_MISMATCH=4, AND
#   (2) HOLD the staged project state read-only — SKIP the connect-time auto-push, so a
#       project that was about to be re-asserted is NOT applied until the user re-applies
#       it explicitly.
# This proves BOTH halves: detection (1) AND enforcement (2). Enforcement only fires when
# a bundle is actually staged (the auto-push gate is `if (haveBundle && readOnlyDefault_)`),
# so we --save-state a fixture first, then --load-state it (which sets hasBundle_) into the
# runs that must gate on it. Without the staged bundle the sessionUp half could be deleted
# and a detection-only test would still pass — so the fixture is load-bearing.
#
# We can't hot-swap the refdev's engine mid-test, so the runtime exposes ONE seam:
# HARP_FORCE_ENGINE_MAJOR seeds the "last-seen" major to a value that differs from the
# device's real one, forcing the mismatch on a single connect. Asserts:
#   FORCED (seam + staged bundle): host logs the helloAndIdentity detection ("engine
#     major 99 -> ...") AND the DISTINCT sessionUp enforcement line ("...not auto-applied");
#     does NOT log "project state re-asserted"; diag-bundle session-history (key 6) carries
#     a reason-4 transition.
#   CONTROL (no seam + same staged bundle): host logs "project state re-asserted" (the
#     auto-push fired); does NOT log the enforcement line; NO reason-4 transition.
# The control proves the seam — and only the seam — gates the push: same staged bundle,
# same device, the only difference is the forced major.
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
STATE=/tmp/engine-mismatch.state    # --save-state fixture, --load-state'd to stage a bundle
BUNDLE=/tmp/engine-mismatch.cbor
fail() { echo "ENGINE-MISMATCH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

# run_host <force> <label> <extra host args...>  — force "" = no seam (control). Hang-proof:
# 30s perl-alarm, waited by pid. Leaves output in $HOSTLOG for the caller to assert on.
run_host() {
  local force="$1" label="$2"; shift 2
  : > "$HOSTLOG"
  echo "── run the host (§8.7 Ethernet) [$label] HARP_FORCE_ENGINE_MAJOR='${force:-<unset>}' $*"
  HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" HARP_FORCE_ENGINE_MAJOR="$force" \
    HARP_CONSENT_ENGINE_MAJOR="${HARP_CONSENT_ENGINE_MAJOR:-}" \
    perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 2 --realtime "$@" >"$HOSTLOG" 2>&1 & HP=$!
  wait "$HP"; local rc=$?; HP=""
  [ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "[$label] host HUNG (perl-alarm watchdog fired)"; }
  [ "$rc" -eq 0 ] || { cat "$HOSTLOG"; fail "[$label] host exited rc=$rc"; }
  grep -q "connected:" "$HOSTLOG" || { cat "$HOSTLOG"; fail "[$label] host never connected"; }
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

rm -rf "$DEVDIR"; : > "$DEVLOG"; rm -f "$STATE"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# ── 0) Stage a project bundle fixture: a plain session that saves its state. This is what
#       later --load-state runs re-assert on connect (sets hasBundle_, arming the auto-push).
run_host "" save-fixture --save-state "$STATE"
[ -s "$STATE" ] || { cat "$HOSTLOG"; fail "save-state produced no fixture at $STATE"; }
echo "   ✓ staged a project-state fixture ($(wc -c <"$STATE") bytes)"

# ── 1) FORCED mismatch WITH the staged bundle: detection logged, enforcement gates the
#       auto-push (state held read-only, NOT re-asserted), reason-4 recorded.
# --set-at 1.0:3=0.9 is a LIVE mid-render param write (§15.5): while held read-only it MUST be
# dropped (re-audit HIGH #1 — the write gate), surfaced by the sessionDown "suppressed" summary.
run_host 99 forced --load-state "$STATE" --diag-bundle "$BUNDLE" --set-at 1.0:3=0.9
grep -q "engine major 99 " "$HOSTLOG" || { cat "$HOSTLOG"; fail "host did not detect the forced engine-major change (helloAndIdentity)"; }
grep -q "not auto-applied" "$HOSTLOG"    || { cat "$HOSTLOG"; fail "host did not SKIP the auto-push (sessionUp enforcement line absent — read-only gate not taken)"; }
grep -q "project state re-asserted" "$HOSTLOG" && { cat "$HOSTLOG"; fail "host RE-ASSERTED the project state despite the engine-major change (auto-push not held)"; }
grep -q "read-only: suppressed" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host did NOT block the live --set-at param write while read-only (§11.4/§12.2 write gate not taken — HIGH #1)"; }
if perl -MCBOR::XS -e 1 >/dev/null 2>&1; then assert_reason4 "$BUNDLE" 1 "forced-mismatch history missing reason 4"
else echo "   (CBOR::XS absent — host-log gate only; CI runs the decode)"; fi
echo "   ✓ forced engine-major change: detected + auto-push HELD read-only + live writes BLOCKED + reason-4 recorded"

# ── 2) CONTROL: same staged bundle, NO seam => matching engine => auto-push FIRES, no gate.
run_host "" control --load-state "$STATE" --diag-bundle "$BUNDLE.ctl" --set-at 1.0:3=0.9
grep -q "project state re-asserted" "$HOSTLOG" || { cat "$HOSTLOG"; fail "control run did NOT re-assert the staged project state (auto-push should fire on a matching engine)"; }
grep -q "not auto-applied" "$HOSTLOG" && { cat "$HOSTLOG"; fail "control run wrongly held state read-only (false positive on a matching engine)"; }
grep -q "read-only: suppressed" "$HOSTLOG" && { cat "$HOSTLOG"; fail "control run wrongly SUPPRESSED a live param write on a matching engine (false read-only — write gate over-fired)"; }
if perl -MCBOR::XS -e 1 >/dev/null 2>&1; then assert_reason4 "$BUNDLE.ctl" 0 "control run wrongly recorded reason 4"; fi
echo "   ✓ control (matching engine): auto-push fired (re-asserted) + live writes ALLOWED — no read-only, no reason-4"

# ── 2b) CONSENT (§13.4 / med-consent-unreachable): the SAME forced engine-major mismatch as (1),
#        but the user grants consent (HARP_CONSENT_ENGINE_MAJOR=1 — the conformance seam for the
#        §11.4 "Force (consent)" action / consentEngineMajorOverride()). The §13.4/§12.2 read-only
#        hold MUST LIFT: the project re-asserts and live writes are allowed (the push now carries
#        flags bit 2). Without consent the identical scenario (1) is held — consent is the override
#        that makes the engine difference reachable.
# export+unset (NOT a VAR=val prefix: prefixing a shell FUNCTION leaks the assignment into later phases).
export HARP_CONSENT_ENGINE_MAJOR=1 HARP_RECONCILE_TIMEOUT_MS=0  # consent + headless (no panel to poll)
run_host 99 consent --load-state "$STATE" --diag-bundle "$BUNDLE.consent" --set-at 1.0:3=0.9
unset HARP_CONSENT_ENGINE_MAJOR HARP_RECONCILE_TIMEOUT_MS       # must NOT leak into fresh-open / serial-differs
grep -q "project state re-asserted" "$HOSTLOG" || { cat "$HOSTLOG"; fail "consent: project NOT re-asserted despite consent (the §13.4 engine hold did not lift — med-consent-unreachable)"; }
grep -q "not auto-applied" "$HOSTLOG" && { cat "$HOSTLOG"; fail "consent: project still HELD read-only despite consent (override ignored)"; }
grep -q "read-only: suppressed" "$HOSTLOG" && { cat "$HOSTLOG"; fail "consent: live writes still suppressed despite consent (write gate not lifted)"; }
echo "   ✓ consent (HARP_CONSENT_ENGINE_MAJOR): the §13.4 hold LIFTED — project re-asserted + live writes ALLOWED"

# ── 3) FRESH-OPEN bundle-baseline (§12.2, NO seam): a project saved against a major-1 engine,
#       opened on this major-2 device, must default read-only WITHOUT HARP_FORCE_ENGINE_MAJOR.
#       The baseline is the BUNDLE's recorded engine major — the central "loads but sounds
#       different without consent" guarantee, distinct from the across-reconnect force path
#       (1)/(2). The --engine-ver device seam stages the major-1 save.
kill -9 "$DP" 2>/dev/null; DP=""
rm -rf "$DEVDIR.v1"; : > "$DEVLOG"
echo "── stage a major-1 bundle: device reporting engine 1.0.0 (--engine-ver)"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --engine-ver 1.0.0 --state-dir "$DEVDIR.v1" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "v1 (engine 1.0.0) device didn't start on $PORT"; }
run_host "" save-v1 --save-state "$STATE.v1"
[ -s "$STATE.v1" ] || { cat "$HOSTLOG"; fail "save-state (engine 1.0.0) produced no fixture"; }
kill -9 "$DP" 2>/dev/null; DP=""
rm -rf "$DEVDIR.v2"; : > "$DEVLOG"
echo "── open it on the default-engine (2.0.0) device with NO HARP_FORCE_ENGINE_MAJOR"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR.v2" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "v2 (default engine) device didn't start on $PORT"; }
run_host "" fresh-open --load-state "$STATE.v1" --diag-bundle "$BUNDLE.fresh"
grep -q "not auto-applied" "$HOSTLOG" || { cat "$HOSTLOG"; fail "fresh-open: a major-1 project on a major-2 device was NOT held read-only (§12.2 fresh-open gate, no force)"; }
grep -q "project state re-asserted" "$HOSTLOG" && { cat "$HOSTLOG"; fail "fresh-open: RE-ASSERTED a mismatched-engine project (read-only gate not taken)"; }
if perl -MCBOR::XS -e 1 >/dev/null 2>&1; then assert_reason4 "$BUNDLE.fresh" 1 "fresh-open history missing reason 4"; fi
echo "   ✓ fresh-open major-1 bundle on major-2 device: held read-only WITHOUT force + reason-4"

# ── 4) SERIAL DIFFERS (§12.2, re-audit HIGH #4): the SAME project (saved on $SERIAL) opened on a
#       DIFFERENT physical unit (different serial, SAME engine) MUST default read-only — the
#       project must not silently auto-push onto another unit (it has its own state).
kill -9 "$DP" 2>/dev/null; DP=""
rm -rf "$DEVDIR.alt"; : > "$DEVLOG"
echo "── open the $SERIAL project on a DIFFERENT unit (serial SIM-ALT-0002, same engine)"
"$DEVICED" --serial "SIM-ALT-0002" --port "$PORT" --state-dir "$DEVDIR.alt" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "alt-serial device didn't start on $PORT"; }
run_host "" serial-differs --load-state "$STATE" --diag-bundle "$BUNDLE.serial"
grep -q "bound a different unit" "$HOSTLOG" || { cat "$HOSTLOG"; fail "serial-differs: host did not detect the different unit (serial mismatch undetected)"; }
grep -q "not auto-applied" "$HOSTLOG"        || { cat "$HOSTLOG"; fail "serial-differs: project NOT held read-only on a different unit (auto-push not skipped — HIGH #4)"; }
grep -q "project state re-asserted" "$HOSTLOG" && { cat "$HOSTLOG"; fail "serial-differs: RE-ASSERTED the project onto a DIFFERENT unit (the §12.2 serial gate was not taken)"; }
echo "   ✓ serial differs: $SERIAL project on SIM-ALT-0002 held read-only (not auto-pushed onto another unit)"

kill -9 "$DP" 2>/dev/null; DP=""
echo "ENGINE-MISMATCH PASS (§12.2: engine-major change [force + fresh-open] AND serial-differs [different unit] -> held read-only [auto-push skipped]; matching engine re-asserts)"
