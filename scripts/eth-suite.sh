#!/usr/bin/env bash
# §8.7 Ethernet/IP conformance SUITE — the single OS-matrix entry point.
#
# eth.yml runs this once per OS (ubuntu / macos / windows) AFTER a per-OS build. It is
# the one source of truth for WHICH conformance scripts run, so the test set can no
# longer drift between platforms (the old eth.yml-vs-win.yml split silently ran 5 of
# ~10 scripts on Windows). A test runs on EVERY platform by default; a skip is an
# explicit, logged, capability- or OS-gated decision — never a silent omission.
#
# Gating model (capability-based, so coverage auto-expands as binaries appear):
#   - DEVICED / HOSTBIN / VHOST / CHOST are located per-OS (env override wins) and
#     exported, so each sub-script finds its binaries without per-OS edits here.
#   - PROBE-dependent scripts (recall, offline-edit, device-side diag-bundle) run iff
#     harp-probe is built; on a platform that doesn't build it yet they SKIP (and will
#     auto-enable the moment that platform starts building harp-probe — no edit here).
#   - The per-OS strictness inside each script (loopback ±1ms on Linux / wiring-only
#     elsewhere; the bit-exact macOS/Windows retry+relaxed RMS floor) is unchanged —
#     this suite only orchestrates, it does not relax anything.
#
# DRY_RUN=1 prints the located binaries + the run/skip plan for this host and exits 0
# (used by local validation before pushing — the real multi-OS proof is the PR matrix).
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) OSID=windows; EXE=.exe ;;
  Darwin)               OSID=macos;   EXE= ;;
  *)                    OSID=linux;   EXE= ;;
esac

# ---- locate binaries (env override wins; else POSIX layout, else Windows split, else find) ----
find1() { find "$1" -name "$2" -type f 2>/dev/null | head -1; }

if [ -z "${DEVICED:-}" ]; then
  if   [ -x "./build/harp-deviced" ];          then DEVICED="./build/harp-deviced"
  elif [ -x "./build-dev/harp-deviced.exe" ];  then DEVICED="./build-dev/harp-deviced.exe"
  else DEVICED="$(find1 . "harp-deviced$EXE")"; fi
fi
[ -n "${HOSTBIN:-}" ] || HOSTBIN="$(find1 build-vst "harp-vst3-host$EXE")"
[ -n "${VHOST:-}"   ] || VHOST="$HOSTBIN"
[ -n "${CHOST:-}"   ] || CHOST="$(find1 build-vst "clap-host$EXE")"
if [ -z "${PROBE:-}" ]; then
  if   [ -x "./build/harp-probe" ];         then PROBE="./build/harp-probe"
  elif [ -x "./build-dev/harp-probe.exe" ]; then PROBE="./build-dev/harp-probe.exe"
  else PROBE="$(find1 . "harp-probe$EXE")"; fi
fi
export DEVICED HOSTBIN VHOST CHOST PROBE

have() { [ -n "${1:-}" ] && [ -x "$1" ]; }

echo "── eth-suite on $OSID"
echo "   DEVICED=$DEVICED"
echo "   HOSTBIN=$HOSTBIN"
echo "   CHOST=$CHOST"
echo "   PROBE=${PROBE:-<none>}  $(have "$PROBE" && echo '(present)' || echo '(absent — probe tests skip)')"

