# eth-extern-lib.sh — EXTERNAL-ENDPOINT MODE for the §8.7 Ethernet/IP conformance suite.
#
# Sourced (never executed) by scripts/eth-suite.sh + the external-capable sub-scripts.
#
# HARP_ETH_EXTERN=host:port switches the eth harness from its DEFAULT "spawn a localhost
# harp-deviced and talk to it over loopback (same clock domain, no NIC)" to "target an
# ALREADY-RUNNING external harp-deviced at that address over a REAL two-host network hop" —
# closing the loopback-only gap the maturity capstone flagged. When set, an external-capable
# sub-script does NOT spawn/kill a local deviced; it dials $HARP_ETH_EXTERN. Unset/empty =>
# the default spawn-local loopback path, 100% unchanged (eth.yml is unaffected).
#
# HARP_ETH_EXTERN_SERIAL (optional) = the external device's serial; exported as
# HARP_DEVICE_SERIAL so the host's connect-check pins the right board (the localhost stand-in
# defaults to SIM-0001; the rig Pi is e.g. PI4B-0002). Unset => the host takes whatever serial
# the daemon reports (no pin).
#
# Capability gating: the SUITE decides run-vs-skip per test (an allowlist of external-capable
# tests; everything else is skipped WITH A LOGGED REASON, exactly like the per-OS / probe
# skips — never a silent omission). Each external-capable sub-script then guards its own
# device-spawn with eth_extern_active so it targets the external endpoint instead.

# true iff external-endpoint mode is requested.
eth_extern_active() { [ -n "${HARP_ETH_EXTERN:-}" ]; }

# the external endpoint (host:port) to dial.
eth_extern_ep() { printf '%s' "${HARP_ETH_EXTERN:-}"; }

# host part of the external endpoint (strip :port).
eth_extern_host() { local ep; ep="$(eth_extern_ep)"; printf '%s' "${ep%:*}"; }

# port part of the external endpoint (strip host:).
eth_extern_port() { local ep; ep="$(eth_extern_ep)"; printf '%s' "${ep##*:}"; }

# Pin the host's connect-check serial when the caller supplied one (HARP_ETH_EXTERN_SERIAL).
# No-op otherwise, so the loopback stand-in (default SIM-0001) still connects.
eth_extern_export_serial() {
    if [ -n "${HARP_ETH_EXTERN_SERIAL:-}" ]; then export HARP_DEVICE_SERIAL="$HARP_ETH_EXTERN_SERIAL"; fi
}

# One-line banner every external-capable branch prints, so the run log makes it UNAMBIGUOUS
# that this test targeted the EXTERNAL endpoint and did NOT auto-spawn a local deviced.
eth_extern_banner() {  # eth_extern_banner <test-name>
    echo "── [$1] EXTERNAL-ENDPOINT MODE: targeting already-running harp-deviced at $(eth_extern_ep) — NOT spawning a local one"
}
