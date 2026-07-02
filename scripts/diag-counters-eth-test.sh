#!/bin/bash
# diag-counters-eth-test — ALL-16 §14.2 diagnostic counters, over the §8.7 loopback.
#
# COVERAGE GAP this closes: the §14.2 counter map is a frozen conformance golden
# (device/session.c emit_counters — a 16-pair, text-keyed map shared byte-identically
# by diag.counters AND the diag.bundle device-section). But nothing GATED the whole
# map: diag-bundle-eth greps just ONE key ('usb_errors'), and the rest of the suite
# only asserts the HEADLINE three via targeted tests (evt_late via evt-storm,
# audio_underruns/overruns via the rt paths, fence_timeouts via realtime-fence). A
# rename / drop / added / retyped counter in the other thirteen would slip through.
#
# This reads diag.counters over the loopback (`harp-probe counters`) and gates EVERY
# counter: (a) the exact 16-key NAME SET is present AND the map size is exactly 16
# (no missing, no extras, no renames); (b) each value's TYPE (uint vs the one signed
# int, clock_drift_ppb); (c) VALUE BOUNDS on a clean idle session — the 12 never-silent
# safety counters are 0, the storage gauges are a sane positive total with 0<free<=total.
# A regression in emit_counters() fails here loud.
#
# Co-existence: unique port + kills ONLY its own device by pid; hard perl-alarm watchdog;
# workspace-RELATIVE state dir (Git Bash /tmp->C:\ trips the device mkdir; see eth-tests.sh).
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
PROBE="${PROBE:-./build/harp-probe}"
PORT="${PORT:-17993}"
SERIAL="${SERIAL:-SIM-0001}"
DEVDIR=diagcounters-eth-state   # workspace-RELATIVE (see header)
DEVLOG=/tmp/diagcounters-eth-dev.log
OUT=/tmp/diagcounters-eth.txt
ERR=/tmp/diagcounters-eth-probe.err
fail() { echo "DIAG-COUNTERS FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$PROBE" ]   || fail "$PROBE not built"

DP=""
trap '[ -n "$DP" ] && kill -9 "$DP" 2>/dev/null; rm -rf "$DEVDIR"' EXIT
wait_listen() { for _ in $(seq 1 25); do grep -q "listening on $PORT" "$DEVLOG" 2>/dev/null && return 0; sleep 0.2; done; return 1; }

rm -rf "$DEVDIR"; : > "$DEVLOG"; rm -f "$OUT" "$ERR"
echo "── start device (serial $SERIAL) on $PORT over the §8.7 loopback"
"$DEVICED" --serial "$SERIAL" --port "$PORT" --state-dir "$DEVDIR" >>"$DEVLOG" 2>&1 & DP=$!
wait_listen || { cat "$DEVLOG"; fail "device didn't start on $PORT"; }

echo "── harp-probe counters -> $OUT (§14.2 diag.counters over the loopback)"
perl -e 'alarm 15; exec @ARGV' "$PROBE" -d "127.0.0.1:$PORT" counters >"$OUT" 2>"$ERR" \
    || { cat "$OUT" "$ERR"; fail "counters command failed (rc=$?)"; }
[ -s "$OUT" ] || { cat "$ERR"; fail "no counter output"; }

# value of a named counter (the '  name = value' probe format); empty string if absent.
val() { grep -E "^[[:space:]]*$1 = " "$OUT" | tail -1 | sed -E 's/^.*= //'; }
is_uint() { case "$1" in ''|*[!0-9]*) return 1;; *) return 0;; esac; }
is_int()  { case "$1" in ''|-) return 1;; -*) is_uint "${1#-}";; *) is_uint "$1";; esac; }

# The exact §14.2 16-counter set (device/session.c emit_counters). ALL must be present.
ALL16="x.harp-refdev.fence_waits x.harp-refdev.fence_timeouts usb_errors frame_errors \
audio_underruns audio_overruns audio_late_frames msc_discontinuities clock_drift_ppb \
evt_late evt_stale_epoch x.harp-refdev.evq_drops x.harp-refdev.ramp_late session_resets \
storage_bytes_total storage_bytes_free"

for c in $ALL16; do
    [ -n "$(val "$c")" ] || { cat "$OUT"; fail "counter '$c' ABSENT from diag.counters (§14.2 map incomplete)"; }
done
# exactly 16 — no missing, no extras/renames (the map is a frozen conformance golden).
lines="$(grep -cE '^[[:space:]]*[A-Za-z0-9._-]+ = ' "$OUT")"
[ "$lines" = 16 ] || { cat "$OUT"; fail "diag.counters emitted $lines counter lines, expected exactly 16 (§14.2 map drift)"; }
echo "   all 16 §14.2 counter keys present; map size exactly 16 ✓"

# ── value gates on a CLEAN idle session ─────────────────────────────────────
# 12 safety counters MUST be 0 (never-silent invariants: any nonzero is a live fault).
for z in x.harp-refdev.fence_timeouts usb_errors frame_errors audio_underruns \
         audio_overruns audio_late_frames msc_discontinuities evt_late evt_stale_epoch \
         x.harp-refdev.evq_drops x.harp-refdev.ramp_late session_resets; do
    v="$(val "$z")"; is_uint "$v" || fail "counter '$z' not a uint: '$v'"
    [ "$v" = 0 ] || fail "safety counter '$z' = $v on a clean idle session (expected 0 — §14.2 never-silent)"
done
echo "   12 safety counters all 0 on a clean session ✓"

# fence_waits: unsigned, >= 0 (idle may legitimately be 0).
fw="$(val x.harp-refdev.fence_waits)"; is_uint "$fw" || fail "fence_waits not a uint: '$fw'"
# clock_drift_ppb: SIGNED int (device-side 0; a sensing device could report negative — §14.2).
cdp="$(val clock_drift_ppb)"; is_int "$cdp" || fail "clock_drift_ppb not a signed int: '$cdp'"
# storage: real filesystem gauges from statvfs() — total>0, 0<free<=total on POSIX. On Windows the
# MinGW device has no statvfs (device/session.c deliberately "skips them"), so both gauges are 0:
# type-check them but don't require positive there (0 is correct-by-design on Windows, not a regression).
st="$(val storage_bytes_total)"; sf="$(val storage_bytes_free)"
is_uint "$st" || fail "storage_bytes_total not a uint: '$st'"
is_uint "$sf" || fail "storage_bytes_free not a uint: '$sf'"
case "${DEVICED:-}" in
  *.exe)  echo "   fence_waits uint, clock_drift_ppb signed-int, storage gauges typed (0 on Windows — no statvfs, by design) ✓" ;;
  *)      [ "$st" -gt 0 ] || fail "storage_bytes_total not positive: '$st'"
          [ "$sf" -gt 0 ] || fail "storage_bytes_free not positive: '$sf'"
          [ "$sf" -le "$st" ] || fail "storage_bytes_free ($sf) > storage_bytes_total ($st) — impossible"
          echo "   fence_waits uint, clock_drift_ppb signed-int, storage total>0 & 0<free<=total ✓" ;;
esac

kill -9 "$DP" 2>/dev/null; wait "$DP" 2>/dev/null; DP=""
echo "DIAG-COUNTERS PASS (all 16 §14.2 counters present + exact map size 16; 12 safety counters 0; every value typed + bounded)"