# ---- run / skip bookkeeping (no fail-fast: collect every result so one PR shows ALL
#      platform failures at once, minimizing branch round-trips) ----
RESULTS=""
FAILED=0
run() {  # run <name> <script...>
  name="$1"; shift
  if [ "${DRY_RUN:-0}" = 1 ]; then echo "▶ WOULD RUN $name ($*)"; RESULTS="$RESULTS
PLAN  $name"; return; fi
  echo "::group::eth-suite: $name"
  if "$@"; then echo "✓ $name"; RESULTS="$RESULTS
PASS  $name"
  else rc=$?; echo "::error::eth-suite: $name FAILED (rc=$rc)"; RESULTS="$RESULTS
FAIL  $name (rc=$rc)"; FAILED=1; fi
  echo "::endgroup::"
}
skip() { echo "⏭ SKIP $1 — $2"; RESULTS="$RESULTS
SKIP  $1 — $2"; }

# ---- the conformance list (one source of truth for all OSes) ----
run eth-tests          scripts/eth-tests.sh                  # bit-exact + multichannel ASRC over RTP
run offline-golden     scripts/offline-golden-eth.sh         # deterministic host-paced bounce
run corrupt-cbor       scripts/corrupt-cbor-eth-test.sh      # §8.7 hostile-frame fault injection
run reconnect          scripts/reconnect-eth-test.sh         # §8.7 mid-session disconnect survival
run rtp-loss           scripts/rtp-loss-eth-test.sh          # §8.7 RTP/UDP packet-loss tolerance
run diag-bundle-host   scripts/diag-bundle-host-eth-test.sh  # §14.4 runtime getDiagBundle (probe-free)
run engine-mismatch    scripts/engine-mismatch-eth-test.sh   # §12.2 engine-major change -> read-only default
run admission          scripts/admission-eth-test.sh         # §8.4 audio-bandwidth admission + refuse-with-budget
run loopback           scripts/loopback-eth-test.sh          # §14.3 round-trip RTT (probe-free)
run reported-latency   scripts/reported-latency-test.sh      # §6.4 reported PDC latency (exact, all formats)
run param-map-recall   scripts/param-map-recall-test.sh      # §13.4 recall warns on param-map drift

# §8.2 host-paced late-frame discard: the harp-eth-latefr-test tool is POSIX-only (raw
# server socket for the device's connect-back), so it isn't built on Windows.
if [ -x ./build/harp-eth-latefr-test ]; then run latefr scripts/latefr-eth-test.sh
else skip latefr "harp-eth-latefr-test not built (POSIX-only host-paced tool)"; fi

# §9.4 per-part echo demux drives the device front panel over a unix socket; the MinGW
# device replaces panel.c with a no-op stub, so the multi-instance path is POSIX-only
# until the Windows panel transport lands.
if [ "$OSID" = windows ]; then skip part-filter "MinGW device panel is a stub (§9.4 multi-instance demux pending Windows panel transport)"
else run part-filter    scripts/part-filter-eth-test.sh; fi

# §4.4.3 mDNS _harp._tcp discovery round-trip: needs a LIVE mDNS responder + a dns_sd-built
# harp-probe. macOS CI has mDNSResponder always up; Linux/Windows runners don't reliably, so
# they SKIP — the path is bench-proven on the KR260 (avahi over the direct cable). The script
# also self-skips a no-dns_sd build.
if [ "$OSID" = macos ]; then run mdns-discover scripts/mdns-discover-test.sh
else skip mdns-discover "mDNS needs a live responder (macOS CI only; bench-proven on the KR260/avahi)"; fi

# These mutate/inspect the device via harp-probe ("the musician"). harp-probe is not
# built on every platform yet (Windows: pending); the gate auto-enables them when it is.
if have "$PROBE"; then
  run recall           scripts/recall-eth-test.sh            # §11.4 recall round-trip + archive
  run credit           scripts/credit-eth-test.sh            # §4.2.1b obj credit queue/flush under starvation
  run txn              scripts/txn-test.sh                   # §9.6 event transactions: buffer/commit-atomic/abort
  run core             scripts/core-test.sh                  # §5.5 core methods: ping/identify/changed/bye
  run bloat-recall     scripts/bloat-recall-eth-test.sh      # debt #22: live ref resolves on a many-archive store (recall-breaker)
  run gc               scripts/gc-test.sh                    # debt #22a: §10.3 archive retention + mark-sweep GC (wired device path)
  run offline-edit     scripts/offline-edit-eth-test.sh      # §15.5 edit-while-absent reaches device
  run diag-bundle      scripts/diag-bundle-eth-test.sh       # §14.4 device-side export + §16 anon
else
  skip recall       "harp-probe not built on $OSID"
  skip credit       "harp-probe not built on $OSID"
  skip txn          "harp-probe not built on $OSID"
  skip core         "harp-probe not built on $OSID"
  skip bloat-recall "harp-probe not built on $OSID"
  skip gc           "harp-probe not built on $OSID"
  skip offline-edit "harp-probe not built on $OSID"
  skip diag-bundle  "harp-probe not built on $OSID"
fi

echo ""
echo "════════ eth-suite summary ($OSID) ════════$RESULTS"
echo "═══════════════════════════════════════════"
[ "$FAILED" = 0 ] && echo "eth-suite: ALL GREEN on $OSID" || echo "eth-suite: FAILURES on $OSID"

if [ "${DRY_RUN:-0}" = 1 ]; then exit 0; fi
exit "$FAILED"
