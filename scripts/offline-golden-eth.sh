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
PORT="${PORT:-17990}"
export HARP_RECONCILE_TIMEOUT_MS=0   # headless: no front panel to answer a recall pick

# The canonical golden render (same musical content as scripts/golden-test.sh): a clean
# patch (SETTLE) plus a 4-note line. Deterministic input => deterministic bounce.
# Device param ids are CONTIGUOUS 1..12 (engine 2.1.0 renumber: Master Level 8->7,
# arp 9..12 -> 8..11, Glide 13->12); the same value lands on the same param, so the
# bounce hash is UNCHANGED by the renumber and the per-OS pins below still hold.
SETTLE="--set 1=0.5 --set 2=0.6 --set 3=0.7 --set 4=0.5 --set 5=0.1 --set 6=0.2 \
        --set 7=0.6 --set 8=0 --set 9=0.6 --set 10=0.5 --set 11=0 --set 12=0"
SEQ="--notes 62,69,74,65 --seconds 2.6 --hash"

PASS=0; FAIL=0
ok()  { PASS=$((PASS + 1)); echo "   ✓ $1"; }
bad() { FAIL=$((FAIL + 1)); echo "   ✗ FAIL: $1"; }

# ── PINNED GROUND TRUTH (audit gap #2) ───────────────────────────────────────────
# The relative oracles below (run-to-run ==, cross-format VST3==CLAP, RMS floor) prove
# the bounce is DETERMINISTIC and NON-SILENT — but a deterministic-but-WRONG engine (a
# DSP regression that changes the output yet stays reproducible) sails straight through
# them. So we ALSO assert the bounce hash equals a PINNED value captured from known-good
# GREEN main runs. The pin is PER-OS: ubuntu and windows are both x86-64 yet hash
# DIFFERENTLY (glibc vs MSVCRT libm, gcc vs MSVC make expf/sinf not bit-reproducible),
# so a shared-x86 pin is wrong — keyed on uname, same idiom as scripts/eth-suite.sh.
#
# *** UPDATING THE PINS — only when a DSP/render change is INTENTIONAL: ***
#   1. Land the change; let eth.yml go GREEN on all three OSes.
#   2. Copy the new "VST3: hash#1=" (== CLAP == cross-format) and "post-toggle tail #1="
#      values from EACH per-OS job log.
#   3. Replace the matching PIN_BOUNCE/PIN_TOGGLE below, and say in the commit that the
#      DSP change was deliberate — so the pin bump is auditable, not a rubber-stamp.
# A runner-image bump can also legitimately shift the hash with no source change (new
# libm); re-baseline the same way and note it. An UNEXPECTED mismatch is a HARD FAILURE:
# the engine changed silently.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) OSID=windows ;;
    Darwin)               OSID=macos   ;;
    *)                    OSID=linux   ;;
esac
case "$OSID" in
    linux)   PIN_BOUNCE=269340920125c6a1; PIN_TOGGLE=e8221a2dab9b1abc ;;
    macos)   PIN_BOUNCE=154cba5fe21e31b7; PIN_TOGGLE=e8d637b2234e9894 ;;
    windows) PIN_BOUNCE=bbd4641960ded032; PIN_TOGGLE=48e46816b8f6e5a6 ;;
esac
# GOLDEN_PIN=0 disables the pin (for an intentional, in-progress DSP change whose new
# value isn't captured yet). OPT-OUT and LOUD: default is enforced; a skip warns on
# stderr so it can't hide. CI must NOT set GOLDEN_PIN.
pin_check() {  # pin_check LABEL ACTUAL EXPECTED
    if [ "${GOLDEN_PIN:-1}" = "0" ]; then
        echo "   ! pin SKIPPED for $1 (GOLDEN_PIN=0) — ground truth NOT enforced" >&2
        return
    fi
    [ -n "$2" ] || return   # no audio: the determinism/floor gate already failed loudly
    if [ "$2" = "$3" ]; then
        ok "$1 matches PINNED ground truth ($3) [$OSID]"
    else
        bad "$1 DRIFTED from pin: got $2 expected $3 [$OSID] — engine changed (bump the pin ONLY if intentional)"
    fi
}

[ -x "$DEVICED" ] || { echo "OFFLINE-GOLDEN-ETH FAIL: $DEVICED not built"; exit 1; }

# render NAME HOST PLUG: start a fresh-state daemon, render the canonical sequence in
# DEFAULT (kOffline / CLAP_RENDER_OFFLINE) mode over §8.7 host-paced TCP, echo the hash.
render() {
    sd="ogeth-$2.$$"   # workspace-RELATIVE: Git Bash path-converts an absolute /tmp arg to
    rm -rf "$sd"; : > "$sd.log"   # C:/Users/.../Temp/..., whose drive component trips the
    "$DEVICED" --port "$PORT" --state-dir "$sd" --panel-sock "" > "$sd.log" 2>&1 &  # device's recursive mkdir
    dpid=$!
    i=0
    while [ $i -lt 25 ]; do grep -q "listening on $PORT" "$sd.log" 2>/dev/null && break; sleep 0.2; i=$((i + 1)); done
    grep -q "listening on $PORT" "$sd.log" 2>/dev/null || { echo "OFFLINE-GOLDEN: device did NOT start on $PORT — render would be SILENCE" >&2; cat "$sd.log" >&2; }
    h=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' "$1" "$3" $SETTLE $SEQ 2>>"$sd.err" | sed -nE 's/output-hash: //p')
    kill -9 "$dpid" 2>/dev/null
    wait "$dpid" 2>/dev/null
    [ -n "$h" ] || { echo "  render($2): NO output-hash (alarm-killed/hung) — host err + device log:" >&2; tail -30 "$sd.err" 2>/dev/null | sed 's/^/    /' >&2; tail -8 "$sd.log" 2>/dev/null | sed 's/^/    /' >&2; }
    PORT=$((PORT + 1))   # fresh port per render (TIME_WAIT-safe)
    printf '%s' "$h"
}

