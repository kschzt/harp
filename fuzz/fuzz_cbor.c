/* libFuzzer: the CBOR decoder (spec §16 / T9 — parsers MUST survive
 * arbitrary input). Walks the input as a sequence of items via skip
 * (which recurses through every container form), then re-walks it with
 * every typed accessor so each decode path sees hostile bytes. */
#include <stddef.h>
#include <stdint.h>

#include "harp/cbor.h"
#include "harp/store.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    harp_cdec d;

    harp_cdec_init(&d, data, size);
    for (int i = 0; i < 64 && !d.err && d.p < d.end; i++) harp_cdec_skip(&d);

    harp_cdec_init(&d, data, size);
    for (int i = 0; i < 64 && !d.err && d.p < d.end; i++) {
        uint64_t u;
        int64_t s;
        const uint8_t *bp;
        const char *tp;
        size_t n;
        bool bv;
        double f;
        /* each accessor validates-or-fails without advancing past garbage;
         * skip moves on when none match */
        if (harp_cdec_uint(&d, &u)) continue;
        if (harp_cdec_int(&d, &s)) continue;
        if (harp_cdec_bytes(&d, &bp, &n)) continue;
        if (harp_cdec_text(&d, &tp, &n)) continue;
        if (harp_cdec_array(&d, &u)) continue;
        if (harp_cdec_map(&d, &u)) continue;
        if (harp_cdec_bool(&d, &bv)) continue;
        if (harp_cdec_null(&d)) continue;
        if (harp_cdec_float(&d, &f)) continue;
        if (!harp_cdec_skip(&d)) break;
    }

    /* ref decode: a composite consumer of the primitives */
    harp_cdec_init(&d, data, size);
    harp_ref r;
    harp_ref_decode(&d, &r);
    return 0;
}
