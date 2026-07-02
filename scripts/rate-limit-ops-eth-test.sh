#!/bin/bash
# rate-limit-ops-eth-test — T9 §16 DoS: the daemon MUST rate-limit the EXPENSIVE control ops
# (state.snapshot, diag.bundle) so one peer's flood cannot pin the single recv thread and starve
# every other request. This exercises the per-connection token bucket (device/op_ratelimit.h,
# wired into device/session.c handle_state_snapshot / handle_diag_bundle) over the §8.7 loopback.
#
# What it proves (all over ONE connection = one peer, since the shed is per-connection):
#   FLOOD    — hammering state.snapshot back-to-back, a SMALL burst is admitted (the bucket's
#              burst credit) and the REST are shed with the §5.3 `busy` error; the session is NOT
#              dropped (every request still gets a framed reply). Same for diag.bundle (its own
#              independent bucket). Throttled := shed > admitted.
#   COUNTER  — the §14.2 vendor counter x.harp-refdev.rate_limited climbs by EXACTLY the number of
#              shed ops (observable via diag.counters — the counter is per-device, monotonic).
#   NORMAL   — a normal cadence (a snapshot every 150 ms, above the 100 ms sustained rate) is
#              NEVER throttled: every op succeeds and the counter does NOT move. This is the
#              golden-safe guarantee — the guard is invisible to real host/plugin use.
#
# Co-existence: unique port; kills ONLY its own device by pid; workspace-RELATIVE state dir
# (Git Bash /tmp->C:\ trips the MinGW device mkdir; see eth-tests.sh); python3-gated (like
# conn-flood / abuse). Reads the counter via harp-probe (a separate, un-throttled method).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17961}"
SERIAL="${SERIAL:-SIM-RLOPS}"
DEVDIR=ratelimit-ops-state   # workspace-RELATIVE (see header)
DEVLOG=/tmp/ratelimit-ops-dev.log
fail() { echo "RATELIMIT-OPS FAIL: $1"; exit 1; }

command -v python3 >/dev/null 2>&1 || { echo "RATELIMIT-OPS SKIP: python3 not available"; exit 0; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf "$DEVDIR"' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" --panel-sock "" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# read the §14.2 x.harp-refdev.rate_limited counter over a FRESH connection (diag.counters is not
# a rate-limited op, so this read never perturbs the buckets under test).
read_rl() {
    perl -e 'alarm 15; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" counters 2>/dev/null \
        | grep -E '^[[:space:]]*x\.harp-refdev\.rate_limited = ' | tail -1 | sed -E 's/^.*= //'
}

# ── FLOOD phase: one connection hammers snapshot then bundle; python prints the tallies ──
# The python speaks the framed link directly (like conn-flood / hello-gate): core.hello, then N
# back-to-back requests, classifying each framed reply by envelope msgtype (byte 2: 1=response
# success, 2=error) and, for errors, whether the code is `busy` (the §5.3 rate-limit shed).
echo "── FLOOD: hammer state.snapshot + diag.bundle on ONE connection (§16 expensive-op shed)"
FLOODOUT="$(python3 - "127.0.0.1" "$PORT" <<'EOF'
import socket, struct, sys, time
host, port = sys.argv[1], int(sys.argv[2])
N = 60  # flood depth per op

def head(major, n):
    m = major << 5
    if n < 24:    return bytes([m | n])
    if n < 256:   return bytes([m | 0x18, n])
    if n < 65536: return bytes([m | 0x19]) + struct.pack(">H", n)
    return bytes([m | 0x1a]) + struct.pack(">I", n)
def u(n):    return head(0, n)
def text(s): b = s.encode(); return head(3, len(b)) + b
def frame(stream, payload): return bytes([1, stream, 1, 0]) + struct.pack("<I", len(payload)) + payload
def req(rid, method, body=None):
    env = head(5, 4 if body is not None else 3)
    env += u(0) + u(0) + u(1) + u(rid) + u(2) + text(method)
    if body is not None: env += u(3) + body
    return frame(0, env)

HELLO_BODY = head(5, 1) + u(0) + head(4, 2) + u(1) + u(0)   # {0:[1,0]}  (major 1, minor 0)
SNAP_BODY  = head(5, 1) + u(0) + text("live/project")        # {0:"live/project"}

class Conn:
    """Framed link with a PERSISTENT receive buffer so nothing bleeds between floods."""
    def __init__(self, s): self.s = s; self.buf = bytearray()
    def send(self, b): self.s.sendall(b)
    def _pop_frames(self):        # consume every COMPLETE frame currently buffered
        out, o = [], 0
        while o + 8 <= len(self.buf):
            ln = struct.unpack("<I", self.buf[o+4:o+8])[0]
            if o + 8 + ln > len(self.buf): break
            out.append(bytes(self.buf[o+8:o+8+ln])); o += 8 + ln
        del self.buf[:o]
        return out
    def collect(self, n_resp, timeout=10.0):
        """Return exactly n_resp response/error frames (msgtype 1/2); notifications skipped."""
        self.s.settimeout(0.5); got = []; deadline = time.time() + timeout
        while len(got) < n_resp and time.time() < deadline:
            for f in self._pop_frames():
                if len(f) >= 3 and f[2] in (1, 2): got.append(f)
            if len(got) >= n_resp: break
            try: chunk = self.s.recv(65536)
            except socket.timeout: continue
            if not chunk: break
            self.buf += chunk
        return got

def classify(frames):
    success = shed = other = 0
    for p in frames:
        mt = p[2]                      # envelope key0 (uint) msgtype byte
        if mt == 1: success += 1       # HARP_MSG_RESPONSE
        elif mt == 2:                  # HARP_MSG_ERROR
            if b"busy" in p: shed += 1
            else: other += 1
    return success, shed, other

s = socket.create_connection((host, port), timeout=3)
c = Conn(s)
c.send(req(0, "core.hello", HELLO_BODY))
hf = c.collect(1)
if not hf or hf[0][2] != 1:
    sys.exit("core.hello did not return a response: %r" % (hf[:1],))

def flood(method, body):
    c.send(b"".join(req(rid, method, body) for rid in range(1, N + 1)))
    return classify(c.collect(N))

s_ok, s_shed, s_other = flood("state.snapshot", SNAP_BODY)
b_ok, b_shed, b_other = flood("diag.bundle", None)
print("SNAP ok=%d shed=%d other=%d" % (s_ok, s_shed, s_other))
print("BUNDLE ok=%d shed=%d other=%d" % (b_ok, b_shed, b_other))
print("N=%d" % N)
s.close()
EOF
)" || { echo "$FLOODOUT"; fail "flood client errored"; }
echo "$FLOODOUT" | sed 's/^/   /'

