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

# §8.7 counter assertions decode the diag bundle with python3 and print a ✓/§/— status line
# (rtp-loss, rtp-reorder, ratelimit). Windows' default stdout encoding is cp1252 (charmap),
# which can't encode those chars -> UnicodeEncodeError -> python exits nonzero -> the test
# fails for a PRINT, not a real assertion. Force UTF-8 on every sub-script's python stdout so
# the status prints encode on Windows too (round-6 enabled cbor2 on Windows via eth.yml, which
# is what first exercised these prints there). Exported once here -> covers every sub-script.
export PYTHONIOENCODING=utf-8

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
. "$ROOT/scripts/eth-extern-lib.sh"

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
# harp-eth-fence-test: POSIX build/ on Linux/macOS, MinGW build-dev/*.exe on Windows (§8.3.1).
if [ -z "${FENCE:-}" ]; then
  if   [ -x "./build/harp-eth-fence-test" ];         then FENCE="./build/harp-eth-fence-test"
  elif [ -x "./build-dev/harp-eth-fence-test.exe" ]; then FENCE="./build-dev/harp-eth-fence-test.exe"
  else FENCE="$(find1 . "harp-eth-fence-test$EXE")"; fi
fi
export DEVICED HOSTBIN VHOST CHOST PROBE FENCE

have() { [ -n "${1:-}" ] && [ -x "$1" ]; }

echo "── eth-suite on $OSID"
echo "   DEVICED=$DEVICED"
echo "   HOSTBIN=$HOSTBIN"
echo "   CHOST=$CHOST"
echo "   PROBE=${PROBE:-<none>}  $(have "$PROBE" && echo '(present)' || echo '(absent — probe tests skip)')"

# ---- EXTERNAL-ENDPOINT MODE gate (capability-based, exactly like the per-OS / probe skips) ---
# HARP_ETH_EXTERN=host:port => target an already-running EXTERNAL harp-deviced over a real
# network hop instead of spawning a localhost one (closes the loopback-only gap). The DEFAULT
# (unset) spawn-local path is 100% unchanged. In external mode only the EXTERNAL-CAPABLE tests
# run (they dial $HARP_ETH_EXTERN); every other test SKIPS with a LOGGED capability reason —
# tests that structurally need a harness-owned local co-process (SIGKILL fault-injection,
# deterministic host-paced offline goldens, or a daemon started with specific flags/config).
export HARP_ETH_EXTERN HARP_ETH_EXTERN_SERIAL 2>/dev/null || true
EXTERN_CAP=" eth-tests core txn epoch cas-conflict admission loopback diag-bundle-host hello-gate "
if eth_extern_active; then
  echo ""
  echo "── eth-suite EXTERNAL-ENDPOINT MODE: targeting $(eth_extern_ep) over a REAL §8.7 hop (NOT spawning a localhost deviced)"
  [ -n "${HARP_ETH_EXTERN_SERIAL:-}" ] && echo "   external device serial pin: HARP_DEVICE_SERIAL=$HARP_ETH_EXTERN_SERIAL" || echo "   (no HARP_ETH_EXTERN_SERIAL pin — the host takes the daemon's reported serial)"
  echo "   external-capable (run):$EXTERN_CAP"
fi
# Why a given test cannot run against a single, fixed, already-running external deviced.
extern_reason() {
  case "$1" in
    main-multi-out) echo "host-side only — does not exercise the external §8.7 endpoint";;
    offline-golden|offline-fence|realtime-fence|latefr)
      echo "host-paced OFFLINE bounce needs deterministic LOCAL device-clock control (byte-exact in-process golden / connect-back socket)";;
    corrupt-cbor|reconnect|conn-flood)
      echo "fault-injection SIGKILLs/restarts the local deviced — the harness must own its lifecycle";;
    rtp-loss|rtp-reorder)
      echo "needs a harness-started --tone daemon + host-side RTP loss/reorder injection (loopback-only seam)";;
    reported-latency|eth-rtfloor|evt-storm|engine-mismatch|engine-gate)
      echo "needs a harness-configured deviced (--rt-floor/--rt-nsamples/--in-lat/--engine, often multi-config) — can't reconfigure the external endpoint";;
    ratelimit) echo "needs --force-peer-ip on a harness-started deviced (§16 shed seam)";;
    param-map-recall) echo "restarts the deviced with a mutated param-map to force §13.4 drift";;
    credit) echo "needs HARP_FORCE_CREDIT_GRANT in the DEVICE process env (a harness-started daemon)";;
    recall|archive-before-push|bloat-recall|gc|offline-edit)
      echo "mutates/inspects the device store — needs a harness-owned fresh device state-dir";;
    diag-bundle) echo "device-side harp-probe bundle needs a harness-owned state-dir; diag-bundle-host covers §8.3 counters over the real hop";;
    diag-counters) echo "storage-gauge bounds need a harness-owned state-dir; diag-bundle-host covers the §14.2 counters over the real hop";;
    part-filter|reconcile-readonly|part-param-iso)
      echo "drives the device front-panel unix socket — device-LOCAL transport, unreachable over the network hop";;
    multiout-iso|multiout-bleed|multiout-perchan|multiout-clap|multiout-au)
      echo "multi-instance perl-alarm harness needs harness-owned local device instances";;
    mdns-discover|shell-mdns) echo "needs a LOCAL mDNS responder + a harness-spawned daemon";;
    *) echo "needs a harness-managed local deviced";;
  esac
}

