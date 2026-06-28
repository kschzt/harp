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

# fleet: name | conn (eth:host:port | usb:SERIAL) | transport | kind | recover-ssh-host
# kind: refdev = deterministic, analyzer trusted; synth = counters-only.
# recover-ssh-host: for USB targets only — where to `systemctl restart harp-deviced-usb`
# to clear a wedge (an ungraceful host kill mid-stream parks the gadget in ffs_ep0_read
# "waiting for enable"; new claims hang with no self-recovery — only a device daemon
# restart re-binds the UDC -> re-enumerate -> fresh ENABLE. See docs/usb-wedge-rootcause.md).
TARGETS=(
  "kria|eth:kria.local:47987|cable|refdev|"
  "harp|usb:PI4B-0001|usb|refdev|jak@harp.local"
  "harp2|usb:PI4B-0003|usb|refdev|jak@harp2.local"
  "jetson|eth:jetson.local:7777|switch|synth|"
)
# dark/modal note sets (low register, min7/9 / dorian / phrygian), rotated per round
NOTES=(
  "50,53,57,60,57,53,50,55,58,62,58,55,53,57,60,53"   # Dm9 dorian cascade
  "48,51,55,58,55,51,48,53,56,60,56,53,51,55,58,51"   # Cm9
  "45,48,52,55,58,55,52,48,50,53,57,60,57,53,50,45"   # Am-ish phrygian drift
  "43,46,50,53,50,46,43,48,51,55,58,55,51,48,46,43"   # Gm9 brood
)
# TRANSPORT-fault counters that must hold steady during clean streaming. NOTE the
# host-pacing counters (ramp_late / evt_late / evt_stale_epoch / evq_drops) are
# DELIBERATELY excluded: they fire when the HOST sends events/ramps late under Mac
# CPU load (proven — idle Mac deltas 0), so they're host-scheduling noise, not a
# HARP transport defect. These are the device's own audio-pipeline faults.
ERRC="audio_underruns|audio_overruns|audio_late_frames|msc_discontinuities|frame_errors|usb_errors|session_resets|fence_timeouts"

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

# Clear a wedged USB gadget by restarting its device daemon (re-binds the UDC -> the host
# re-enumerates -> a fresh FUNCTIONFS ENABLE). This is the ONLY recovery short of a reboot
# for the ffs_ep0_read "waiting for enable" wedge an ungraceful host kill leaves behind.
# Best-effort + bounded; returns 0 if the restart command succeeded.
usb_recover() {  # $1 = ssh host (jak@harpX.local)
  [ -z "$1" ] && return 1
  ssh -o ConnectTimeout=8 -o BatchMode=yes "$1" 'sudo systemctl restart harp-deviced-usb' >/dev/null 2>&1
}

# compare two err_counters snapshots; echo any counter that INCREASED
counter_delta() {
  awk -F= 'NR==FNR{b[$1]=$2;next}{if(($2+0)>(b[$1]+0))print $1" "b[$1]"->"$2}' <(echo "$1") <(echo "$2")
}

