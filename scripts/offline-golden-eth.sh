#!/bin/sh
# offline-golden-eth — §8.3-over-§8.7: prove the DETERMINISTIC OFFLINE BOUNCE over
# Ethernet. A localhost harp-deviced (the real synth, no USB) serves the host-paced
# render over a TCP audio channel (audio.start key 7); the shell, told it is rendering
# OFFLINE (kOffline / CLAP_RENDER_OFFLINE), negotiates host-paced instead of free-
# running RTP and pulls exact SSI ranges — so the bounce is byte-exact and reproducible.
#
# ASSERTION = PER-FORMAT RUN-TO-RUN DETERMINISM: render the canonical sequence twice,
# each against a FRESH-state daemon, and require the two output-hashes to MATCH. This is
# the right gate here because:
#   - it needs NO pinned value, so it is robust to the engine's expf/sinf NOT being
#     bit-reproducible across arches (ubuntu-latest x86-64 vs macos-latest arm64);
#   - it needs NO cross-format alignment (vst3-host and clap-host pace in different
#     block sizes, so their hashes legitimately differ — that is a HARNESS difference,
#     not a render bug; cross-shell equivalence over USB is golden-test.sh's job).
# A non-deterministic offline bounce (the bug this whole feature's event-fence barrier
# fixes) would make the two hashes differ. The hashes are PRINTED for drift-watching.
#
# FRESH DEVICE PER RENDER: a clean factory state makes the render deterministic without
# golden-test.sh's param-settle warm-up, and one session per daemon sidesteps multi-
# session reconnect. Assumes harp-deviced + the host binaries + the bundles are already
# built (eth.yml builds them). Exit 0 pass / 1 fail.
set -u
cd "$(dirname "$0")/.."

DEVICED="${DEVICED:-./build/harp-deviced}"
VHOST="${VHOST:-./build-vst/harp-vst3-host}"
CHOST="${CHOST:-./build-vst/clap-host}"
VPLUG="${VPLUG:-$(find build-vst -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
CPLUG="${CPLUG:-$(find build-vst -maxdepth 5 -name harp-clap.clap 2>/dev/null | head -1)}"
PORT="${PORT:-47990}"
export HARP_RECONCILE_TIMEOUT_MS=0   # headless: no front panel to answer a recall pick

# The canonical golden render (same musical content as scripts/golden-test.sh): a clean
# patch (SETTLE) plus a 4-note line. Deterministic input => deterministic bounce.
SETTLE="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
        --set 7=0.5 --set 8=0.6 --set 9=0 --set 10=0.6 --set 11=0.5 --set 12=0 --set 13=0"
SEQ="--notes 62,69,74,65 --seconds 2.6 --hash"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo "   ✓ $1"; }
bad() { FAIL=$((FAIL + 1)); echo "   ✗ FAIL: $1"; }

[ -x "$DEVICED" ] || { echo "OFFLINE-GOLDEN-ETH FAIL: $DEVICED not built"; exit 1; }

# render NAME HOST PLUG: start a fresh-state daemon, render the canonical sequence in
# DEFAULT (kOffline / CLAP_RENDER_OFFLINE) mode over §8.7 host-paced TCP, echo the hash.
render() {
    sd="/tmp/ogeth-$2.$$"
    rm -rf "$sd"; : > "$sd.log"
    "$DEVICED" --port "$PORT" --state-dir "$sd" --panel-sock "" > "$sd.log" 2>&1 &
    dpid=$!
    i=0
    while [ $i -lt 25 ]; do grep -q "listening on $PORT" "$sd.log" 2>/dev/null && break; sleep 0.2; i=$((i + 1)); done
    h=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" "$1" "$3" $SETTLE $SEQ 2>>"$sd.err" | sed -nE 's/output-hash: //p')
    kill -9 "$dpid" 2>/dev/null
    wait "$dpid" 2>/dev/null
    PORT=$((PORT + 1))   # fresh port per render (TIME_WAIT-safe)
    printf '%s' "$h"
}

echo "── offline-golden-eth: deterministic host-paced bounce over §8.7 (fake hardware, kOffline)"

# ── VST3 ────────────────────────────────────────────────────────────────────────
if [ -n "$VPLUG" ] && [ -d "$VPLUG" ] && [ -x "$VHOST" ]; then
    v1=$(render "$VHOST" vst3a "$VPLUG")
    v2=$(render "$VHOST" vst3b "$VPLUG")
    echo "──── VST3: hash#1=${v1:-<none>}  hash#2=${v2:-<none>}"
    if [ -n "$v1" ] && [ "$v1" = "$v2" ]; then
        ok "VST3 offline bounce over §8.7 is deterministic (run-to-run hash $v1)"
    else
        bad "VST3 offline bounce non-deterministic / no audio (#1=$v1 #2=$v2)"
    fi
else
    echo "──── VST3: SKIP (host/bundle not built)"
fi

# ── CLAP ────────────────────────────────────────────────────────────────────────
if [ -n "$CPLUG" ] && [ -x "$CHOST" ]; then
    c1=$(render "$CHOST" clapa "$CPLUG")
    c2=$(render "$CHOST" clapb "$CPLUG")
    echo "──── CLAP: hash#1=${c1:-<none>}  hash#2=${c2:-<none>}"
    if [ -n "$c1" ] && [ "$c1" = "$c2" ]; then
        ok "CLAP offline bounce over §8.7 is deterministic (run-to-run hash $c1)"
    else
        bad "CLAP offline bounce non-deterministic / no audio (#1=$c1 #2=$c2)"
    fi
else
    echo "──── CLAP: SKIP (host/bundle not built)"
fi

# ── cross-format equality ────────────────────────────────────────────────────────
# VST3 and CLAP, given the SAME fresh-factory device + same input, render BYTE-IDENTICAL
# (the shells are thin adapters over one runtime+device; the hosts deliver the canonical
# sequence the same way). This is a strictly stronger gate than per-format determinism —
# it catches a shell- or host-specific regression that determinism alone would miss (e.g.
# clap-host establishing the session mode after the dial instead of before, which once
# made CLAP race to free-running). It needs NO pinned value (so it survives expf/sinf not
# being bit-reproducible across arches — both formats run on the SAME runner/arch here).
if [ -n "${v1:-}" ] && [ -n "${c1:-}" ]; then
    echo "──── cross-format: VST3=$v1  CLAP=$c1"
    if [ "$v1" = "$c1" ]; then
        ok "cross-format: VST3 and CLAP offline bounces are byte-identical ($v1)"
    else
        bad "cross-format: VST3 ($v1) != CLAP ($c1) — shells diverge on the same input"
    fi
fi

echo "════ offline-golden-eth: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && [ "$PASS" -gt 0 ]