N=$(echo "$FLOODOUT"      | sed -n 's/^N=//p')
S_OK=$(echo "$FLOODOUT"   | sed -n 's/^SNAP ok=\([0-9]*\).*/\1/p')
S_SHED=$(echo "$FLOODOUT" | sed -n 's/^SNAP .*shed=\([0-9]*\).*/\1/p')
S_OTHER=$(echo "$FLOODOUT"| sed -n 's/^SNAP .*other=\([0-9]*\).*/\1/p')
B_OK=$(echo "$FLOODOUT"   | sed -n 's/^BUNDLE ok=\([0-9]*\).*/\1/p')
B_SHED=$(echo "$FLOODOUT" | sed -n 's/^BUNDLE .*shed=\([0-9]*\).*/\1/p')
B_OTHER=$(echo "$FLOODOUT"| sed -n 's/^BUNDLE .*other=\([0-9]*\).*/\1/p')
[ -n "$N" ] && [ -n "$S_OK" ] && [ -n "$S_SHED" ] && [ -n "$B_OK" ] && [ -n "$B_SHED" ] \
    || fail "could not parse flood tallies from: $FLOODOUT"

# every flooded request got a framed reply (no dropped session), all accounted for.
[ "$S_OTHER" = 0 ] || fail "state.snapshot flood produced $S_OTHER non-busy errors (expected 0 — valid body)"
[ $((S_OK + S_SHED)) -eq "$N" ] || fail "state.snapshot: ok($S_OK)+shed($S_SHED) != N($N) — session dropped mid-flood?"
[ $((B_OK + B_SHED + B_OTHER)) -eq "$N" ] || fail "diag.bundle: replies($((B_OK+B_SHED+B_OTHER))) != N($N) — session dropped mid-flood?"

# THROTTLED: a small burst admitted, the rest shed; shed must dominate (this is the DoS guard).
[ "$S_OK" -ge 1 ] || fail "state.snapshot admitted 0 — the burst allowance is gone (guard too strict)"
[ "$S_SHED" -gt "$S_OK" ] || fail "state.snapshot NOT throttled: shed($S_SHED) <= admitted($S_OK)"
[ "$S_SHED" -ge $((N / 2)) ] || fail "state.snapshot shed only $S_SHED/$N — the flood was not throttled hard enough"
[ "$B_OK" -ge 1 ] || fail "diag.bundle admitted 0 — the burst allowance is gone (guard too strict)"
[ "$B_SHED" -gt "$B_OK" ] || fail "diag.bundle NOT throttled: shed($B_SHED) <= admitted($B_OK)"
echo "   throttled: state.snapshot admitted $S_OK, shed $S_SHED; diag.bundle admitted $B_OK, shed $B_SHED (of $N each) ✓"