# toggle_tail: clap-host renders a note line LIVE (free-running RTP), then flips to OFFLINE
# on the LIVE active plugin at 2 s (render->set -> the §8.7 mid-stream re-dial). Echoes the
# tail-hash = the POST-toggle window only (the pre-toggle free-running portion is non-
# deterministic and excluded). Notes span the toggle so the tail carries real audio.
toggle_tail() {
    sd="ogeth-tg$PORT"   # workspace-relative (dodges the /tmp -> C:\ mkdir choke, see render)
    rm -rf "$sd"; : > "$sd.log"
    "$DEVICED" --port "$PORT" --state-dir "$sd" --panel-sock "" > "$sd.log" 2>&1 &
    dpid=$!
    i=0
    while [ $i -lt 25 ]; do grep -q "listening on $PORT" "$sd.log" 2>/dev/null && break; sleep 0.2; i=$((i + 1)); done
    h=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' "$CHOST" "$CPLUG" \
        --notes 60,64,67,72,60,64,67,72 --seconds 5 --toggle-offline-at 2 --hash \
        2>>"$sd.err" | sed -nE 's/tail-hash: //p')
    kill -9 "$dpid" 2>/dev/null
    wait "$dpid" 2>/dev/null
    PORT=$((PORT + 1))
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
    # ground-truth pin: v1==CLAP (cross-format assert below) so pinning v1 pins CLAP too.
    pin_check "VST3 offline bounce" "$v1" "$PIN_BOUNCE"
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

# ── absolute-signal floor: the offline bounce must be NON-SILENT ─────────────────
# Determinism (#1==#2) and cross-format equality (VST3==CLAP) BOTH pass on SILENCE — a
# device that never started or a host that never connected renders zeros, which are
# deterministic AND byte-identical. (Exactly this hid for a while on Windows: the device
# silently failed to start, so the golden was vacuous silence yet "4 passed".) So an
# absolute-signal floor is required NEXT to the equality oracles: render once more with
# rms reporting and assert the 4-note line is audibly above zero.
if [ -n "${VPLUG:-}" ] && [ -d "$VPLUG" ] && [ -x "$VHOST" ]; then
    sd="ogeth-rms.$$"; rm -rf "$sd"; : > "$sd.log"
    "$DEVICED" --port "$PORT" --state-dir "$sd" --panel-sock "" > "$sd.log" 2>&1 &
    dpid=$!; i=0
    while [ $i -lt 25 ]; do grep -q "listening on $PORT" "$sd.log" 2>/dev/null && break; sleep 0.2; i=$((i + 1)); done
    vr=$(HARP_ETH_DEVICE="127.0.0.1:$PORT" perl -e 'alarm 30; exec @ARGV' "$VHOST" "$VPLUG" $SETTLE --notes 62,69,74,65 --seconds 2.6 --json 2>>"$sd.err" | sed -nE 's/.*"rms":([0-9.]+).*/\1/p')
    kill -9 "$dpid" 2>/dev/null; wait "$dpid" 2>/dev/null; PORT=$((PORT + 1))
    echo "──── non-silent: VST3 offline-bounce rms=${vr:-<none>}  (silence=0 sails through determinism)"
    if [ -n "$vr" ] && awk "BEGIN{exit !($vr > 0.02)}"; then
        ok "offline bounce is NON-SILENT (rms $vr) — the golden reflects real audio, not silence"
    else
        bad "offline bounce is SILENT (rms=${vr:-none}) — determinism/cross-format were passing vacuously"
    fi
fi

# ── mid-stream live->offline toggle (CLAP) ──────────────────────────────────────
# A real CLAP/AU host flips render mode on a LIVE active plugin (no deactivate). v1
# RE-DIALS the §8.7 session free-running -> host-paced mid-stream. clap-host renders LIVE
# then flips to OFFLINE at 2 s; the POST-toggle tail must be deterministic host-paced
# (run-to-run equal) — proving the re-dial switched modes (a stale live session would
# leave the tail non-deterministic). The pre-toggle free-running portion is excluded.
if [ -n "${CPLUG:-}" ] && [ -x "$CHOST" ]; then
    g1=$(toggle_tail)
    g2=$(toggle_tail)
    echo "──── mid-stream toggle: post-toggle tail #1=${g1:-<none>}  #2=${g2:-<none>}"
    if [ -n "$g1" ] && [ "$g1" = "$g2" ]; then
        ok "mid-stream live->offline toggle re-dials to deterministic host-paced (tail $g1)"
    else
        bad "mid-stream toggle tail non-deterministic (#1=$g1 #2=$g2) — re-dial didn't reach host-paced"
    fi
    pin_check "mid-stream toggle tail" "$g1" "$PIN_TOGGLE"   # second independent render path
else
    echo "──── mid-stream toggle: SKIP (clap host/bundle not built)"
fi

echo "════ offline-golden-eth: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && [ "$PASS" -gt 0 ]
