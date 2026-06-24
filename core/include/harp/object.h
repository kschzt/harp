/* HARP core — content-addressed state objects (spec §10).
 *
 * hash = 1 algorithm byte (0x01 = SHA-256) + 32-byte digest, over the object's
 * RFC 8949 §4.2.1 deterministic encoding.
 */
#ifndef HARP_OBJECT_H
#define HARP_OBJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "harp/cbor.h"

#define HARP_HASH_LEN 33
#define HARP_HASH_ALG_SHA256 0x01

typedef struct {
    uint8_t b[HARP_HASH_LEN];
} harp_hash;

bool harp_hash_eq(const harp_hash *a, const harp_hash *b);
void harp_hash_hex(const harp_hash *h, char out[2 * HARP_HASH_LEN + 1]);
bool harp_hash_from_hex(const char *hex, harp_hash *out);
harp_hash harp_hash_compute(const void *data, size_t len);

/* §10.2: read a 33-byte hash from a CBOR byte string, rejecting any unknown
 * algorithm byte (only 0x01 = SHA-256 is defined). Returns false on a wrong
 * length OR an unrecognized algorithm. Every wire parse site goes through this
 * so none can silently accept a hash it cannot verify. */
bool harp_hash_read(harp_cdec *d, harp_hash *out);

enum {
    HARP_OBJ_BLOB = 0,
    HARP_OBJ_LIST = 1,
    HARP_OBJ_TREE = 2,
    HARP_OBJ_SNAPSHOT = 3,
};

/* ---- encoders (deterministic; safe to hash the output) ---- */

void harp_obj_encode_blob(harp_cbuf *out, const char *media_type,
                          const void *payload, size_t payload_len);

void harp_obj_encode_list(harp_cbuf *out, const char *media_type,
                          const harp_hash *chunks, size_t nchunks);

typedef struct {
    const char *name;
    harp_hash hash;
    uint32_t kind; /* kind of the referenced object */
} harp_tree_entry;

/* Entries may be given in any order; encoding sorts them per deterministic
 * map-key ordering. n <= 256 in this implementation. */
void harp_obj_encode_tree(harp_cbuf *out, const harp_tree_entry *entries, size_t n);

void harp_obj_encode_snapshot(harp_cbuf *out, const harp_hash *root_tree,
                              const harp_hash *parents, size_t nparents,
                              uint64_t unix_time, const char *author,
                              const char *engine_semver, const char *message /* nullable */);

/* ---- parsers ---- */

/* Object kind from encoding, or -1 if malformed. */
int harp_obj_kind(const uint8_t *enc, size_t len);

bool harp_obj_parse_blob(const uint8_t *enc, size_t len, const char **media,
                         size_t *media_len, const uint8_t **payload, size_t *payload_len);

/* Extracts the root tree hash of a snapshot. */
bool harp_obj_parse_snapshot_root(const uint8_t *enc, size_t len, harp_hash *root);
/* §13.4: extract a snapshot's engine semver (key 5) — the device's state.refset load gate
 * compares its MAJOR to the device's engine major. False if not a snapshot or no engine field. */
bool harp_obj_parse_snapshot_engine(const uint8_t *enc, size_t len, char *out, size_t outsz);

typedef bool (*harp_tree_cb)(const char *name, size_t name_len, const harp_hash *h,
                             uint32_t kind, void *ud); /* return false to stop */
bool harp_obj_tree_foreach(const uint8_t *enc, size_t len, harp_tree_cb cb, void *ud);

/* All hashes directly referenced by an object (tree entries, list chunks,
 * snapshot root — parents excluded by default, see harp_obj_refs_opts). */
typedef bool (*harp_hash_cb)(const harp_hash *h, void *ud);
bool harp_obj_foreach_child(const uint8_t *enc, size_t len, bool include_parents,
                            harp_hash_cb cb, void *ud);

#endif
