#!/bin/bash
# evt-storm-eth-test — §9.2 evt_late stays 0 under an event storm on the low-nsamples free-running
# RTP path, over the §8.7 loopback.
#
# Guards the #76 latency change directly: with rt-nsamples=64 the device emits small RTP packets and
# the host renders at a small DAW block, so the host→device event delivery window shrinks. §9.2 says a
# note/set timestamped at a future sample MUST be applied AT that sample, never past it — the device's
# g_evt_late counter (device-section "evt_late") MUST stay 0. If the latency math regresses (the host
# schedules events for a frame whose covering RTP packet already went out), evt_late ticks up and this
# fails. We hammer a dense note storm at --block 64 over the free-running plane and assert the device
# evt_late counter is flat (read off the host's --diag-bundle device-section, key 4 -> counters -> evt_late).
#
# Co-existence: unique port; kills ONLY its own device by pid; perl-alarm watchdog; workspace-RELATIVE
# state dir (Git Bash /tmp->C:\ trips the MinGW device mkdir on Windows; see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-47974}"
SERIAL="${SERIAL:-SIM-0001}"
DEVDIR=evtstorm-eth-state   # workspace-RELATIVE (see header)
DEVLOG=/tmp/evtstorm-eth-dev.log
HOSTLOG=/tmp/evtstorm-eth-host.log
BUNDLE=/tmp/evtstorm-eth.cbor
fail() { echo "EVT-STORM FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

DP=""; HP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; [ -n "$HP" ] && kill -9 "$HP" 2>/dev/null' EXIT
rm -rf "$DEVDIR"; : > "$DEVLOG"; : > "$HOSTLOG"; rm -f "$BUNDLE"

# rt-nsamples=64 -> the §9.2 low-nsamples free-running RTP path (small packets, tight delivery window).
echo "── device (rt-nsamples=64) over the §8.7 loopback; host renders a dense note storm at --block 64"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --rt-nsamples 64 --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# A dense event storm: many notes at a tight period over a small block, --realtime so the host paces
# against the wall clock (the mode #76's latency math actually runs in). Each note is a future-stamped
# evt the device must apply ON its sample; a regressed delivery window pushes them past -> evt_late++.
NOTES="48,50,52,53,55,57,59,60,62,64,65,67,69,71,72,71,69,67,65,64,62,60,59,57,55,53,52,50,48,50,52,53"
HARP_ETH_DEVICE="127.0.0.1:$PORT" HARP_DEVICE_SERIAL="$SERIAL" \
  perl -e 'alarm 30; exec @ARGV' "$HOSTBIN" "$PLUG" --seconds 5 --realtime --block 64 \
    --notes "$NOTES" --note-period 0.05 --diag-bundle "$BUNDLE" >"$HOSTLOG" 2>&1 & HP=$!
wait "$HP"; rc=$?; HP=""
kill -9 "$DP" 2>/dev/null; DP=""

grep -iE "AddressSanitizer|SEGV|abort trap|terminating due to" "$HOSTLOG" && fail "host CRASHED under the event storm"
[ "$rc" -eq 142 ] && { cat "$HOSTLOG"; fail "host HUNG under the event storm (perl-alarm watchdog fired)"; }
grep -q "connected:" "$HOSTLOG" || { cat "$HOSTLOG"; fail "host never connected (no oracle)"; }
[ -s "$BUNDLE" ] || { cat "$HOSTLOG"; fail "no diag bundle written (capture didn't run?)"; }

# §9.2: the device evt_late counter MUST be 0 — read it off the host bundle's device-section
# (top key 4 -> device-section counters map key 1 -> string key "evt_late"). Decoded with python3+cbor2
# (installed in CI via eth.yml on ALL three OSes, Windows included — HIGH #6). On CI the decoder MUST
# be present on EVERY OS, so its absence is a hard fail — a regression that silently started applying
# events late can't slip through green. A bare dev box without cbor2 still skip-logs.
if python3 -c "import cbor2" >/dev/null 2>&1; then
  python3 - "$BUNDLE" <<'PY' || fail "§9.2 evt_late not flat on the low-nsamples free-running path (#76 latency regression?)"
import sys, cbor2
b = cbor2.load(open(sys.argv[1], "rb"))
dev = b.get(4) or {}
ctr = dev.get(1) or {}
late = ctr.get("evt_late")
if late is None: sys.exit("device-section counter 'evt_late' absent (key 4 -> 1 -> evt_late) — not surfaced")
if late != 0: sys.exit("device evt_late = %r after the storm — events applied PAST their sample (§9.2 / #76 regression)" % (late,))
print("   ✓ device-section evt_late = 0 (events applied on-sample under the storm; rt-nsamples=64 free-running)")
PY
elif [ -n "${CI:-}" ]; then
  fail "cbor2 unavailable on CI — the §9.2 evt_late assertion cannot be skipped on any OS (install cbor2; see eth.yml)"
else
  echo "   (cbor2 absent — skipped the device evt_late assertion on this dev box)"
fi
echo "EVT-STORM PASS (§9.2: evt_late stayed 0 under a dense note storm on the rt-nsamples=64 free-running path; clean exit rc=$rc)"