# ── COUNTER phase: x.harp-refdev.rate_limited climbed by EXACTLY the sheds (§14.2 observability) ──
RL_AFTER_FLOOD="$(read_rl)"
case "$RL_AFTER_FLOOD" in ''|*[!0-9]*) fail "rate_limited counter absent/non-numeric: '$RL_AFTER_FLOOD'";; esac
EXPECT=$((S_SHED + B_SHED))
[ "$RL_AFTER_FLOOD" = "$EXPECT" ] \
    || fail "x.harp-refdev.rate_limited = $RL_AFTER_FLOOD, expected $EXPECT (snap $S_SHED + bundle $B_SHED sheds)"
echo "   x.harp-refdev.rate_limited = $RL_AFTER_FLOOD == $S_SHED+$B_SHED sheds ✓"

# ── NORMAL phase: a 150ms cadence is NEVER throttled; the counter does not move ──
echo "── NORMAL: 8 × state.snapshot at a 150ms cadence (above the 100ms sustained rate)"
NORMOUT="$(python3 - "127.0.0.1" "$PORT" <<'EOF'
import socket, struct, sys, time
host, port = sys.argv[1], int(sys.argv[2])
def head(major, n):
    m = major << 5
    if n < 24: return bytes([m | n])
    if n < 256: return bytes([m | 0x18, n])
    return bytes([m | 0x19]) + struct.pack(">H", n)
def u(n): return head(0, n)
def text(s): b = s.encode(); return head(3, len(b)) + b
def frame(st, p): return bytes([1, st, 1, 0]) + struct.pack("<I", len(p)) + p
def req(rid, method, body=None):
    env = head(5, 4 if body is not None else 3)
    env += u(0)+u(0)+u(1)+u(rid)+u(2)+text(method)
    if body is not None: env += u(3)+body
    return frame(0, env)
HELLO = head(5,1)+u(0)+head(4,2)+u(1)+u(0)
SNAP  = head(5,1)+u(0)+text("live/project")
def parse_frames(buf):
    out, o = [], 0
    while o + 8 <= len(buf):
        ln = struct.unpack("<I", buf[o+4:o+8])[0]
        if o + 8 + ln > len(buf): break
        out.append(buf[o+8:o+8+ln]); o += 8 + ln
    return out
def drain(s, secs):  # read everything available for `secs`, discard (hello rsp + core.credit ntf)
    s.settimeout(secs); end = time.time() + secs
    while time.time() < end:
        try: c = s.recv(65536)
        except socket.timeout: break
        if not c: break
s = socket.create_connection((host, port), timeout=3); s.settimeout(3)
s.sendall(req(0, "core.hello", HELLO)); drain(s, 0.5)  # settle hello response + the credit grant
# 8 snapshots, one every 150ms (> the 100ms sustained rate) — the bucket refills faster than we
# spend, so NONE should shed. Responses buffer in TCP; classify them all at the end by msgtype.
for rid in range(1, 9):
    s.sendall(req(rid, "state.snapshot", SNAP))
    time.sleep(0.15)
s.settimeout(0.5); buf = b""; end = time.time() + 3.0
while time.time() < end and len(parse_frames(buf)) < 8:
    try: c = s.recv(65536)
    except socket.timeout: continue
    if not c: break
    buf += c
ok = shed = 0
for p in parse_frames(buf):
    if len(p) < 3: continue
    if p[2] == 1: ok += 1
    elif p[2] == 2 and b"busy" in p: shed += 1
print("NORMAL ok=%d shed=%d" % (ok, shed))
s.close()
EOF
)" || { echo "$NORMOUT"; fail "normal-cadence client errored"; }
echo "$NORMOUT" | sed 's/^/   /'
NM_OK=$(echo "$NORMOUT"   | sed -n 's/^NORMAL ok=\([0-9]*\).*/\1/p')
NM_SHED=$(echo "$NORMOUT" | sed -n 's/^NORMAL .*shed=\([0-9]*\).*/\1/p')
[ "$NM_OK" = 8 ]   || fail "normal cadence: only $NM_OK/8 snapshots succeeded (a real host cadence must NOT be throttled)"
[ "$NM_SHED" = 0 ] || fail "normal cadence: $NM_SHED snapshots were shed (the guard must be invisible to normal use)"

RL_AFTER_NORMAL="$(read_rl)"
[ "$RL_AFTER_NORMAL" = "$RL_AFTER_FLOOD" ] \
    || fail "rate_limited moved during normal use: $RL_AFTER_FLOOD -> $RL_AFTER_NORMAL (normal cadence must add 0 sheds)"
echo "   normal cadence: 8/8 admitted, 0 shed, counter unchanged ($RL_AFTER_NORMAL) ✓"

# ── the daemon still serves a well-behaved client after the flood (no wedge) ──
perl -e 'alarm 15; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" identify >/dev/null 2>&1 \
    || fail "daemon did not serve a normal client after the flood (wedged?)"
echo "   daemon still serves a normal client post-flood ✓"

kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""
echo "RATELIMIT-OPS PASS (§16: state.snapshot + diag.bundle throttled under flood [shed>admitted], counter climbs by exactly the sheds, normal 150ms cadence never throttled, daemon never wedged)"
