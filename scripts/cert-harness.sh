#!/usr/bin/env bash
# cert-harness.sh — the unified HARP §17.3 T1–T17 conformance harness + report.
#
# The maturity capstone flagged that although the individual conformance tests exist and run
# (ci.yml, eth.yml, hw.yml, soak.yml), NO single runner indexes the T1–T17 certification battery
# by number, runs the cloud-capable subset, and emits one pass/fail/skip/uncovered report. This
# is that runner. It is a pure ADDITION — it re-uses the existing test scripts and unit suite
# unchanged (no test or golden is modified); it only orchestrates and reports.
#
# WHAT IT DOES
#   • Reads scripts/cert-tests.tsv — the single index mapping each T# → its §17.3 title, its
#     headline class (cloud|partial|rig|uncovered), and every concrete covering test.
#   • RUNS the cloud-capable covering tests (loopback eth / unit / golden — no physical rig),
#     capability-gated exactly like eth-suite.sh: a covering test whose binaries this lane did
#     not build SKIPs with a logged reason (it runs per-PR in the eth.yml 3-OS matrix).
#   • SKIPs rig-only coverage (real USB unplug/replug, sleep/wake, power-loss, chained hubs,
#     24h soak, multi-device-at-scale) with the logged reason — those run in hw.yml / soak.yml
#     when the self-hosted rig is online (HARP_RIG_ONLINE=1).
#   • Marks the genuinely UNCOVERED tests explicitly (honest gaps for follow-up).
#   • Emits a per-T table + a final summary line, and exits NONZERO ONLY on a genuine FAIL —
#     never on a SKIP (rig / build-gated) or an uncovered gap.
#
# CLOUD vs RIG (why the split is authoritative, not guessed): the cloud subset is exactly the
# set eth-suite.sh (eth.yml) and ci.yml already run on GitHub-hosted runners against a localhost
# harp-deviced over TCP+RTP — no hardware. The rig subset is exactly what hw-tests-linux.sh
# (hw.yml) and soak.yml hw-soak drive over real USB on PI4B-0002.
#
# USAGE
#   scripts/cert-harness.sh            run the cloud subset, print the report
#   DRY_RUN=1 scripts/cert-harness.sh  print the run/skip plan for this host and exit 0
#   CERT_TIMEOUT=180 scripts/...       per-test wall-clock cap in seconds (default 300)
#   DEVICED=… PROBE=… HOSTBIN=… CHOST=… FENCE=…  override binary locations (else auto-located)
#
# Binaries are auto-located with the SAME convention as eth-suite.sh, so a normal
#   cmake -B build && cmake --build build -j
#   cmake -B build-vst -S tools/vst3-host && cmake --build build-vst -j
# is all a runner needs.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
MANIFEST="${CERT_MANIFEST:-$ROOT/scripts/cert-tests.tsv}"
[ -f "$MANIFEST" ] || { echo "cert-harness: manifest not found: $MANIFEST" >&2; exit 2; }

# Headless recall reconcile: never block a recall test waiting for a front-panel pick (matches
# hw-tests-linux.sh). Only sets a DEFAULT; a caller can override. Guards the documented
# 30s-reconcile-timeout flake without changing any test.
export HARP_RECONCILE_TIMEOUT_MS="${HARP_RECONCILE_TIMEOUT_MS:-0}"
# UTF-8 for the sub-scripts' python status prints (matches eth-suite.sh; harmless elsewhere).
export PYTHONIOENCODING="${PYTHONIOENCODING:-utf-8}"

CERT_TIMEOUT="${CERT_TIMEOUT:-300}"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) OSID=windows; EXE=.exe ;;
  Darwin)               OSID=macos;   EXE= ;;
  *)                    OSID=linux;   EXE= ;;
esac

find1() { find "$1" -name "$2" -type f 2>/dev/null | head -1; }

# ---- locate binaries (env override wins; same layout logic as eth-suite.sh) ----
if [ -z "${DEVICED:-}" ]; then
  if   [ -x "./build/harp-deviced" ];         then DEVICED="./build/harp-deviced"
  elif [ -x "./build-dev/harp-deviced.exe" ]; then DEVICED="./build-dev/harp-deviced.exe"
  else DEVICED="$(find1 . "harp-deviced$EXE")"; fi
