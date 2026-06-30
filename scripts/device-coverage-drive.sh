#!/bin/sh
# device-coverage-drive — drive a CLEAN-EXIT harp-deviced through a representative slice of its
# behaviour so a --coverage build's device/ .gcda is FLUSHED, giving gcovr a REAL device-daemon
# line/branch number (not a behavioural-only pass/fail).
#
# Why this exists: the eth conformance suite SIGKILLs its daemon, so gcov never flushes the
# device .gcda — device/{session,state,audio_loop,engine}.c + harp-deviced.c read ~0% under the
# gcov-instrumented unit build, which is why the `coverage` job could only GATE core/src/. This
# dedicated driver fixes that: HARP_CLEAN_EXIT=1 makes the daemon's SIGTERM/SIGINT RETURN from
# main() (running the atexit __gcov dump) instead of the production _exit(0) in the signal handler.
# See device/harp-deviced.c (g_graceful_shutdown / g_accept_stop) and the `coverage` job in ci.yml.
#
# What this EXERCISES (so the gcovr device/ gate is a measurement, not behavioural-only):
#   session.c    — the §5/§9/§11 ctl dispatch table; hello/identity/liveness; recall save/restore +
#                  §11.3 CAS reject/override + §9.6 txn; §9 events (epoch/notif/format); core.* ;
#                  counters/params/meters/diag; the §11.4 reconcile relay (via the front panel)
#   state.c      — snapshot/restore, the params-blob codec, content-addressed store round-trips
#   audio_loop.c — host-paced (latefr) AND free-running RTP (rtp/bitexact) render+emit loops
#   engine.c     — the synth render (a held note over RTP) + param/knob apply + the cold golden
#   harp-deviced.c — the accept loop, §16 pre-hello path, RTP/TCP audio dest open, clean shutdown
#   panel.c      — the AF_UNIX front-panel accept/serve + reconcile post/poll
#
# The PROTOCOL probe sequence is DETERMINISTIC and MUST pass (a failure is a real regression). The
# audio tools are best-effort: they are run to EXERCISE audio_loop.c; their FIDELITY/SINAD is gated
# in eth.yml (and on the hw rig), NOT here — so cloud-runner timing jitter can't flake this lane.
# The real gate is the gcovr device/ floor in ci.yml.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build-cov/harp-deviced}"
PROBE="${PROBE:-./build-cov/harp-probe}"
LATEFR="${LATEFR:-./build-cov/harp-eth-latefr-test}"
RTP="${RTP:-./build-cov/harp-eth-rtp-test}"
BITEXACT="${BITEXACT:-./build-cov/harp-eth-bitexact-test}"
FENCE="${FENCE:-./build-cov/harp-eth-fence-test}"
GOLDEN="${ENGINE_GOLDEN:-./build-cov/harp-engine-golden}"
PORT="${PORT:-18077}"
WORK="${WORK:-/tmp/harp-cov-drive}"
HOST_STORE="$WORK/host"
SECS="${SECS:-3}"   # audio-tool duration (short = fast; the paths are covered regardless)

