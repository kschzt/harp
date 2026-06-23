#!/bin/bash
# part-filter-eth-test — §9.4: a plugin instance reflects device front-panel echoes for
# ITS OWN part only. Over the §8.7 loopback (no hardware), a part-0 instance renders while
# a part-0 knob (id 3) AND a part-1 knob (id 6) are injected through the panel socket. The
# instance MUST surface the part-0 echo and MUST NOT surface the part-1 one.
#
# The device echoes EVERY front-panel edit unconditionally (front_panel_set_part always
# evt_echo_param on the edited part), so the ABSENCE of param 6 is the part FILTER, not a
# missing echo. Pre-fix (popEcho ignored the echo's part) the instance surfaced BOTH ids —
# the bug where one plugin instance mirrored every part's edits.
#
# Hang-proof: a perl alarm hard-kills the host (a no-connect would otherwise supervise for
# hot-plug forever), and we assert the instance actually connected. Kills only its OWN
# device (by pid) on a unique port — never a broad pkill (which would kill a co-running
# agent's harp-deviced on the same box).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47963}"
SOCK="/tmp/harp-pf-panel.sock"
LOG=/tmp/pf-host.log
fail() { echo "PART-FILTER FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

rm -rf /tmp/pf-state; : > /tmp/pf-dev.log; : > "$LOG"; rm -f "$SOCK"
"$DEVICED" --port "$PORT" --panel-sock "$SOCK" --state-dir /tmp/pf-state >/tmp/pf-dev.log 2>&1 &
DP=$!; trap 'kill -9 "$DP" 2>/dev/null' EXIT
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/pf-dev.log 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" /tmp/pf-dev.log || { cat /tmp/pf-dev.log; fail "device didn't start on $PORT"; }
for _ in $(seq 1 25); do [ -S "$SOCK" ] && break; sleep 0.2; done

panel() { python3 -c "import socket,sys; s=socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect('$SOCK'); s.send(sys.argv[1].encode()+b'\n'); s.recv(256); s.close()" "$1"; }

# Inject a part-0 (id 3) and a part-1 (id 6) front-panel knob once the instance connects.
( for _ in $(seq 1 60); do grep -q "connected:" "$LOG" 2>/dev/null && break; sleep 0.1; done
  sleep 0.3
  panel "knob 0 3 0.90"   # part 0, id 3 -> the instance's OWN part: must be reflected
  panel "knob 1 6 0.90"   # part 1, id 6 -> ANOTHER part: must be filtered out
  sleep 0.3
  panel "knob 0 3 0.20" ) &
INJ=$!
# part-0 instance renders 2s; perl `alarm` is a HARD watchdog so a no-connect can't hang.
HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="SIM-0001" HARP_CHANNEL=0 \
  perl -e 'alarm 15; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 2 --realtime >"$LOG" 2>&1 || true
kill -9 "$INJ" 2>/dev/null; wait "$INJ" 2>/dev/null

echo "── knob echoes the part-0 instance surfaced (meter ids 40xx/41xx omitted from view):"
grep "echo: param [0-9]* " "$LOG" | grep -vE "param 40|param 41" | sort -u | sed 's/^/   /' || true
grep -q "connected:" "$LOG" || fail "instance never connected to the loopback device (no oracle)"
grep -q "echo: param 3 " "$LOG" \
    || fail "part-0 instance did NOT reflect its OWN part-0 edit (param 3) — echo broken?"
grep -q "echo: param 6 " "$LOG" \
    && fail "part-0 instance WRONGLY reflected the part-1 edit (param 6) — the part-filter bug"
echo "PART-FILTER PASS (part-0 instance reflected its own param 3, ignored the part-1 param 6)"
