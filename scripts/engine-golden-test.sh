#!/bin/sh
# engine-golden-test — debt #19 cloud gate for the RAW device engine render.
#
# Runs harp-engine-golden, which drives render_output() / engine_voices_cold() DIRECTLY (no shell,
# no transport, no USB) over a fixed note sequence and INTERNALLY asserts the two contracts that
# hold on every platform: the render is DETERMINISTIC (run-to-run hash equality) and NON-SILENT
# (rms > 0). This script adds the absolute regression ORACLE: the rendered-byte hash, pinned
# PER-OS — libm sin/exp differ across platforms, so one hash cannot cover all three.
#
# A new platform prints its hash as UNPINNED and passes (determinism + non-silence still gate);
# capture that value from the CI log and pin it below to turn on the regression oracle there.
set -e
cd "$(dirname "$0")/.."
BIN="${ENGINE_GOLDEN:-./build/harp-engine-golden}"
[ -x "$BIN" ] || { echo "ENGINE-GOLDEN FAIL: $BIN not built"; exit 1; }

case "$(uname -s)" in
  Darwin)               WANT=e47077a6d7469763 ;;
  Linux)                WANT=21976313a078b4ec ;;
  MINGW*|MSYS*|CYGWIN*) WANT= ;;   # capture-then-pin from CI
  *)                    WANT= ;;
esac

out=$("$BIN")          # exits nonzero on non-determinism / silence (the portable gates)
echo "$out"
got=$(printf '%s' "$out" | sed -n 's/^engine-render-hash: //p')
[ -n "$got" ] || { echo "ENGINE-GOLDEN FAIL: no hash emitted"; exit 1; }

if [ -z "$WANT" ]; then
  echo "ENGINE-GOLDEN: $(uname -s) hash $got is UNPINNED — determinism + non-silence passed; pin this value to enable the regression oracle"
  exit 0
fi
if [ "$got" != "$WANT" ]; then
  echo "ENGINE-GOLDEN FAIL: $(uname -s) hash $got != pinned $WANT — the raw device DSP changed (re-pin DELIBERATELY, with why)"
  exit 1
fi
echo "ENGINE-GOLDEN PASS: raw device DSP render matches the pinned oracle ($got on $(uname -s))"
