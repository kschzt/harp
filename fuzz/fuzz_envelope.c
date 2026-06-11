/* libFuzzer: control-plane envelope parse (§5.2) plus a walk of the body
 * the way dispatch handlers do — map, uint keys, mixed-type values. */
#include <stddef.h>
#include <stdint.h>

#include "harp/cbor.h"
#include "harp/envelope.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    harp_env e;
    if (!harp_env_parse(data, size, &e)) return 0;
    if (!e.has_body) return 0;
    /* the canonical handler body walk: map of uint keys, skip values */
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n;
    if (!harp_cdec_map(&b, &n)) return 0;
    for (uint64_t i = 0; i < n && i < 64; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&b, &key)) break;
        if (!harp_cdec_skip(&b)) break;
    }
    return 0;
}
