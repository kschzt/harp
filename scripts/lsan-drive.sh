#!/bin/sh
# lsan-drive — the LeakSanitizer gate for the long-running device DAEMON.
#
# Why this exists: the daemon's only sanitizer coverage (eth-asan.yml) runs with
# ASAN_OPTIONS=detect_leaks=0, and the two DoS soaks (abuse-test.sh, conn-flood-test.sh)
# SIGKILL the daemon mid-session — so a genuine allocation that harp-deviced never frees
# was UNPROVEN either way. LSan reports at process EXIT, which needs a CLEAN exit; the DoS
# soaks can't give it one. This driver does: it starts an ASan+leaks-ON harp-deviced with
# HARP_CLEAN_EXIT=1 (SIGTERM RETURNS from main() -> atexit -> the LSan leak check runs),
# drives a bounded but representative real session (multiple connect/disconnect cycles,
# recall save/restore + §11.3 CAS + §9.6 txn, §9 events, a param set, a short RTP audio
# render, a diag bundle), then SIGTERMs it and REQUIRES (a) a clean rc 0 AND (b) no LSan
# "detected memory leaks" line in the daemon log. Either signal fails the gate.
#
# SCOPE — the gate is the DAEMON, not the CLI tools. harp-deviced runs with detect_leaks=1
# (the gate). The short-lived harp-probe / eth-audio tools run with detect_leaks=0: they are
# throwaway processes that exit immediately and are KNOWN to leak their CLI scratch at exit
# (this is exactly the rationale abuse-test.sh:17 already documents), which is irrelevant to
# "does the all-day audio daemon leak". They still keep FULL AddressSanitizer memory-error
# checking (only the exit-time LEAK report is silenced for them) so a real use-after-free /
# overflow in a tool still aborts. Run this against an ASan build (see .github/workflows/ci.yml
# job `lsan`); on macOS the build works but LSan does not RUN (Linux-only) — the Linux CI job
# is the oracle. LSAN_OPTIONS points at .lsan-suppressions.txt when present (currently empty:
# the daemon's process-lifetime state hangs off the `g_dev` global, so LSan sees it as still-
# reachable, not leaked — no suppression is needed and none should be added for a real leak).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build-lsan/harp-deviced}"
PROBE="${PROBE:-./build-lsan/harp-probe}"
RTP="${RTP:-./build-lsan/harp-eth-rtp-test}"
PORT="${PORT:-18078}"
WORK="${WORK:-/tmp/harp-lsan-drive}"
HOST_STORE="$WORK/host"
SECS="${SECS:-2}"   # RTP audio-drive duration (short: the alloc paths are exercised regardless)
SUPP="$PWD/.lsan-suppressions.txt"

# CLI tools (probe + audio exercisers): full ASan memory-error checking, but NO exit-leak
# report (they are throwaway one-shot processes — see SCOPE above). abort_on_error=1 keeps a
# real memory bug fatal. This is the ONLY place detect_leaks=0 appears, and it is NOT the gate.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:print_stacktrace=1${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
# The DAEMON's options (the gate): leaks ON by DEFAULT. exitcode=23 makes a leak a DISTINCT
# non-zero rc. Overridable ONLY to smoke the script plumbing where LSan can't RUN (macOS aborts
# on detect_leaks=1 — it is Linux-only): a local `DEV_ASAN_OPTIONS=detect_leaks=0 ...` run proves
# the drive/clean-exit logic without the leak check. The Linux CI `lsan` job never overrides it,
# so the gate on CI is always leaks-ON — the override cannot silently weaken the per-PR gate.
DEV_ASAN_OPTIONS="${DEV_ASAN_OPTIONS:-detect_leaks=1:abort_on_error=1:print_stacktrace=1:exitcode=23}"
DEV_LSAN_OPTIONS="report_objects=1"
[ -s "$SUPP" ] && DEV_LSAN_OPTIONS="$DEV_LSAN_OPTIONS:suppressions=$SUPP"

