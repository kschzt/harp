#!/usr/bin/env bash
# HARP static-analysis lane — cppcheck (warning + portability) over the production C and C++
# sources, against a by-design baseline (scripts/cppcheck-suppressions.txt). Exits non-zero on
# ANY finding outside the baseline, so a new real issue fails CI. Run it locally exactly as CI
# does:  bash scripts/run-cppcheck.sh
#
# The high-signal checks stay ACTIVE (uninitvar, nullPointer, bufferOverflow, ...). The baseline
# file is IDs-only (no comments — older cppcheck rejects "# ..." lines as "no id"); the rationale
# per suppressed id:
#   dangerousTypeCast        C-style casts at C / socket / VST3-SDK boundaries (C-interop code)
#   invalidPointerCast       §8.7 audio wire reinterprets byte buffers as the float sample stream
#   uninitMemberVar*         lock-free SPSC rings + per-block POD state, written-before-read (§15.1)
#   unknownMacro             VST3 SDK macros (BEGIN_FACTORY_DEF, OBJ_METHODS) are opaque to cppcheck
#   identicalConditionAfterEarlyExit:shell/eth_transport.h  double-checked locking (mutex not modeled)
#   missingInclude/System, checkersReport  cppcheck environment limits, not code
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
SUPP="--suppressions-list=scripts/cppcheck-suppressions.txt"
COMMON="--enable=warning,portability --inline-suppr --error-exitcode=2 $SUPP --quiet"

echo "── cppcheck: C sources (core/src, device, host) ──"
# shellcheck disable=SC2086
cppcheck $COMMON --std=c11 --language=c \
  -I core/include -I host -I device \
  core/src device host
rc_c=$?

echo "── cppcheck: C++ sources (shell, vst3-host) ──"
# shellcheck disable=SC2086
cppcheck $COMMON --std=c++17 --language=c++ \
  -I core/include -I host -I shell \
  shell tools/vst3-host/main.cpp
rc_cpp=$?

if [ "$rc_c" -eq 0 ] && [ "$rc_cpp" -eq 0 ]; then
  echo "cppcheck: CLEAN (no findings outside the by-design baseline)"
  exit 0
fi
echo "cppcheck: FOUND issues outside the baseline (C rc=$rc_c, C++ rc=$rc_cpp) — fix them or, if truly by-design, add a commented suppression to scripts/cppcheck-suppressions.txt"
exit 1