# ---- run / skip bookkeeping (no fail-fast: collect every result so one PR shows ALL
#      platform failures at once, minimizing branch round-trips) ----
RESULTS=""
FAILED=0
run() {  # run <name> <script...>
  name="$1"; shift
  # External mode: run only the external-capable tests; skip the rest WITH A LOGGED reason.
  if eth_extern_active; then
    case "$EXTERN_CAP" in
      *" $name "*) : ;;
      *) skip "$name" "external mode: $(extern_reason "$name")"; return ;;
    esac
  fi
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
run main-multi-out     scripts/main-multi-output-test.sh     # M5: 17-bus whole-device output layout (host-side, all OS)
run eth-tests          scripts/eth-tests.sh                  # bit-exact + multichannel ASRC over RTP
run offline-golden     scripts/offline-golden-eth.sh         # deterministic host-paced bounce
run corrupt-cbor       scripts/corrupt-cbor-eth-test.sh      # §8.7 hostile-frame fault injection
run reconnect          scripts/reconnect-eth-test.sh         # §8.7 mid-session disconnect survival
run rtp-loss           scripts/rtp-loss-eth-test.sh          # §8.7 RTP/UDP packet-loss tolerance
run rtp-reorder        scripts/rtp-reorder-eth-test.sh       # §8.7 RTP reorder: no loss over-count (shipped path)
run diag-bundle-host   scripts/diag-bundle-host-eth-test.sh  # §14.4 runtime getDiagBundle (probe-free)
run engine-mismatch    scripts/engine-mismatch-eth-test.sh   # §12.2 engine-major change -> read-only default
run admission          scripts/admission-eth-test.sh         # §8.4 audio-bandwidth admission + refuse-with-budget
run loopback           scripts/loopback-eth-test.sh          # §14.3 round-trip RTT (probe-free)
run reported-latency   scripts/reported-latency-test.sh      # §6.4 reported PDC latency (exact, all formats)
run param-map-recall   scripts/param-map-recall-test.sh      # §13.4 recall warns on param-map drift
run eth-rtfloor        scripts/eth-rtfloor-test.sh           # §6.4 rt-profile (key 14): declared floor/packet + clamps
run hello-gate         scripts/hello-gate-test.sh            # §5.4 pre-hello request denied (gate regression guard)
run evt-storm          scripts/evt-storm-eth-test.sh         # §9.2 evt_late stays 0 on the rt-nsamples=64 free-running path (#76 guard)
run staged-connected   scripts/staged-connected-eth-test.sh  # §11.4 HIGH #8: --load-state-after-connect onto a mismatched unit held read-only

# §8.2 host-paced late-frame discard: the harp-eth-latefr-test tool is POSIX-only (raw
# server socket for the device's connect-back), so it isn't built on Windows.
if [ -x ./build/harp-eth-latefr-test ]; then run latefr scripts/latefr-eth-test.sh
else skip latefr "harp-eth-latefr-test not built (POSIX-only host-paced tool)"; fi

# §8.3.1 event-fence integration: harp-eth-fence-test drives the OFFLINE host-paced bounce
# with a FENCED pacing frame + a deferred NOTE event so the device's fence branch is reached;
# asserts fence_waits moves, fence_timeouts stays 0 (unbounded offline barrier), and the
# bounce is run-to-run deterministic. Now Windows-buildable (winsock connect-back socket), so
# it runs on every OS the moment harp-eth-fence-test is built.
if have "$FENCE"; then run offline-fence scripts/offline-fence-eth.sh
else skip offline-fence "harp-eth-fence-test not built"; fi