fi
if [ -z "${PROBE:-}" ]; then
  if   [ -x "./build/harp-probe" ];         then PROBE="./build/harp-probe"
  elif [ -x "./build-dev/harp-probe.exe" ]; then PROBE="./build-dev/harp-probe.exe"
  else PROBE="$(find1 . "harp-probe$EXE")"; fi
fi
[ -n "${HOSTBIN:-}" ] || HOSTBIN="$(find1 build-vst "harp-vst3-host$EXE")"
[ -n "${VHOST:-}"   ] || VHOST="$HOSTBIN"
[ -n "${CHOST:-}"   ] || CHOST="$(find1 build-vst "clap-host$EXE")"
if [ -z "${FENCE:-}" ]; then
  if   [ -x "./build/harp-eth-fence-test" ];         then FENCE="./build/harp-eth-fence-test"
  elif [ -x "./build-dev/harp-eth-fence-test.exe" ]; then FENCE="./build-dev/harp-eth-fence-test.exe"
  else FENCE="$(find1 . "harp-eth-fence-test$EXE")"; fi
fi
export DEVICED PROBE HOSTBIN VHOST CHOST FENCE

# a ctest build dir carrying the unit suite (root cmake tree)
UNIT_DIR=""
for d in build build-cov build-rel .; do
  [ -f "$d/CTestTestfile.cmake" ] && { UNIT_DIR="$d"; break; }
done

have() { [ -n "${1:-}" ] && [ -x "$1" ]; }

# requires-token -> "present?" predicate (returns 0 if satisfiable in this lane)
req_ok() {
  case "$1" in
    device) have "$DEVICED" ;;
    probe)  have "$PROBE" ;;
    host)   have "$HOSTBIN" ;;
    clap)   have "$CHOST" ;;
    fence)  have "$FENCE" ;;
    unit)   [ -n "$UNIT_DIR" ] ;;
    -|"")   true ;;
    *)      false ;;
  esac
}
# human name for a missing requirement
req_bin() {
  case "$1" in
    device) echo harp-deviced ;; probe) echo harp-probe ;;
    host)   echo harp-vst3-host ;; clap) echo clap-host ;;
    fence)  echo harp-eth-fence-test ;; unit) echo "ctest build dir" ;;
    *)      echo "$1" ;;
  esac
}

# ---- portable per-test timeout: prefer coreutils `timeout`, else a perl SIGALRM shim.
# TERM first (lets the sub-script's EXIT trap reap its daemon), then KILL. Exit 124 on timeout.
run_bounded() {
  local secs="$1"; shift
  if command -v timeout >/dev/null 2>&1; then
    timeout -k 5 "$secs" "$@"; return $?
  fi
  perl -e '
    my $t = shift; my $pid = fork();
    if (!defined $pid) { exit 127 }
    if ($pid == 0) { exec @ARGV or exit 127 }
    my $timed = 0;
    $SIG{ALRM} = sub { $timed = 1; kill "TERM", $pid; };
    alarm $t;
    waitpid($pid, 0); my $rc = $?;
    if ($timed) { select(undef,undef,undef,3); kill "KILL", $pid; exit 124 }
    exit($rc >> 8 ? $rc >> 8 : ($rc & 127 ? 128 + ($rc & 127) : 0));
  ' "$secs" "$@"
}

# ───────── read the manifest ─────────
# Portable to bash 3.2 (macOS): no associative arrays. T ids are T1..T17, so we key
# per-T data by the integer N (from "T<N>") in plain indexed arrays. TORDER holds the
# integers in first-appearance order; ROW_* are parallel indexed arrays over all rows.
TORDER=""
NROWS=0
while IFS=$'\t' read -r t kind req cmd note; do
  case "$t" in ''|\#*) continue ;; esac
  n="${t#T}"
  if [ "$kind" = "meta" ]; then
    CLASS[$n]="$req"; TITLE[$n]="$note"
    case " $TORDER " in *" $n "*) : ;; *) TORDER="$TORDER $n" ;; esac
    continue
  fi
  ROW_T[$NROWS]="$n"; ROW_KIND[$NROWS]="$kind"; ROW_REQ[$NROWS]="$req"
  ROW_CMD[$NROWS]="$cmd"; ROW_NOTE[$NROWS]="$note"
  NROWS=$((NROWS+1))
