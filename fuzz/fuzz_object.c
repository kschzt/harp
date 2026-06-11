/* libFuzzer: content-addressed object parsers (§10.2) — kind sniff, blob
 * payload extraction, snapshot root, tree iteration, and the closure
 * child walk both with and without parents. These run on every object
 * received from a peer, so they are the §16/T9 front line. */
#include <stddef.h>
#include <stdint.h>

#include "harp/object.h"

static bool tree_cb(const char *name, size_t name_len, const harp_hash *h, uint32_t kind,
                    void *ud) {
    (void)name;
    (void)name_len;
    (void)h;
    (void)kind;
    int *count = ud;
    return ++*count < 4096; /* bound traversal on hostile fan-out */
}

static bool child_cb(const harp_hash *h, void *ud) {
    (void)h;
    int *count = ud;
    return ++*count < 4096;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    harp_obj_kind(data, size);

    const char *media;
    size_t media_len;
    const uint8_t *payload;
    size_t payload_len;
    harp_obj_parse_blob(data, size, &media, &media_len, &payload, &payload_len);

    harp_hash root;
    harp_obj_parse_snapshot_root(data, size, &root);

    int count = 0;
    harp_obj_tree_foreach(data, size, tree_cb, &count);
    count = 0;
    harp_obj_foreach_child(data, size, false, child_cb, &count);
    count = 0;
    harp_obj_foreach_child(data, size, true, child_cb, &count);
    return 0;
}