# §8.3.1 event-fence integration, REAL-TIME side: same tool with HARP_FENCE_FORCE_RT=1 fences
# beyond the feed and asserts the device BOUNDS the wait + counts fence_timeouts (no wedge) —
# the production-path guard the unit test could not give (the loop once bypassed the helper).
# Runs on Windows too now (the §8.3.1 bound is device-side timing, identical across OSes).
if have "$FENCE"; then run realtime-fence scripts/realtime-fence-eth.sh
else skip realtime-fence "harp-eth-fence-test not built"; fi

# §9.4 per-part echo demux drives the device front panel over a unix socket; the MinGW
# device replaces panel.c with a no-op stub, so the multi-instance path is POSIX-only
# until the Windows panel transport lands.
if [ "$OSID" = windows ]; then skip part-filter "MinGW device panel is a stub (§9.4 multi-instance demux pending Windows panel transport)"
else run part-filter    scripts/part-filter-eth-test.sh; fi

# MULTI-OUT (M1): the Kontakt/Overbridge-style multi-out main — one VST3 instance, 17 output
# buses. Per-part zero-bleed isolation over free-running RTP (the wide-union >8-slot
# multi-packet path) + offline host-paced determinism (the same path USB drives). POSIX-only
# (the perl-alarm capture harness); the protocol is OS-independent so macOS+Linux cover it.
if [ "$OSID" = windows ]; then skip multiout-iso "multiout-iso uses the POSIX perl-alarm harness (Windows multi-out coverage pending)"
else run multiout-iso   scripts/multiout-iso-test.sh; fi

# MULTI-OUT (M1b): ADJACENT-part zero-bleed. multiout-iso pins isolation only between parts
# 8 slots apart (ch 3 & 7); this drives a part and asserts the IMMEDIATELY adjacent buses
# (the next slot pair — where an off-by-one/off-by-two slot bug bleeds) are BIT-EXACT silent,
# at the LOW/MIDDLE/HIGH ends of the 16-part range, plus a full-scan total-isolation pass and
# an offline bit-exact silence golden. POSIX-only (the perl-alarm capture harness).
if [ "$OSID" = windows ]; then skip multiout-bleed "multiout-bleed uses the POSIX perl-alarm harness (Windows multi-out coverage pending)"
else run multiout-bleed scripts/multiout-bleed-test.sh; fi

# MULTI-OUT (M2): per-channel PARAMS — one main instance edits any part's device params per
# event (a satellite's MIDI CC on channel N -> part N's param, §9.4 key 5). Routing + isolation
# via deterministic offline hashes. POSIX-only (the perl-alarm capture harness).
if [ "$OSID" = windows ]; then skip multiout-perchan "perl-alarm harness (Windows multi-out coverage pending)"
else run multiout-perchan scripts/multiout-perchan-test.sh; fi

# MULTI-OUT (M4): the SAME multi-out contract through the CLAP host — 17 ports
# (clap_plugin_audio_ports), per-part isolation, and per-channel CC routed as a raw
# CLAP_EVENT_MIDI (§9.4 key 5). Proves the multi-out main is first-class in CLAP, not just
# VST3. POSIX-only (perl-alarm harness) + needs the clap-host binary.
if [ "$OSID" = windows ]; then skip multiout-clap "perl-alarm harness (Windows multi-out coverage pending)"
elif have "$CHOST"; then run multiout-clap scripts/multiout-clap-test.sh
else skip multiout-clap "clap-host not built on $OSID"; fi

# MULTI-OUT (M4): the SAME contract through the AU host — 17 output ELEMENTS (per-element
# render, the main-mix pull on element 0 paces the device + caches the parts), per-part
# isolation, and per-channel CC routed as raw MusicDeviceMIDIEvent (§9.4 key 5). The AU
# golden bytes match the VST3/CLAP multi-out bounces exactly. macOS only (no AU elsewhere);
# the script self-skips if au-host / the installed component are absent.
if [ "$OSID" = macos ]; then run multiout-au scripts/multiout-au-test.sh
else skip multiout-au "AU is macOS-only (no au-host / component on $OSID)"; fi

# §11.4 action 3 "Open read-only": dirty the live (harp-probe) + pick choice 2 via the device
# --panel-sock, then assert the explicit read-only pick SUPPRESSES live writes (med-open-ro-noop).
# POSIX-only (MinGW device panel is a stub) + probe-gated; auto-enables when both land on a platform.
if [ "$OSID" = windows ]; then skip reconcile-readonly "MinGW device panel is a stub (§11.4 reconcile pick needs the panel transport)"
elif have "$PROBE"; then run reconcile-readonly scripts/reconcile-readonly-eth-test.sh
else skip reconcile-readonly "harp-probe not built on $OSID"; fi

