#!/bin/bash
# soak-matrix — release-grade flood validation across every supported DAW
# buffer size, 32..1024. Not part of per-change hw-tests (too slow);
# run before tagging anything. 32 entered the matrix after a field
# session ran it for real (five random LFOs, 10 min, evt_late 0).
set -u
cd "$(dirname "$0")/.."
S=${1:-45}
FAIL=0
for B in 32 64 128 256 512 1024; do
    echo "════ block $B, ${S}s"
    if ! BLOCK=$B ./scripts/soak.sh "$S"; then
        rc=$?
        [ $rc -eq 2 ] && exit 2 # claim conflict: nothing below will work
        FAIL=$((FAIL+1))
    fi
done
echo "════ soak-matrix: $((6-FAIL))/6 passed"
[ $FAIL -eq 0 ]