done < "$MANIFEST"

# ───────── banner ─────────
echo "════════════════════════════════════════════════════════════════════════════"
echo " HARP §17.3 conformance harness — T1–T17 (host: $OSID)"
echo "════════════════════════════════════════════════════════════════════════════"
echo "   manifest : $MANIFEST"
echo "   DEVICED  : ${DEVICED:-<none>}   $(have "$DEVICED" && echo present || echo ABSENT)"
echo "   PROBE    : ${PROBE:-<none>}   $(have "$PROBE" && echo present || echo ABSENT)"
echo "   HOSTBIN  : ${HOSTBIN:-<none>}   $(have "$HOSTBIN" && echo present || echo ABSENT)"
echo "   CHOST    : ${CHOST:-<none>}   $(have "$CHOST" && echo present || echo ABSENT)"
echo "   FENCE    : ${FENCE:-<none>}   $(have "$FENCE" && echo present || echo ABSENT)"
echo "   UNIT dir : ${UNIT_DIR:-<none>}"
echo "   per-test timeout: ${CERT_TIMEOUT}s   DRY_RUN=${DRY_RUN:-0}"
echo ""

# ───────── per-T status accounting ─────────
FAILS=0

# For each T (in manifest order), evaluate its rows.
for n in $TORDER; do
  t="T$n"
  cls="${CLASS[$n]:-?}"
  title="${TITLE[$n]:-<no title>}"
  echo "──────────────────────────────────────────────────────────────────────────"
  echo "$t  [$cls]  $title"

  ran_pass=0; ran_fail=0; skipped_build=0; skipped_rig=0; uncovered=0
  det=""   # detail note captured for the report line

  for i in $(seq 0 $((NROWS-1))); do
    [ $NROWS -eq 0 ] && break
    [ "${ROW_T[$i]}" = "$n" ] || continue
    kind="${ROW_KIND[$i]}"; req="${ROW_REQ[$i]}"; cmd="${ROW_CMD[$i]}"; note="${ROW_NOTE[$i]}"
    case "$kind" in
      rig)
        echo "   ⏭ RIG   $note"
        skipped_rig=1; [ -z "$det" ] && det="rig: ${note%% *}…" ;;
      uncovered)
        echo "   ✗ UNCOVERED  $note"
        uncovered=1; det="uncovered" ;;
      run)
        # capability gate: every required binary must be present
        miss=""
        for tok in ${req//,/ }; do req_ok "$tok" || miss="$miss $(req_bin "$tok")"; done
        if [ -n "$miss" ]; then
          echo "   ⏭ SKIP  ${cmd##*/} — missing:${miss} (cloud test; runs per-PR in the eth.yml matrix)"
          skipped_build=1; continue
        fi
        rcmd="${cmd//@UNIT@/$UNIT_DIR}"
        if [ "${DRY_RUN:-0}" = 1 ]; then
          echo "   ▶ WOULD RUN  ${note}  [$rcmd]"
          continue
        fi
        echo "   ▶ RUN   ${note}"
        echo "     \$ $rcmd"
        echo "::group::cert-harness $t: ${cmd##*/}"
        if run_bounded "$CERT_TIMEOUT" bash -c "$rcmd"; then
          echo "::endgroup::"
          echo "   ✓ PASS  ${cmd##*/}"
          ran_pass=1
        else
          rc=$?
          echo "::endgroup::"
          if [ "$rc" = 124 ]; then
            echo "   ✗ FAIL  ${cmd##*/} — TIMED OUT after ${CERT_TIMEOUT}s (a hang is a §17 fail)"
          else
            echo "::error::cert-harness $t ${cmd##*/} FAILED (rc=$rc)"
            echo "   ✗ FAIL  ${cmd##*/} (rc=$rc)"
          fi
          ran_fail=1
        fi ;;
    esac
  done

  # aggregate this T
  if   [ "${DRY_RUN:-0}" = 1 ]; then ST[$n]="PLAN"
  elif [ $ran_fail -eq 1 ];    then ST[$n]="FAIL";        FAILS=$((FAILS+1))
  elif [ $ran_pass -eq 1 ];    then
       if [ "$cls" = partial ]; then ST[$n]="PASS-PARTIAL"; else ST[$n]="PASS-CLOUD"; fi
  elif [ $skipped_build -eq 1 ]; then ST[$n]="SKIP-BUILD"
  elif [ $skipped_rig -eq 1 ];   then ST[$n]="SKIP-RIG"
  elif [ $uncovered -eq 1 ];     then ST[$n]="UNCOVERED"
  else ST[$n]="SKIP-RIG"; fi
done

# ───────── final report table ─────────
echo ""
echo "════════════════════════════════════════════════════════════════════════════"
echo " CERTIFICATION REPORT — HARP §17.3 T1–T17  (host: $OSID)"
echo "════════════════════════════════════════════════════════════════════════════"
printf " %-4s  %-9s  %-13s  %s\n" "T" "CLASS" "STATUS" "TITLE"
printf " %-4s  %-9s  %-13s  %s\n" "----" "---------" "-------------" "-----------------------------------------"
c_cloud=0; c_partial=0; c_rig=0; c_uncov=0; c_skipbuild=0; c_fail=0
for n in $TORDER; do
  st="${ST[$n]:-?}"; cls="${CLASS[$n]:-?}"; title="${TITLE[$n]:-}"
  # trim long titles for the table
  short="$title"; [ ${#short} -gt 60 ] && short="${short:0:57}..."
  printf " %-4s  %-9s  %-13s  %s\n" "T$n" "$cls" "$st" "$short"
  case "$st" in
    PASS-CLOUD)   c_cloud=$((c_cloud+1)) ;;
    PASS-PARTIAL) c_partial=$((c_partial+1)) ;;
    SKIP-RIG)     c_rig=$((c_rig+1)) ;;
    SKIP-BUILD)   c_skipbuild=$((c_skipbuild+1)) ;;
    UNCOVERED)    c_uncov=$((c_uncov+1)) ;;
    FAIL)         c_fail=$((c_fail+1)) ;;
  esac