# §4.4.3 mDNS _harp._tcp discovery round-trip: needs a LIVE mDNS responder + a dns_sd-built
# harp-probe. macOS CI has mDNSResponder always up; Linux/Windows runners don't reliably, so
# they SKIP — the path is bench-proven on the KR260 (avahi over the direct cable). The script
# also self-skips a no-dns_sd build.
if [ "$OSID" = macos ]; then run mdns-discover scripts/mdns-discover-test.sh
else skip mdns-discover "mDNS needs a live responder (macOS CI only; bench-proven on the KR260/avahi)"; fi

# §6.1 SHELL-side mDNS auto-discovery: the runtime's selectDevice browses _harp._tcp (not just
# harp-probe) — HARP_ETH_DEVICE=mdns. Same responder/dns_sd need -> macOS CI only; self-skips a
# no-dns_sd build.
if [ "$OSID" = macos ]; then run shell-mdns scripts/eth-shell-mdns-test.sh
else skip shell-mdns "mDNS needs a live responder (macOS CI only; bench-proven on the fleet)"; fi

# These mutate/inspect the device via harp-probe ("the musician"). harp-probe is not
# built on every platform yet (Windows: pending); the gate auto-enables them when it is.
if have "$PROBE"; then
  run recall           scripts/recall-eth-test.sh            # §11.4 recall round-trip + archive
  run archive-before-push scripts/archive-before-push-test.sh # §11.4 archive-BEFORE-push ordering: displaced state preserved before live overwrite
  run credit           scripts/credit-eth-test.sh            # §4.2.1b obj credit queue/flush under starvation
  run txn              scripts/txn-test.sh                   # §9.6 event transactions: buffer/commit-atomic/abort
  run epoch            scripts/epoch-test.sh                 # §7.1/§8.6 time.epoch on rate change + stale-epoch discard (cert T4)
  run engine-gate      scripts/engine-gate-eth-test.sh       # §13.4 device refuses foreign-engine snapshot; consent (0x4) overrides
  run cas-conflict     scripts/cas-conflict-eth-test.sh      # §11.3 CAS conflict: stale-expect AND dirty-ref both -> conflict ("ref is dirty")
  run core             scripts/core-test.sh                  # §5.5 core methods: ping/identify/changed/bye
  run conn-flood       scripts/conn-flood-test.sh            # §16 DoS: half-open drop + connect-storm survival
  run ratelimit        scripts/ratelimit-eth-test.sh         # §16(b) shed-on-reflood + no-shed-after-hello (--force-peer-ip)
  run bloat-recall     scripts/bloat-recall-eth-test.sh      # debt #22: live ref resolves on a many-archive store (recall-breaker)
  run gc               scripts/gc-test.sh                    # debt #22a: §10.3 archive retention + mark-sweep GC (wired device path)
  run offline-edit     scripts/offline-edit-eth-test.sh      # §15.5 edit-while-absent reaches device
  run diag-bundle      scripts/diag-bundle-eth-test.sh       # §14.4 device-side export + §16 anon
  run diag-counters    scripts/diag-counters-eth-test.sh     # §14.2 ALL-16 diag counters: exact map + types + clean-session bounds
else
  skip recall       "harp-probe not built on $OSID"
  skip credit       "harp-probe not built on $OSID"
  skip txn          "harp-probe not built on $OSID"
  skip epoch        "harp-probe not built on $OSID"
  skip cas-conflict "harp-probe not built on $OSID"
  skip core         "harp-probe not built on $OSID"
  skip conn-flood   "harp-probe not built on $OSID"
  skip ratelimit    "harp-probe not built on $OSID"
  skip bloat-recall "harp-probe not built on $OSID"
  skip gc           "harp-probe not built on $OSID"
  skip offline-edit "harp-probe not built on $OSID"
  skip diag-bundle  "harp-probe not built on $OSID"
  skip diag-counters "harp-probe not built on $OSID"
fi

MODE="$OSID"
eth_extern_active && MODE="$OSID, EXTERNAL $(eth_extern_ep)"
echo ""
echo "════════ eth-suite summary ($MODE) ════════$RESULTS"
echo "═══════════════════════════════════════════"
[ "$FAILED" = 0 ] && echo "eth-suite: ALL GREEN on $MODE" || echo "eth-suite: FAILURES on $MODE"

if [ "${DRY_RUN:-0}" = 1 ]; then exit 0; fi
exit "$FAILED"
