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
/* Remove refs/<name>. 0 ok (including already-absent: idempotent), -1 bad name. */
int harp_store_ref_delete(harp_store *s, const char *name);

/* CBOR codec for the ref record itself (used on the wire too). */
void harp_ref_encode(harp_cbuf *out, const harp_ref *r);
bool harp_ref_decode(harp_cdec *d, harp_ref *r);

/* ---- object iteration + garbage collection (§10.3: a device MAY reclaim unreachable
 * objects at will; hosts re-send on demand). ---- */
typedef bool (*harp_obj_cb)(const harp_hash *h, void *ud); /* return false to stop early */
/* Iterate every stored object hash (objects/<66-hex>; skips dotfiles, .tmp.* temporaries
 * and non-hex names). 0 ok, -1 if objects/ can't be opened. */
int harp_store_obj_foreach(const harp_store *s, harp_obj_cb cb, void *ud);
/* Number of stored objects (one directory pass; used to pre-size the GC mark set). */
size_t harp_store_obj_count(const harp_store *s);
/* Remove one object. 0 ok (incl. absent), -1 io. Caller MUST guarantee unreachability. */
int harp_store_obj_delete(harp_store *s, const harp_hash *h);

/* Mark-sweep GC: marks every object reachable from the closure of EVERY ref currently on
 * disk (re-listed live, include_parents=false), then deletes the unmarked objects. To prune,
 * delete the unwanted refs FIRST (harp_store_ref_delete) — GC only reclaims what no surviving
 * ref reaches. FAIL-CLOSED: if any reachable object can't be read or fully walked, or the mark
 * set can't be allocated, it sweeps NOTHING and returns -1 (never deletes against a partial
 * mark). At most `max_sweep` objects are deleted per call (0 = unbounded); the rest drain on
 * the next call. *reclaimed (may be NULL) receives the number deleted. */
int harp_store_gc(harp_store *s, size_t max_sweep, size_t *reclaimed);
/* Test seam (mirrors harp_store_fault_skip_rename): stop the sweep after the first delete,
 * to prove GC is crash-resumable + idempotent. Default false; ONLY a test sets it. */
extern bool harp_store_fault_skip_sweep;

#endif
