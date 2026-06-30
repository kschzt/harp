#ifndef HARP_ANONYMIZE_H
#define HARP_ANONYMIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "harp/cbor.h" /* harp_cbuf */

/* §16 / §14.4 device-section anonymizer — the SINGLE source of truth for the §16 PII leaf
 * list, shared by the two host-side diag-bundle producers (the out-of-process harp-probe
 * and the in-shell HarpRuntime) so they can never diverge (a one-sided leaf is a PII leak).
 *
 * DECODE the device-section map { 0 => identity, 1 => counters } and RE-ENCODE it into
 * `out`, clearing the identity PII leaves to "" IN PLACE while preserving everything else
 * byte-for-meaning. Subtrees needing no edit are copied via harp_cdec_span (byte-for-byte).
 *
 * Cleared (§16): identity key 2 (serial); identity key 9 (build-id, may embed host/date);
 * vendor (key 0) / product (key 1) sub-map key 1 (name); channel-map (key 7) per-entry keys
 * 2/3/4 (name/group/path). Preserved verbatim: vid/pid, firmware, engine (incl. engine-id +
 * param-map-hash), protocol, latency-profile, boot count, ump-group-map, part count, caps,
 * per-entry slot/direction/host-paced flag, the array length/order, and the whole counters
 * map (no PII). Returns false on a malformed section so the caller can fall back. */
bool harp_anonymize_device_section(harp_cbuf *out, const uint8_t *sec, size_t len);

#endif /* HARP_ANONYMIZE_H */
