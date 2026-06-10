/* HARP core — file-backed object store and refs (spec §10.3, §11).
 *
 * Layout under the store directory:
 *   objects/<66-hex>      one file per object, written tmp+rename (atomic)
 *   refs/<name>           CBOR ref record, written tmp+rename (journaled CAS:
 *                         a crash leaves the old or the new ref, never a hybrid)
 */
#ifndef HARP_STORE_H
#define HARP_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "harp/cbor.h"
#include "harp/object.h"

#define HARP_REF_NAME_MAX 160

typedef struct {
    char dir[512];
} harp_store;

typedef struct {
    char name[HARP_REF_NAME_MAX];
    bool unborn; /* target is null */
    harp_hash hash;
    uint64_t generation;
    bool dirty;
} harp_ref;

/* 0 on success; creates dir, objects/, refs/. */
int harp_store_open(harp_store *s, const char *dir);

bool harp_store_have(const harp_store *s, const harp_hash *h);
/* Verifies content-addressing (computes the hash) and writes atomically.
 * 0 ok, -1 io error. *out receives the hash if non-NULL. */
int harp_store_put(harp_store *s, const uint8_t *enc, size_t len, harp_hash *out);
/* 0 ok, -1 not found / io. Appends the encoding to out. */
int harp_store_get(const harp_store *s, const harp_hash *h, harp_cbuf *out);

/* Missing ref reads as unborn with generation 0, dirty false. 0 ok, -1 bad name. */
int harp_store_ref_read(const harp_store *s, const char *name, harp_ref *r);
/* Atomic (tmp+rename). 0 ok, -1 error. */
int harp_store_ref_write(harp_store *s, const harp_ref *r);

typedef void (*harp_ref_cb)(const harp_ref *r, void *ud);
int harp_store_ref_list(const harp_store *s, harp_ref_cb cb, void *ud);

/* CBOR codec for the ref record itself (used on the wire too). */
void harp_ref_encode(harp_cbuf *out, const harp_ref *r);
bool harp_ref_decode(harp_cdec *d, harp_ref *r);

#endif
