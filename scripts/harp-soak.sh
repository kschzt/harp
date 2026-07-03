#!/usr/bin/env bash
# harp-soak.sh — an all-day HARP transport SOAK. Streams dark/modal music across the
# fleet's diverse transports (direct-cable eth / switch eth / USB), reads every capture
# back through the fidelity analyzer, and — the hard oracle — diffs the device's own
# transport-health counters across each render. Any underrun / overrun / late-frame /
# discontinuity / drift / session-reset the DEVICE counts, or any analyzer FLAG, is a
# real HARP event and gets logged with a timestamp + the exact repro.
#
# Design choices that matter (learned this session):
#   - param indices differ per engine/version → the level param is resolved BY NAME
#     ("Master Level" on refdev, "Level" on the gpu synth) per device, never hardcoded.
#   - content is CONTINUOUS-ish dark/modal arps in the LOW register (high notes get
#     filtered to silence by a dark cutoff) so any silence gap = a real dropout.
#   - the analyzer's absolute-signal floor guards the "silence sails through" trap.
#
# usage:  scripts/harp-soak.sh [ROUNDS]      (default: loop until Ctrl-C)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HOME/Desktop/muswav/soak"; mkdir -p "$OUT"
LOG="$OUT/soak.log"
PROBE="$ROOT/build/harp-probe"
HOST="$ROOT/build-vst/harp-vst3-host"
BUNDLE="$(find "$ROOT/build-vst" -name harp-shell.vst3 -type d | head -1)"
READBACK="$ROOT/scripts/harp-readback.py"
ROUNDS="${1:-100000}"
# A render that comes back below this whole-stream RMS was SILENT — the device produced
# no audio for the full 8s. That is never valid output for ANY device (not engine
# character, not a within-signal transient), so it is a HARD, LOUD fault (see is_silent).
SILENCE_FLOOR="${SILENCE_FLOOR:-0.001}"
SILENT_FAILS=0

# fleet: name | conn (eth:host:port | usb:SERIAL) | transport label
# name | conn | transport | kind (refdev = deterministic, analyzer trusted; synth = counters-only)
TARGETS=(
  "kria|eth:kria.local:47987|cable|refdev"
  "harp|usb:PI4B-0001|usb|refdev"
  "harp2|usb:PI4B-0003|usb|refdev"
  "jetson|eth:jetson.local:7777|switch|synth"
)
# dark/modal note sets (low register, min7/9 / dorian / phrygian), rotated per round
NOTES=(
  "50,53,57,60,57,53,50,55,58,62,58,55,53,57,60,53"   # Dm9 dorian cascade
  "48,51,55,58,55,51,48,53,56,60,56,53,51,55,58,51"   # Cm9
  "45,48,52,55,58,55,52,48,50,53,57,60,57,53,50,45"   # Am-ish phrygian drift
  "43,46,50,53,50,46,43,48,51,55,58,55,51,48,46,43"   # Gm9 brood
)
# error counters that must hold steady during clean streaming
ERRC="audio_underruns|audio_overruns|audio_late_frames|msc_discontinuities|frame_errors|usb_errors|evt_late|evt_stale_epoch|evq_drops|ramp_late|session_resets|fence_timeouts"

ts() { date +%H:%M:%S; }
log() { echo "[$(ts)] $*" | tee -a "$LOG"; }

probe_args() { case "$1" in eth:*) echo "-d ${1#eth:}";; usb:*) echo "-d usb:${1#usb:}";; esac; }
conn_env()   { case "$1" in eth:*) echo "HARP_ETH_DEVICE=${1#eth:}";; usb:*) echo "HARP_DEVICE_SERIAL=${1#usb:}";; esac; }

# bounded probe — a wedged USB gadget must fail FAST (8s), not hang ~30s
pt() { perl -e 'alarm shift; exec @ARGV' 8 "$PROBE" "$@" 2>/dev/null; }