fail() { echo "DEVICE-COVERAGE-DRIVE FAIL: $1"; [ -f "$WORK/dev.log" ] && { echo "── device log ──"; tail -20 "$WORK/dev.log"; }; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built (configure a --coverage build first)"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
PP=""
cleanup() {
  [ -n "$PP" ] && { kill "$PP" 2>/dev/null; wait "$PP" 2>/dev/null; PP=""; }
  [ -n "$DP" ] && { kill -TERM "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""; }
}
trap cleanup EXIT INT TERM
rm -rf "$WORK"; mkdir -p "$HOST_STORE"

# start_dev CMD ARGS... : launch a clean-exit daemon, wait for "listening on $PORT", set $DP
start_dev() {
  : > "$WORK/dev.log"
  HARP_CLEAN_EXIT=1 "$@" >>"$WORK/dev.log" 2>&1 &
  DP=$!
  i=0
  while [ "$i" -lt 50 ]; do
    grep -q "listening on $PORT" "$WORK/dev.log" 2>/dev/null && return 0
    kill -0 "$DP" 2>/dev/null || { cat "$WORK/dev.log"; fail "daemon died before listening"; }
    i=$((i + 1)); sleep 0.2
  done
  cat "$WORK/dev.log"; fail "daemon did not start on $PORT"
}
# stop_dev : SIGTERM the clean-exit daemon and REQUIRE rc 0 (a non-clean exit = no .gcda flush)
stop_dev() {
  [ -n "$DP" ] || return 0
  kill -TERM "$DP" 2>/dev/null
  wait "$DP"; rc=$?
  DP=""
  [ "$rc" = 0 ] || fail "daemon did not exit cleanly (rc=$rc) — .gcda would not flush"
}

P() { "$PROBE" -d "127.0.0.1:$PORT" -s "$HOST_STORE" "$@"; }
# passq PATTERN PROBE_ARGS... : run a probe subcommand and REQUIRE the substring in its output.
# Output is captured to a var (NOT a pipe) so fail()'s exit aborts the whole script, not a subshell —
# i.e. a probe-asserted regression actually fails the lane (no masking).
passq() {
  _pat="$1"; shift
  _out=$(P "$@" 2>&1)
  printf '%s\n' "$_out" | grep -q "$_pat" || { printf '%s\n' "$_out"; fail "probe '$*': expected '$_pat'"; }
}
# best-effort audio exerciser: run to EXERCISE audio_loop.c; never fail the lane on its rc (fidelity
# is gated in eth.yml). The perl alarm bounds a wedge so a hang becomes a finite skip, not a stuck job.
audio() { echo "──── audio: $*"; perl -e 'alarm 30; exec @ARGV' "$@" 2>&1 | tail -2; }

echo "════════ phase 1: protocol + recall + host-paced audio (synth engine) ════════"
start_dev "$DEVICED" --port "$PORT" --state-dir "$WORK/devA" --panel-sock "$WORK/panel.sock"
# §5 hello / identity / liveness / introspection (these print and exit 0; run for coverage)
P identify >/dev/null  || fail "identify (no connect?)"
P ping 2   >/dev/null  || fail "ping"
P refs     >/dev/null  || true
P counters >/dev/null  || true
P params   >/dev/null  || true
P meters   >/dev/null  || true
P knob 0 0.5 >/dev/null || true        # §9 param set -> dirties the live ref, exercises engine apply
P gc       >/dev/null  || true
P diag-bundle "$WORK/diag.cbor" >/dev/null || true
# §11 recall: happy path, then §11.3 CAS reject/override, §9.6 txn, §5.5 core — these are the
# deterministic-over-TCP subcommands the `core` job also asserts, so a failure here is a real
# regression (not masking).
passq "recall complete"             demo
passq "SYNCED silently"             restore
passq "CAS-TEST PASS"               cas-test
passq "TXN-TEST PASS"               txn-test
passq "CORE-TEST PASS"              core-test
# §13.4 engine gate: EXERCISE only (the foreign-engine refusal/consent CORRECTNESS needs the
# eth-suite's two-engine setup — engine-gate-eth-test.sh — and is GATED there, not here).
P engine-gate >/dev/null 2>&1 || true
# §5.4 version negotiation: control accepted, then a FORCED major mismatch -> 'incompatible' + range
passq "VERSION-TEST PASS (control)" version-test
vt=$(HARP_FORCE_PROTO_MAJOR=2 "$PROBE" -d "127.0.0.1:$PORT" -s "$HOST_STORE" version-test 2>&1)
printf '%s\n' "$vt" | grep -q "VERSION-TEST PASS:" || { printf '%s\n' "$vt"; fail "forced-major version-test"; }
# §7.1/§9 events: time.epoch on rate change + stale-epoch discard, unknown-notif ignore, evt.format
passq "EPOCH-TEST PASS"             epoch-test
passq "NOTIF-TEST PASS"             notif-test
passq "FORMAT-TEST PASS"            format-test
# §11.4 reconcile relay through the AF_UNIX front panel (covers panel.c post/poll)
P reconcile-offer aa11 bb22 1 >/dev/null 2>&1 || true
P reconcile-poll              >/dev/null 2>&1 || true
# §8.2 host-paced render+emit loop (audio_loop.c host-paced branch)
[ -x "$LATEFR" ] && audio "$LATEFR" "127.0.0.1:$PORT"
# §8.7 free-running RTP, SYNTH voices (held note 60 -> engine.c voice render in the audio loop)
[ -x "$RTP" ] && audio "$RTP" "127.0.0.1:$PORT" "$SECS" 47811 60
stop_dev
echo "phase 1: device exited cleanly (.gcda flushed)"

echo "════════ phase 2: free-running RTP fidelity tone (audio_loop free-run + audio_rtp_emit) ════════"
start_dev "$DEVICED" --port "$PORT" --tone 440 --state-dir "$WORK/devB" --panel-sock ""
[ -x "$RTP" ]      && audio "$RTP" "127.0.0.1:$PORT" "$SECS" 47812
[ -x "$BITEXACT" ] && audio "$BITEXACT" "127.0.0.1:$PORT" "$SECS" 47821
stop_dev
echo "phase 2: device exited cleanly (.gcda flushed)"

echo "════════ phase 3: §8.3.1 bounded event fence on the real-time host-paced path ════════"
start_dev env HARP_FENCE_FORCE_RT=1 "$DEVICED" --port "$PORT" --state-dir "$WORK/devC" --panel-sock ""
[ -x "$FENCE" ] && audio "$FENCE" "127.0.0.1:$PORT" realtime
stop_dev
echo "phase 3: device exited cleanly (.gcda flushed)"

echo "════════ phase 4: §11 front panel — AF_UNIX serve loop via the real harp-panel.py sidecar ════════"
# Drive panel.c (the §11 front-panel API) end-to-end: start a daemon with a panel socket, run the
# REAL web sidecar against it, and curl its endpoints so the device's panel serve loop handles
# params/refs/counters/snapshot requests. Then clean-exit the daemon so panel.c's .gcda flushes.
# python3+curl gated (skip if absent, like web-panel-test.sh) so the lane stays robust off-Linux.
if command -v python3 >/dev/null 2>&1 && command -v curl >/dev/null 2>&1; then
  HTTPPORT="${HTTPPORT:-48091}"
  start_dev "$DEVICED" --port "$PORT" --state-dir "$WORK/devP" --panel-sock "$WORK/panelP.sock"
  for _ in $(seq 1 25); do [ -S "$WORK/panelP.sock" ] && break; sleep 0.2; done
  python3 web/harp-panel.py "$WORK/panelP.sock" "$HTTPPORT" >"$WORK/panel.log" 2>&1 & PP=$!
  for _ in $(seq 1 30); do curl -fsS "http://127.0.0.1:$HTTPPORT/api/params" >/dev/null 2>&1 && break; sleep 0.2; done
  for ep in params refs counters meters; do
    curl -fsS "http://127.0.0.1:$HTTPPORT/api/$ep" >/dev/null 2>&1 && echo "  panel /api/$ep ok" || echo "  panel /api/$ep soft"
  done
  curl -fsS "http://127.0.0.1:$HTTPPORT/api/snapshot" >/dev/null 2>&1 || true   # panel snapshot action
  curl -fsS "http://127.0.0.1:$HTTPPORT/" >/dev/null 2>&1 || true               # the panel HTML page
  kill "$PP" 2>/dev/null; wait "$PP" 2>/dev/null; PP=""
  stop_dev
  echo "phase 4: device exited cleanly (panel.c .gcda flushed)"
else
  echo "phase 4: SKIP (no python3/curl) — panel.c stays behaviourally-tested in the core job"
fi

echo "════════ phase 5: raw engine render (engine.c cold synth voices, standalone clean exit) ════════"
[ -x "$GOLDEN" ] && "$GOLDEN" >/dev/null 2>&1 && echo "engine-golden: rendered + exited 0" || echo "engine-golden: skipped/soft"

echo "DEVICE-COVERAGE-DRIVE PASS: representative device slice driven, all daemons clean-exited"
