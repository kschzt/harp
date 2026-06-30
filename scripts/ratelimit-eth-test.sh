#!/bin/bash
# ratelimit-eth-test — §16(b) connection-attempt rate-limit: shed-on-reflood + no-shed-after-hello,
# over the §8.7 loopback.
#
# §16 clause (b): on the Ethernet/IP binding a device MUST rate-limit connection attempts. The
# per-peer penalty ring (device/conn_ratelimit.h) sheds a source that just FAILED pre-hello (a
# half-open / slow-loris that held the single session slot without completing core.hello), while a
# hello-COMPLETING client is NEVER penalized (the PR3 lesson: a global connection-COUNT token bucket
# false-shed the legitimate recovery probe). The shed path EXEMPTS loopback (127.*), and every CI
# test connects from 127.0.0.1 — so the hooks agent's `--force-peer-ip A.B.C.D` (device seam) makes
# the accept loop treat the peer as a NON-loopback IP for the rate-limit decision ONLY, so the shed
# path actually runs under test.
#
# Behavioral oracle (the penalize path is silent — no log/counter, just a close):
#   - shed-on-reflood: a SILENT slow-loris from the forced IP (TCP open, never sends core.hello) is
#     HELD by the device toward its pre-hello deadline (~5s SO_RCVTIMEO), then dropped — which penalizes
#     the IP for ~2s. A RAPID reconnect from the same forced IP within that window is SHED — closed
#     near-INSTANTLY at accept (well under the ~5s the un-penalized slow-loris was held). We assert the
#     penalized reconnect-drop << the first (un-penalized) slow-loris hold.
#   - no-shed-after-hello: a hello-COMPLETING client (harp-probe ping) is never penalized, so a probe
#     immediately AFTER it is still SERVED end-to-end — admitted, not shed.
#
# Co-existence: unique port; kills ONLY its own device by pid; perl/python watchdogs; workspace-
# RELATIVE state dir (Git Bash /tmp->C:\ trips the MinGW device mkdir on Windows; see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17834}"
FORCE_IP="${FORCE_IP:-203.0.113.7}"   # TEST-NET-3 (RFC 5737) — a non-loopback IP the shed path keys on
DEVDIR=ratelimit-eth-state   # workspace-RELATIVE (see header)
STORE=ratelimit-eth-store
DEVLOG=/tmp/ratelimit-eth-dev.log
fail() { echo "RATELIMIT FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"
command -v python3 >/dev/null 2>&1 || { echo "RATELIMIT SKIP: python3 not available"; exit 0; }

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf "$DEVDIR" "$STORE"' EXIT INT TERM
rm -rf "$DEVDIR" "$STORE"; : > "$DEVLOG"

# --force-peer-ip: treat every connecting peer as $FORCE_IP for the §16(b) rate-limit decision ONLY.
echo "── device with --force-peer-ip $FORCE_IP over the §8.7 loopback (port $PORT)"
"$DEVICED" --port "$PORT" --force-peer-ip "$FORCE_IP" --state-dir "$DEVDIR" --panel-sock "" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# (1) shed-on-reflood: a half-open penalizes the forced IP; a rapid reconnect is shed (closed ~instantly).
echo "── §16(b) shed-on-reflood: a reconnect right after a held silent slow-loris is shed (closed << ~5s pre-hello hold)"
python3 - "$PORT" <<'PY' || { echo "RATELIMIT FAIL: shed-on-reflood not enforced"; tail -20 "$DEVLOG"; exit 1; }
import socket, sys, time
port = int(sys.argv[1])

def half_open_drop_time(budget):
    # SLOW-LORIS: open the TCP connection and KEEP IT OPEN, silent — send NOTHING and never
    # complete core.hello. The device must HOLD the open-but-silent socket toward its pre-hello
    # deadline (the 5s SO_RCVTIMEO within the 10s total budget), not drop it instantly. (Sending a
    # bogus frame header instead would trip the §4.2 frame decode -> an instant protocol-violation
    # reset at ~0s, which is NOT the slow-loris hold this oracle measures.) Measure how long until
    # the device closes our side (recv b'' EOF) or resets — that wall-clock is the held window.
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    t0 = time.time(); s.settimeout(budget)
    try:
        got = s.recv(64)
    except socket.timeout:
        got = b'__TIMEOUT__'
    except ConnectionResetError:
        got = b'__RESET__'   # device reset our silent half-open at its pre-hello deadline
    dt = time.time() - t0; s.close()
    return dt, got

# first half-open: NOT yet penalized -> the device HOLDS this silent slow-loris toward its pre-hello
# deadline (the 5s SO_RCVTIMEO), then drops it. It must clearly survive (be held) for seconds — if it
# dropped at ~0s the device is NOT holding the open-but-silent socket (the slow-loris isn't realised).
dt1, got1 = half_open_drop_time(8.0)
if got1 == b'__TIMEOUT__':
    sys.exit("first silent half-open NOT dropped within 8s — the daemon hung (pre-hello timeout missing)")
if dt1 < 1.0:
    sys.exit("first silent half-open dropped after only %.2fs — the device did NOT hold the open-but-silent slow-loris toward its pre-hello deadline" % dt1)
print("   first half-open (un-penalized, silent slow-loris) HELD for %.2fs then dropped" % dt1)
# the device penalizes $FORCE_IP for ~2s AFTER run_session returns. Reconnect IMMEDIATELY: it must be
# SHED — closed near-instantly (the penalized branch closes at accept, before the pre-hello budget arms).
dt2, got2 = half_open_drop_time(8.0)
if got2 == b'__TIMEOUT__':
    sys.exit("reconnect during the penalty window was NOT shed — held open (rate-limit not applied)")
print("   reconnect during penalty window dropped after %.2fs" % dt2)
# DISCRIMINATOR: the penalized reconnect (shed at accept, ~0s) must drop MUCH faster than the un-penalized
# silent half-open was HELD (~5s pre-hello deadline). Require the reconnect to be well under 1s in absolute
# terms (a shed is ~0s; a held silent slow-loris is the full ~5s) AND under half the first hold.
if not (dt2 < 1.0 and dt2 < dt1 * 0.5):
    sys.exit("reconnect drop %.2fs not << first hold %.2fs — the forced IP was NOT shed on reflood (§16(b))" % (dt2, dt1))
print("   ✓ shed-on-reflood: penalized reconnect shed in %.2fs vs the un-penalized %.2fs slow-loris hold" % (dt2, dt1))
PY

# Let the 2s penalty window lapse so the hello path below is not collaterally shed.
perl -e 'select(undef,undef,undef,2.5)' 2>/dev/null || python3 -c 'import time;time.sleep(2.5)'

# (2) no-shed-after-hello: a hello-COMPLETING client is never penalized, so it is SERVED end-to-end even
#     from the forced (rate-limited) IP. harp-probe ping completes core.hello.
echo "── §16(b) no-shed-after-hello: a hello-completing client from the forced IP is SERVED (never shed)"
perl -e 'alarm 30; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" -s "$STORE" ping >/tmp/ratelimit-ping.log 2>&1 \
  || { echo "RATELIMIT FAIL: a hello-completing client from the forced IP was SHED (no-shed-after-hello violated — false positive shed)"; tail -20 /tmp/ratelimit-ping.log; tail -20 "$DEVLOG"; exit 1; }
# and a SECOND hello-completing reconnect is admitted too (a completed hello never penalizes the IP).
perl -e 'alarm 30; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" -s "$STORE" ping >/tmp/ratelimit-ping2.log 2>&1 \
  || { echo "RATELIMIT FAIL: a reconnect after a hello-completing session was shed (a completed hello must not penalize the IP)"; tail -20 /tmp/ratelimit-ping2.log; tail -20 "$DEVLOG"; exit 1; }
echo "   ✓ no-shed-after-hello: hello-completing clients from the forced IP are admitted + served"

kill -0 "$DP" 2>/dev/null || { echo "RATELIMIT FAIL: daemon died during the test"; tail -20 "$DEVLOG"; exit 1; }
kill -9 "$DP" 2>/dev/null; DP=""
echo "RATELIMIT PASS (§16(b): shed-on-reflood [pre-hello-failure penalty] + no-shed-after-hello [hello-completing never penalized], via --force-peer-ip)"
