#!/bin/bash
# eth-shell-mdns-test — §6.1/§4.4.3 SHELL auto-discovery. selectDevice browses `_harp._tcp`
# and dials the first synth it resolves: automatically when no literal HARP_ETH_DEVICE and no
# USB device are present, or explicitly via HARP_ETH_DEVICE=mdns (what this test sets, so it is
# deterministic regardless of any USB device on the runner). Proves the runtime (not just
# harp-probe) folds network devices into its device list. Needs a live mDNS responder AND the
# shell built with dns_sd, so eth-suite runs
# this on macOS (mDNSResponder always up) and SKIPS elsewhere; self-skips if dns_sd is absent.
#
# On a shared segment the shell may resolve ANY advertised _harp._tcp synth first (that is the
# feature); on an isolated CI runner only this test's device is present, so it is deterministic.
set -u
cd "$(dirname "$0")/.."
DEVICED="${DEVICED:-./build/harp-deviced}"
HOSTBIN="${HOSTBIN:-./build-vst/harp-vst3-host}"
PROBE="${PROBE:-./build/harp-probe}"
PLUG="${PLUG:-$(find build-vst build -maxdepth 5 -name harp-shell.vst3 -type d 2>/dev/null | head -1)}"
PORT="${PORT:-17812}"
SERIAL=SIM-SHELL-MDNS
fail() { echo "SHELL-MDNS FAIL: $1"; exit 1; }
[ -x "$DEVICED" ] || fail "$DEVICED not built"
[ -x "$HOSTBIN" ] || fail "$HOSTBIN not built"
[ -n "$PLUG" ] && [ -d "$PLUG" ] || fail "harp-shell.vst3 not found"

# capability gate: the shell shares host/mdns with harp-probe, so probe's stub message tells
# us whether this build has dns_sd at all.
if [ -x "$PROBE" ]; then
  "$PROBE" discover >/tmp/shell-mdns-cap.log 2>&1
  grep -q "without dns_sd" /tmp/shell-mdns-cap.log 2>/dev/null && { echo "SHELL-MDNS SKIP: no dns_sd on this host"; exit 0; }
fi

rm -rf shell-mdns-state; : > /tmp/shell-mdns-dev.log
"$DEVICED" --port "$PORT" --mdns --serial "$SERIAL" --tone 440 --state-dir shell-mdns-state --panel-sock "" >/tmp/shell-mdns-dev.log 2>&1 & DP=$!
trap 'kill "$DP" 2>/dev/null; rm -rf shell-mdns-state' EXIT INT TERM
for _ in $(seq 1 25); do grep -q "listening on $PORT" /tmp/shell-mdns-dev.log 2>/dev/null && break; sleep 0.2; done
grep -q "listening on $PORT" /tmp/shell-mdns-dev.log || { cat /tmp/shell-mdns-dev.log; fail "device didn't start on $PORT"; }
sleep 2  # let the responder register the advertisement

echo "── shell with HARP_ETH_DEVICE=mdns (explicit browse, skips USB): must discover + dial + play"
HARP_ETH_DEVICE=mdns perl -e 'alarm 25; exec @ARGV' "$HOSTBIN" "$PLUG" --realtime --seconds 8 >/tmp/shell-mdns-host.log 2>&1
grep -iE "mDNS|discovered|underruns|rms=|connect" /tmp/shell-mdns-host.log | head

grep -qi "mDNS: discovered network device" /tmp/shell-mdns-host.log \
  || { cat /tmp/shell-mdns-host.log; fail "shell did not auto-discover a _harp._tcp device"; }
grep -qiE "underruns:" /tmp/shell-mdns-host.log \
  || { cat /tmp/shell-mdns-host.log; fail "shell discovered but never ran a session"; }
RMS=$(grep -oE "rms=[0-9.]+" /tmp/shell-mdns-host.log | tail -1 | cut -d= -f2)
awk "BEGIN{exit !(${RMS:-0} > 0)}" \
  || { cat /tmp/shell-mdns-host.log; fail "discovered + connected but produced NO audio (rms=${RMS:-n/a})"; }
# NOTE: the §12.3 eth-reconnect path (selectDevice re-dialing a dropped network synth via the
# boundEthHostport_ pin) runs in production but needs a no-USB host to test in isolation — a USB
# device would bind first — so it is exercised here only indirectly; tracked for the coverage audit.
echo "SHELL-MDNS PASS (discovered + dialed a _harp._tcp synth via HARP_ETH_DEVICE=mdns; audio rms=$RMS)"
