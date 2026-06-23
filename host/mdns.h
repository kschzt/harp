/* host/mdns.h — shared mDNS/DNS-SD discovery of `_harp._tcp` devices (§4.4.3).
 *
 * Browse-and-resolve, used by BOTH harp-probe (the `discover` command) and the shell
 * runtime (selectDevice auto-discovery, §6.1). Built where dns_sd is available — native
 * on macOS (libSystem), avahi-compat's libdns_sd on Linux; elsewhere (Windows, a bare
 * runner) harp_mdns_discover() returns -1 so callers degrade cleanly (the shell falls
 * back to USB / an explicit HARP_ETH_DEVICE). The serial is NOT in the TXT (§16); the
 * resolved host:port is dialled and identity comes from core.hello. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     host[256];     /* resolved hostname or IP — the dial target with :port */
    uint16_t port;          /* framed-link TCP port, host byte order */
    char     proto[32];     /* TXT "proto" (= bcdHARP major.minor), or "?" if absent */
    bool     serial_leaked; /* §16: a "serial" key present in the TXT (a privacy violation) */
} harp_mdns_instance;

/* Browse `_harp._tcp` for up to timeout_ms, resolving each advertised instance to
 * host:port + TXT. Fills out[] with up to `max` resolved instances (deduplicated by
 * host:port), and returns the count found — or -1 where dns_sd is unavailable on this
 * build. Stops early once `max` instances are resolved. BLOCKING for up to the budget;
 * call only from a non-realtime thread (the shell calls it on the supervisor thread). */
int harp_mdns_discover(int timeout_ms, harp_mdns_instance *out, size_t max);

#ifdef __cplusplus
}
#endif
