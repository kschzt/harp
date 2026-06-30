#include "harp/anonymize.h"

#include "harp/cbor.h"

/* See harp/anonymize.h for the §16 leaf contract. This is the ONE implementation both
 * host-side diag-bundle producers (harp-probe + the in-shell HarpRuntime) now call, so the
 * leaf list can never diverge between them. */
bool harp_anonymize_device_section(harp_cbuf *out, const uint8_t *sec, size_t len) {
    harp_cdec d;
    harp_cdec_init(&d, sec, len);
    uint64_t nsec;
    if (!harp_cdec_map(&d, &nsec)) return false;
    harp_cbor_map(out, nsec);
    for (uint64_t i = 0; i < nsec; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        harp_cbor_uint(out, key);
        if (key != 0) {
            /* counters (key 1) + any future device-section member: no PII, verbatim. */
            const uint8_t *span;
            size_t sl;
            if (!harp_cdec_span(&d, &span, &sl)) return false;
            harp_cbuf_put(out, span, sl);
            continue;
        }
        /* key 0 => identity: re-encode, clearing serial + vendor/product names. */
        uint64_t nid;
        if (!harp_cdec_map(&d, &nid)) return false;
        harp_cbor_map(out, nid);
        for (uint64_t j = 0; j < nid; j++) {
            uint64_t ik;
            if (!harp_cdec_uint(&d, &ik)) return false;
            harp_cbor_uint(out, ik);
            if (ik == 0 || ik == 1) {
                /* vendor (key 0) / product (key 1) sub-map { 0 => id, 1 => name };
                 * clear key 1 (name) to "", preserve key 0 (vid/pid) + any other verbatim. */
                uint64_t nsub;
                if (!harp_cdec_map(&d, &nsub)) return false;
                harp_cbor_map(out, nsub);
                for (uint64_t s = 0; s < nsub; s++) {
                    uint64_t sk;
                    if (!harp_cdec_uint(&d, &sk)) return false;
                    harp_cbor_uint(out, sk);
                    if (sk == 1) {
                        if (!harp_cdec_skip(&d)) return false; /* drop the name */
                        harp_cbor_text(out, "");               /* "" in place (§16) */
                    } else {
                        const uint8_t *span;
                        size_t sl;
                        if (!harp_cdec_span(&d, &span, &sl)) return false;
                        harp_cbuf_put(out, span, sl);
                    }
                }
            } else if (ik == 2 || ik == 9) {
                /* serial (key 2) and build-id (key 9, may embed host/date): cleared (§16). */
                if (!harp_cdec_skip(&d)) return false;
                harp_cbor_text(out, "");
            } else if (ik == 7) {
                /* channel-map (key 7): an array of entry maps. For EACH entry, clear keys
                 * 2 (name) / 3 (group) / 4 (path) to "" in place, preserving the slot index
                 * (key 0), direction (key 1), host-paced flag (key 5), and array length/order. */
                uint64_t nent;
                if (!harp_cdec_array(&d, &nent)) return false;
                harp_cbor_array(out, nent);
                for (uint64_t en = 0; en < nent; en++) {
                    uint64_t nek;
                    if (!harp_cdec_map(&d, &nek)) return false;
                    harp_cbor_map(out, nek);
                    for (uint64_t ek = 0; ek < nek; ek++) {
                        uint64_t ekey;
                        if (!harp_cdec_uint(&d, &ekey)) return false;
                        harp_cbor_uint(out, ekey);
                        if (ekey == 2 || ekey == 3 || ekey == 4) {
                            if (!harp_cdec_skip(&d)) return false; /* drop name/group/path */
                            harp_cbor_text(out, "");               /* "" in place (§16) */
                        } else {
                            const uint8_t *span;
                            size_t sl;
                            if (!harp_cdec_span(&d, &span, &sl)) return false;
                            harp_cbuf_put(out, span, sl);
                        }
                    }
                }
            } else {
                /* fw/engine/protocol/latency-profile/boot/ump-map/part-count: byte-for-meaning. */
                const uint8_t *span;
                size_t sl;
                if (!harp_cdec_span(&d, &span, &sl)) return false;
                harp_cbuf_put(out, span, sl);
            }
        }
    }
    return !d.err;
}