# resolve the output-level param index BY NAME (Master Level / Level / Volume / Gain)
level_idx() {
  local pa="$1"
  pt $pa params 2>/dev/null | awk '
    /\[[0-9]+\] *Master Level/ {gsub(/[][]/,"",$1); m=$1}
    /\[[0-9]+\] *Level/        {gsub(/[][]/,"",$1); if(!l)l=$1}
    /\[[0-9]+\] *(Volume|Gain)/{gsub(/[][]/,"",$1); if(!v)v=$1}
    END{ print (m?m:(l?l:(v?v:0))) }'
}
err_counters() { pt $1 counters 2>/dev/null | grep -E "$ERRC" | sed 's/ //g'; }

# compare two err_counters snapshots; echo any counter that INCREASED
counter_delta() {
  awk -F= 'NR==FNR{b[$1]=$2;next}{if(($2+0)>(b[$1]+0))print $1" "b[$1]"->"$2}' <(echo "$1") <(echo "$2")
}

# is_silent RMS — exit 0 (true) if RMS is below the silence floor. A silent whole-render
# is never valid output, so the soak flags it HARD + LOUD, distinct from the analyzer's
# within-signal FLAGs (spikes/DC), and — unlike a within-signal flag on a synth — it is
# NOT excused as engine character: a synth that emits pure silence has dropped out too.
is_silent() { awk -v r="${1:-1}" -v f="$SILENCE_FLOOR" 'BEGIN{ exit !((r+0) < (f+0)) }'; }

# Prove the silence gate without hardware: it must FIRE on silence and PASS real audio,
# so a refactor that neuters it is caught in CI/pre-push.  HARP_SOAK_SELFTEST=1 scripts/harp-soak.sh
if [ -n "${HARP_SOAK_SELFTEST:-}" ]; then
  rc=0
  is_silent 0.0    || { echo "SELFTEST FAIL: 0.0 not silent";       rc=1; }
  is_silent 0.0005 || { echo "SELFTEST FAIL: 0.0005 not silent";    rc=1; }
  is_silent 0.05   && { echo "SELFTEST FAIL: 0.05 flagged silent";  rc=1; }
  is_silent 0.2    && { echo "SELFTEST FAIL: 0.2 flagged silent";   rc=1; }
  [ $rc = 0 ] && echo "SOAK SELFTEST PASS: silence gate fires <$SILENCE_FLOOR, passes real audio"
  exit $rc
fi

