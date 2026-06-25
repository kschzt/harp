#!/bin/bash
# epoch-test — §7.1/§8.6 time.epoch on a sample-rate change, over the §8.7 loopback.
#
# Wires the harp-probe `epoch-test` driver (host/harp-probe.c::cmd_epoch_test) into CI. Per
# §8.6 a sample-rate change opens a NEW clock epoch: the device MUST bump its epoch counter
# and emit ntf time.epoch {new-epoch, new-rate, old-epoch, old-msc-final}; any event still
# timestamped in the OLD (stale) epoch is then discarded + counted (§7.1). This is the
# normative MUST behind cert T4 — until now the driver existed but ran in NO suite (HIGH #3
# audit gap: orphaned), so the §8.6 epoch increment had no CI coverage.
#
# The probe drives audio.start at one rate, then re-starts at 44100 and captures the
# time.epoch notification, asserting new == old+1 and rate == 44100; the stale-epoch discard
# half is checked inside the same driver. All assertions live in cmd_epoch_test — this script
# just stands up a TCP-serving harp-deviced for it.
#
# Co-existence: unique port; kills ONLY its own device by pid; workspace-RELATIVE state dir
# (Git Bash /tmp->C:\ trips the MinGW device's mkdir on Windows; see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-47922}"
DEVDIR=epoch-state   # workspace-RELATIVE
DEVLOG=/tmp/epoch-dev.log
fail() { echo "EPOCH FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null' EXIT INT TERM
rm -rf "$DEVDIR"; : > "$DEVLOG"
"$DEVICED" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" "$DEVLOG" || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

# §8.6 time.epoch increment + new-rate + §7.1 stale-epoch discard — all asserted in the driver.
perl -e 'alarm 30; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" epoch-test \
  || { cat "$DEVLOG"; fail "epoch-test assertions failed (§7.1/§8.6 time.epoch / stale-epoch)"; }
echo "EPOCH PASS (§7.1/§8.6: time.epoch bumped + new rate on a sample-rate change; stale-epoch discarded — cert T4)"