done
echo "════════════════════════════════════════════════════════════════════════════"

if [ "${DRY_RUN:-0}" = 1 ]; then
  echo " DRY_RUN: plan only, nothing executed."
  exit 0
fi

TOTAL=0; for t in $TORDER; do TOTAL=$((TOTAL+1)); done
echo " SUMMARY ($TOTAL T's):  cloud-PASS=$c_cloud  partial-PASS=$c_partial  rig-SKIP=$c_rig  build-SKIP=$c_skipbuild  UNCOVERED=$c_uncov  FAIL=$c_fail"
echo ""
echo " Legend:"
echo "   PASS-CLOUD    a cloud test ran on loopback/unit/golden and passed (per-PR gated)"
echo "   PASS-PARTIAL  a cloud proxy ran+passed; a material part is rig-deepened (see the RIG rows)"
echo "   SKIP-RIG      rig-only (real USB / sleep-wake / power-loss / chained-hub / 24h / multi-device);"
echo "                 runs in hw.yml + soak.yml when HARP_RIG_ONLINE=1"
echo "   SKIP-BUILD    a cloud test exists but its binary was not built in this lane;"
echo "                 it runs per-PR in the eth.yml 3-OS matrix"
echo "   UNCOVERED     honest gap — no test covers this yet (follow-up)"
echo "════════════════════════════════════════════════════════════════════════════"

if [ $c_fail -gt 0 ]; then
  echo " RESULT: FAIL — $c_fail T('s) had a failing cloud test."
  exit 1
fi
echo " RESULT: OK — no cloud FAILs. (SKIP/UNCOVERED are informational, not failures.)"
exit 0
