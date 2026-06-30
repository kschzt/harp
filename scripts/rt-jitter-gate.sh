#!/usr/bin/env bash
# scripts/rt-jitter-gate.sh — CI performance gate for the §4.3 USB real-time feed
# thread (#133), with NO hardware.
#
# It runs tools/rt-jitter-bench (harp-rt-jitter-bench) twice under synthetic CPU
# contention and asserts BOTH halves of the #133 claim:
#
#   1. PASS arm — RT on (the shipped default). The feed thread is promoted with the
#      production harp_thread_set_realtime() (THREAD_TIME_CONSTRAINT / SCHED_FIFO).
#      Its wakeup-lateness P99.9 must stay <= PASS_US. This is the regression gate:
#      if a future change drops the RT promotion, this lateness tail blows up and
#      the gate goes red.
#
#   2. DISCRIMINATION arm — RT off (HARP_USB_RT=0, the #133 A/B "before"). The SAME
#      thread on ordinary time-share must tail PAST the gate by a margin
#      (>= PASS_US * DISC_FACTOR). This proves the gate has teeth: the metric it
#      asserts is the one RT actually moves, so a real regression cannot slip by.
#
# Thresholds are generous (loud, not flaky): on a 10-core dev Mac under 16 burners
# the RT-on lateness P99.9 is single-digit µs while RT-off is ~4-5 ms — a ~600x
# separation. PASS_US (default 1500 µs) sits two orders of magnitude above the
# RT-on tail yet far below the RT-off tail, so runner jitter cannot trip it but
# removing RT certainly does. Override any of the knobs via the environment.
set -u

BENCH="${1:-${BENCH:-}}"
if [ -z "${BENCH}" ]; then
  for c in ./build-rt/harp-rt-jitter-bench ./build/harp-rt-jitter-bench \
           ./build-test/harp-rt-jitter-bench; do
    [ -x "$c" ] && BENCH="$c" && break
  done
fi
if [ -z "${BENCH}" ] || [ ! -x "${BENCH}" ]; then
  echo "rt-jitter-gate: bench binary not found (build target harp-rt-jitter-bench)" >&2
  exit 2
fi

SAMPLES="${SAMPLES:-5000}"
WARMUP="${WARMUP:-256}"
PASS_US="${PASS_US:-1500}"       # RT-on wakeup-lateness P99.9 must be <= this (µs)
DISC_FACTOR="${DISC_FACTOR:-2}"  # RT-off must exceed PASS_US by at least this factor
BURNER_ARG=""
[ -n "${BURNERS:-}" ] && BURNER_ARG="--burners ${BURNERS}"
# Allow a sandbox that genuinely cannot grant RT to skip (off by default — a CI
# runner that silently lost RT should go red, not green).
ALLOW_SKIP="${RT_GATE_ALLOW_SKIP:-0}"

# pull an integer key=value field out of the single RTBENCH line, portably
extract() { # $1=line $2=key
  echo "$1" | tr ' ' '\n' | sed -n "s/^$2=\([0-9][0-9]*\)$/\1/p" | head -n1
}

echo "rt-jitter-gate: bench=${BENCH} samples=${SAMPLES} PASS_US=${PASS_US} DISC_FACTOR=${DISC_FACTOR}"
echo "--- arm 1: RT ON (shipped default) — must hold the gate ---"
ON_OUT="$("${BENCH}" --samples "${SAMPLES}" --warmup "${WARMUP}" ${BURNER_ARG} --require-rt --max-late-us "${PASS_US}")"
ON_RC=$?
echo "${ON_OUT}"

if [ "${ON_RC}" -eq 3 ]; then
  if [ "${ALLOW_SKIP}" = "1" ]; then
    echo "rt-jitter-gate: SKIP — host cannot grant real-time scheduling here." >&2
    exit 0
  fi
  echo "rt-jitter-gate: FAIL — real-time scheduling was not granted on this runner." >&2
  echo "  (THREAD_TIME_CONSTRAINT/SCHED_FIFO unavailable; the gate requires it. Set" >&2
  echo "   RT_GATE_ALLOW_SKIP=1 only on a runner where RT is legitimately impossible.)" >&2
  exit 1
fi
if [ "${ON_RC}" -ne 0 ]; then
  echo "rt-jitter-gate: FAIL — RT-on wakeup-lateness P99.9 exceeded ${PASS_US} µs." >&2
  echo "  The real-time promotion is no longer pinning the feed thread under load." >&2
  exit 1
fi

echo "--- arm 2: RT OFF (HARP_USB_RT=0) — must tail past the gate (proves teeth) ---"
OFF_OUT="$(HARP_USB_RT=0 "${BENCH}" --samples "${SAMPLES}" --warmup "${WARMUP}" ${BURNER_ARG})"
echo "${OFF_OUT}"

ON_P999="$(extract "${ON_OUT}" late_p999)"
OFF_P999="$(extract "${OFF_OUT}" late_p999)"
if [ -z "${ON_P999}" ] || [ -z "${OFF_P999}" ]; then
  echo "rt-jitter-gate: FAIL — could not parse late_p999 from the bench output." >&2
  exit 2
fi

DISC_MIN=$(( PASS_US * DISC_FACTOR ))
echo "rt-jitter-gate: RT-on late_p999=${ON_P999}µs  RT-off late_p999=${OFF_P999}µs  (gate=${PASS_US}µs, discrimination floor=${DISC_MIN}µs)"

if [ "${OFF_P999}" -le "${DISC_MIN}" ]; then
  echo "rt-jitter-gate: FAIL — RT-off lateness (${OFF_P999}µs) did NOT exceed the" >&2
  echo "  discrimination floor (${DISC_MIN}µs). The contention is not biting, so the" >&2
  echo "  gate would not catch a real RT regression. Raise BURNERS or SAMPLES." >&2
  exit 1
fi

echo "rt-jitter-gate: PASS — RT pins the feed-thread cadence (${ON_P999}µs <= ${PASS_US}µs)"
echo "  and removing it (HARP_USB_RT=0) blows the tail to ${OFF_P999}µs — the gate discriminates."
exit 0