fail() { echo "LSAN-DRIVE FAIL: $1"; [ -f "$WORK/dev.log" ] && { echo "── device log ──"; tail -40 "$WORK/dev.log"; }; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built (configure an ASan build: -DHARP_ASAN=ON)"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
cleanup() { [ -n "$DP" ] && { kill -TERM "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""; }; }
trap cleanup EXIT INT TERM
rm -rf "$WORK"; mkdir -p "$HOST_STORE"

# start_dev : launch a clean-exit ASan+leaks-ON daemon, wait for "listening on $PORT", set $DP
start_dev() {
  : > "$WORK/dev.log"
  env ASAN_OPTIONS="$DEV_ASAN_OPTIONS" LSAN_OPTIONS="$DEV_LSAN_OPTIONS" HARP_CLEAN_EXIT=1 \
    "$DEVICED" --port "$PORT" --state-dir "$WORK/dev" --panel-sock "$WORK/panel.sock" \
    >>"$WORK/dev.log" 2>&1 &
  DP=$!
  i=0
  while [ "$i" -lt 50 ]; do
    grep -q "listening on $PORT" "$WORK/dev.log" 2>/dev/null && return 0
    kill -0 "$DP" 2>/dev/null || { cat "$WORK/dev.log"; fail "daemon died before listening"; }
    i=$((i + 1)); sleep 0.2
  done
  cat "$WORK/dev.log"; fail "daemon did not start on $PORT"
}
# stop_dev : SIGTERM the daemon; the clean-exit path runs the LSan leak check at process exit.
# REQUIRE rc 0 (exitcode=23 => a leak; any non-zero => not a clean, leak-free shutdown) AND
# assert the log carries no LSan leak report (belt-and-suspenders vs. rc handling differences).
stop_dev() {
  [ -n "$DP" ] || return 0
  kill -TERM "$DP" 2>/dev/null
  wait "$DP"; rc=$?
  DP=""
  if grep -qE "LeakSanitizer: detected memory leaks|ERROR: LeakSanitizer" "$WORK/dev.log"; then
    echo "── LSan report ──"; sed -n '/LeakSanitizer/,$p' "$WORK/dev.log"
    fail "daemon leaked (LeakSanitizer report above)"
  fi
  [ "$rc" = 0 ] || fail "daemon did not exit cleanly (rc=$rc); rc=23 => LSan leak, other => crash/abort"
}

P() { "$PROBE" -d "127.0.0.1:$PORT" -s "$HOST_STORE" "$@"; }
# passq PATTERN PROBE_ARGS... : run a probe subcommand and REQUIRE the substring (captured to a
# var, not a pipe, so fail()'s exit aborts the whole script — a probe regression fails the lane).
passq() {
  _pat="$1"; shift
  _out=$(P "$@" 2>&1)
  printf '%s\n' "$_out" | grep -q "$_pat" || { printf '%s\n' "$_out"; fail "probe '$*': expected '$_pat'"; }
}
# best-effort audio exerciser: EXERCISE the audio_loop/engine alloc paths; never fail on its rc
# (fidelity is gated in eth.yml). perl alarm bounds a wedge into a finite skip.
audio() { echo "──── audio: $*"; perl -e 'alarm 30; exec @ARGV' "$@" 2>&1 | tail -2; }

echo "════════ LSan drive: bounded real daemon session (leaks ON, clean exit) ════════"
start_dev

# §5 hello / identity / liveness / introspection — allocate + free the ctl request/response path
P identify >/dev/null || fail "identify (no connect?)"
P ping 2   >/dev/null || fail "ping"
P refs     >/dev/null || true
P counters >/dev/null || true
P params   >/dev/null || true
P meters   >/dev/null || true
P knob 0 0.5 >/dev/null 2>&1 || true                 # §9 param set -> dirties live ref (engine apply; best-effort like the coverage drive)
P diag-bundle "$WORK/diag.cbor" >/dev/null || true   # §14.4 bundle codec (CBOR alloc)

# §11 recall: save (demo), restore, §11.3 CAS reject/override, §9.6 txn, §5.5 core — the store
# object/blob alloc + write + read-back paths (the heaviest daemon allocator).
passq "recall complete" demo
passq "SYNCED silently" restore
passq "CAS-TEST PASS"   cas-test
passq "TXN-TEST PASS"   txn-test
passq "CORE-TEST PASS"  core-test

# §7.1/§9 events: epoch on rate change + stale discard, unknown-notif ignore, evt.format — the
# per-event evq alloc/drain path (a classic place for a per-message leak to hide).
passq "EPOCH-TEST PASS"  epoch-test
passq "NOTIF-TEST PASS"  notif-test
passq "FORMAT-TEST PASS" format-test

# §11.4 reconcile relay through the AF_UNIX front panel (panel.c post/poll alloc)
P reconcile-offer aa11 bb22 1 >/dev/null 2>&1 || true
P reconcile-poll              >/dev/null 2>&1 || true

# §8.7 free-running RTP, synth voices (held note 60) — audio thread + engine render + RTP emit
# allocate their buffers here; a clean exit must reclaim/keep-reachable all of them.
[ -x "$RTP" ] && audio "$RTP" "127.0.0.1:$PORT" "$SECS" 47813 60

# A SECOND connect/disconnect cycle: a per-session leak accumulates across sessions, so drive
# another full session before the clean exit to make any per-session retention visible to LSan.
P identify >/dev/null || fail "identify (2nd session)"
passq "recall complete" demo
passq "SYNCED silently" restore

stop_dev
echo "LSAN-DRIVE PASS: daemon driven through a bounded real session and clean-exited LEAK-FREE"