log "=== SOAK START — fleet: ${TARGETS[*]%%|*} — out $OUT ==="
# resolve the level param index per device ONCE, by name (bash-3.2 safe: indexed arrays)
NT=${#TARGETS[@]}; LVLS=(); SKIPPED=()
for i in $(seq 0 $((NT-1))); do
  IFS='|' read -r name conn xport kind <<<"${TARGETS[$i]}"
  idx="$(level_idx "$(probe_args "$conn")")"
  LVLS[$i]="$idx"
  log "$name ($xport): level param = id $idx"
done
round=0
while [ "$round" -lt "$ROUNDS" ]; do
  round=$((round+1))
  notes="${NOTES[$(( (round-1) % ${#NOTES[@]} ))]}"
  for i in $(seq 0 $((NT-1))); do
    IFS='|' read -r name conn xport kind <<<"${TARGETS[$i]}"
    pa="$(probe_args "$conn")"; ce="$(conn_env "$conn")"
    lvl="${LVLS[$i]}"
    if [ "$lvl" = "0" ]; then
      [ -z "${SKIPPED[$i]:-}" ] && { log "$name: SKIP (unreachable — flaky/wedged USB; quiet retry every 20 rounds)"; SKIPPED[$i]=1; }
      [ $((round % 20)) -eq 0 ] && LVLS[$i]="$(level_idx "$pa")"   # periodic recovery probe (bounded)
      continue
    fi
    wav="$OUT/${name}-latest.wav"   # rolling (latest clean = preview); flagged ones preserved below
    cb="$(err_counters "$pa")"
    out=$(env $ce HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" \
          --set 2=0.5 --set 3=0.5 --set 4=0.35 --set 5=0.01 --set 6=0.3 --set "$lvl"=0.8 \
          --channel 1 --part 1 --notes "$notes" --seconds 8 --realtime --out "$wav" 2>&1)
    hostrms=$(echo "$out" | grep -oE 'rms=[0-9.]+' | head -1)
    hostund=$(echo "$out" | grep -oE 'underruns: [0-9]+' | head -1)
    if ! echo "$out" | grep -q "rms="; then log "$name r$round: NO RENDER ($(echo "$out"|grep -iE 'no HARP|error|timed'|head -1))"; continue; fi
    ca="$(err_counters "$pa")"
    delta="$(counter_delta "$cb" "$ca")"
    # HARD silence oracle — BEFORE the kind-based analyzer classification, because a fully
    # silent render is a real dropout for a synth too (it must not be excused as engine
    # character). Preserve the wav + the counter repro, count it, keep soaking (all-day
    # hunter), and fail LOUD at SOAK END.
    rmsnum="${hostrms#rms=}"
    if is_silent "${rmsnum:-1}"; then
      SILENT_FAILS=$((SILENT_FAILS+1))
      cp "$wav" "$OUT/${name}-r${round}-SILENT.wav" 2>/dev/null
      log "!!! SILENT-FAIL: $name r$round $xport rms=${rmsnum:-?} < $SILENCE_FLOOR — device produced SILENCE (hard fault)${delta:+  COUNTER-DELTA[$(echo "$delta"|tr '\n' ';')]}"
      continue
    fi
    verdict="$(python3 "$READBACK" "$wav" 2>&1 | sed -n '1p' | grep -oE '(ok|<<<.*)$')"
    flag=""
    [ -n "$delta" ] && flag="  COUNTER-DELTA[$(echo "$delta"|tr '\n' ';')]"
    if echo "$verdict" | grep -q '<<<'; then
      # within-signal FLAGs are a transport signal only on a deterministic refdev; the
      # gpu synth's own transients/DC trip the detectors (engine character, not HARP)
      if [ "$kind" = "refdev" ]; then flag="$flag  ANALYZER[$verdict]"; else flag="$flag  [engine:$verdict]"; fi
    fi
    [ -n "$flag" ] && cp "$wav" "$OUT/${name}-r${round}-FLAG.wav" 2>/dev/null
    log "$name r$round $xport ${hostrms:-rms=?} ${hostund:-} ${flag:-ok}"
  done
  # every 10th round: a longer SUSTAINED stream on the cable link (drift / buffer stability)
  if [ $((round % 10)) -eq 0 ]; then
    log "--- sustained 60s stream on kria (cable) ---"
    cb="$(err_counters "-d kria.local:47987")"
    env HARP_ETH_DEVICE=kria.local:47987 HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" \
        --set 3=0.45 --set 7=0.7 --lfo 3=0.03 --channel 1 --part 1 \
        --notes 50,53,57,60,55,58,62,53 --seconds 60 --realtime --out "$OUT/sustain-latest.wav" >/dev/null 2>&1
    ca="$(err_counters "-d kria.local:47987")"; d="$(counter_delta "$cb" "$ca")"
    python3 "$READBACK" "$OUT/sustain-latest.wav" 2>&1 | sed -n '1p' | tee -a "$LOG"
    [ -n "$d" ] && log "  sustained COUNTER-DELTA: $(echo "$d"|tr '\n' ';')"
  fi
  # every 5th round: reconnect CHURN on the cable link (hunts the [control]/recall flake)
  if [ $((round % 5)) -eq 0 ]; then
    fails=0
    for c in $(seq 1 8); do
      env HARP_ETH_DEVICE=kria.local:47987 HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" \
          --set 7=0.7 --channel 1 --part 1 --notes 50,57 --seconds 1 --realtime --out /tmp/churn.wav >/dev/null 2>&1 \
          || fails=$((fails+1))
    done
    log "--- reconnect churn x8 on kria: $fails fail(s) ---"
  fi
done
if [ "$SILENT_FAILS" -gt 0 ]; then
  log "=== SOAK END (round $round) — !!! HARD FAILURE: $SILENT_FAILS SILENT render(s) (see $OUT/*-SILENT.wav) ==="
  exit 1
fi
log "=== SOAK END (round $round) — 0 silent renders ==="