log "=== SOAK START — fleet: ${TARGETS[*]%%|*} — out $OUT ==="
# resolve the level param index per device ONCE, by name (bash-3.2 safe: indexed arrays)
NT=${#TARGETS[@]}; LVLS=(); SKIPPED=()
for i in $(seq 0 $((NT-1))); do
  IFS='|' read -r name conn xport kind recov <<<"${TARGETS[$i]}"
  idx=0  # retry: a transient probe timeout (device busy with a prior teardown at startup)
  for try in 1 2 3; do idx="$(level_idx "$(probe_args "$conn")")"; [ "$idx" != "0" ] && break; sleep 3; done
  if [ "$idx" = "0" ] && [ -n "$recov" ]; then   # USB wedge -> daemon restart, then re-probe
    log "$name ($xport): unresolved at startup -> USB-WEDGE auto-recovery (restart harp-deviced-usb on $recov)"
    usb_recover "$recov"; sleep 6
    for try in 1 2 3; do idx="$(level_idx "$(probe_args "$conn")")"; [ "$idx" != "0" ] && break; sleep 3; done
    [ "$idx" != "0" ] && log "$name: RECOVERED after daemon restart (level id $idx)" || log "$name: STILL wedged after daemon restart (will keep auto-recovering)"
  fi
  LVLS[$i]="$idx"
  log "$name ($xport): level param = id $idx"
done
round=0
while [ "$round" -lt "$ROUNDS" ]; do
  round=$((round+1))
  notes="${NOTES[$(( (round-1) % ${#NOTES[@]} ))]}"
  for i in $(seq 0 $((NT-1))); do
    IFS='|' read -r name conn xport kind recov <<<"${TARGETS[$i]}"
    pa="$(probe_args "$conn")"; ce="$(conn_env "$conn")"
    lvl="${LVLS[$i]}"
    if [ "$lvl" = "0" ]; then
      [ -z "${SKIPPED[$i]:-}" ] && { log "$name: wedged/unreachable USB — auto-recovering every 3 rounds via daemon restart"; SKIPPED[$i]=1; }
      if [ $((round % 3)) -eq 0 ]; then          # auto-recover, don't just sideline for ~10min
        [ -n "$recov" ] && { usb_recover "$recov" && sleep 6; }
        ni="$(level_idx "$pa")"
        if [ "$ni" != "0" ]; then LVLS[$i]="$ni"; SKIPPED[$i]=""; log "$name: USB RECOVERED (level id $ni) after daemon restart"; fi
      fi
      continue
    fi
    wav="$OUT/${name}-latest.wav"   # rolling (latest clean = preview); flagged ones preserved below
    cb="$(err_counters "$pa")"
    # per-engine patch: refdev gets the subtractive patch; the gpu synth gets a CLEAN
    # engine (FM = param id 1 @ 0.06 — 0 spikes + dark, vs the rough field engine at
    # its default 0.972) at its OWN defaults — the refdev patch (2..6) mangles it.
    if [ "$kind" = "synth" ]; then patch="--set 1=0.06 --set $lvl=0.8"
    else patch="--set 2=0.5 --set 3=0.5 --set 4=0.35 --set 5=0.01 --set 6=0.3 --set $lvl=0.8"; fi
    out=$(env $ce HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" $patch \
          --channel 1 --part 1 --notes "$notes" --seconds 8 --realtime --out "$wav" 2>&1)
    hostrms=$(echo "$out" | grep -oE 'rms=[0-9.]+' | head -1)
    hostund=$(echo "$out" | grep -oE 'underruns: [0-9]+' | head -1)
    if ! echo "$out" | grep -q "rms="; then log "$name r$round: NO RENDER ($(echo "$out"|grep -iE 'no HARP|error|timed'|head -1))"; continue; fi
    ca="$(err_counters "$pa")"
    delta="$(counter_delta "$cb" "$ca")"
    verdict="$(python3 "$READBACK" "$wav" 2>&1 | sed -n '1p' | grep -oE '(ok|<<<.*)$')"
    flag=""
    [ -n "$delta" ] && flag="  COUNTER-DELTA[$(echo "$delta"|tr '\n' ';')]"
    if echo "$verdict" | grep -q '<<<'; then
      if [ "$kind" != "refdev" ]; then
        flag="$flag  [engine:$verdict]"        # gpu synth's own transients/DC, not HARP
      elif echo "$verdict" | grep -q 'DROPOUT' && [ -z "$delta" ] && ! echo "$verdict" | grep -qE 'spike|stutter|clip|DC offset|SILENT'; then
        # a refdev GAP with CLEAN device counters + no corruption = the HOST render thread
        # starved (busy Mac), NOT a HARP transport fault (device-side loss always counts).
        flag="$flag  [host-jitter:$verdict]"
      else
        flag="$flag  ANALYZER[$verdict]"       # real: corruption, or a gap the DEVICE counted
      fi
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
  # every 15th round: OFFLINE (host-paced) determinism — the §8.7 BOUNCE path, vs the
  # realtime free-running RTP every other render uses. Bounce a fixed phrase twice and
  # assert bit-exact (run-to-run determinism); a MISMATCH = a real regression, an EMPTY
  # hash = the host-paced recv-cancellation hang. Covers the other half of §8.7.
  if [ $((round % 15)) -eq 0 ]; then
    oh() { env HARP_ETH_DEVICE=kria.local:47987 HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" --set 3=0.6 --set 7=0.8 --channel 1 --part 1 --notes 50,53,57,60 --seconds 4 --hash 2>/dev/null | grep -oE 'output-hash: [0-9a-f]+' | awk '{print $2}'; }
    h1="$(oh)"; h2="$(oh)"
    if [ -n "$h1" ] && [ "$h1" = "$h2" ]; then log "--- offline host-paced determinism: bit-exact ($h1) ---"
    else log "--- offline host-paced: MISMATCH h1=${h1:-EMPTY} h2=${h2:-EMPTY} (REGRESSION/hang) ---"; fi
  fi
  # every 20th round: RECALL UNDER CHURN — save the device state with distinctive params, perturb
  # the device with a burst of reconnects (each setting a DIFFERENT value), then load the saved
  # state on a fresh connect and assert the render is BIT-EXACT to the saved one. The §11.4 recall
  # round-trip must survive churn: the loaded state must override the churned params (map + audio).
  if [ $((round % 20)) -eq 0 ]; then
    rh() { env HARP_ETH_DEVICE=kria.local:47987 HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" "$@" \
           --channel 1 --part 1 --notes 50,53,57,60 --seconds 4 --hash 2>/dev/null | grep -oE 'output-hash: [0-9a-f]+' | awk '{print $2}'; }
    hA="$(rh --set 3=0.55 --set 7=0.72 --save-state /tmp/soak-recall.state)"
    for c in 1 2 3; do env HARP_ETH_DEVICE=kria.local:47987 HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" \
        --set 7=0.4 --channel 1 --part 1 --notes 50 --seconds 1 --realtime --out /tmp/churn.wav >/dev/null 2>&1; done
    hB="$(rh --load-state /tmp/soak-recall.state)"
    if [ -n "$hA" ] && [ "$hA" = "$hB" ]; then log "--- recall-under-churn: bit-exact ($hA) survived 3 reconnects ---"
    else log "--- recall-under-churn: MISMATCH hA=${hA:-EMPTY} hB=${hB:-EMPTY} (state lost/drifted under churn) ---"; fi
  fi
  # every 30th round: a MULTI-MINUTE DRIFT stream on kria — the §7.3 free-running RTP clock
  # recovery should hold a BOUNDED clock_drift_ppb with NO reanchors over minutes (the 60s
  # sustained leg is too short to surface slow drift). Sample the drift gauge before/after, count
  # reanchors (host) + the ERRC delta (device); a reanchor spike or a counter delta = a clock fault.
  if [ $((round % 30)) -eq 0 ]; then
    log "--- drift: 4-min stream on kria (free-run clock-recovery stability over minutes) ---"
    cb="$(err_counters "-d kria.local:47987")"
    db="$(pt -d kria.local:47987 counters 2>/dev/null | grep -oE 'clock_drift_ppb = -?[0-9]+' | grep -oE -- '-?[0-9]+$')"
    dout=$(env HARP_ETH_DEVICE=kria.local:47987 HARP_RECONCILE_TIMEOUT_MS=0 "$HOST" "$BUNDLE" \
          --set 3=0.5 --set 7=0.7 --channel 1 --part 1 \
          --notes 50,53,57,60,55,58,62,53 --seconds 240 --realtime --out "$OUT/drift-latest.wav" 2>&1)
    ca="$(err_counters "-d kria.local:47987")"; dd="$(counter_delta "$cb" "$ca")"
    da="$(pt -d kria.local:47987 counters 2>/dev/null | grep -oE 'clock_drift_ppb = -?[0-9]+' | grep -oE -- '-?[0-9]+$')"
    # reanchors aren't in the host's render summary — read the kria daemon journal (best-effort).
    re="$(ssh -o ConnectTimeout=6 -o BatchMode=yes ubuntu@kria.local 'journalctl -u harp-deviced --no-pager -n 6 2>/dev/null | grep -oE "[0-9]+ reanchors" | tail -1' 2>/dev/null)"
    und="$(echo "$dout" | grep -oE 'underruns: [0-9]+' | head -1)"
    fl=""
    [ -n "$dd" ] && fl="  COUNTER-DELTA[$(echo "$dd"|tr '\n' ';')]"
    rn="$(echo "${re:-0}" | grep -oE '[0-9]+')"; { [ -n "$rn" ] && [ "$rn" -gt 2 ]; } 2>/dev/null && fl="$fl  REANCHOR-SPIKE($re)"
    log "--- drift result: clock_drift_ppb ${db:-?}->${da:-?} ${re:-reanchors=?} ${und:-} ${fl:-ok} ---"
  fi
done
log "=== SOAK END (round $round) ==="
