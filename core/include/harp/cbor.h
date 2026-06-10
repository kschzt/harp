/* HARP core — CBOR encoder/decoder (RFC 8949).
 *
 * The encoder always emits Core Deterministic Encoding (§4.2.1): shortest-form
 * integer arguments, definite lengths, preferred (shortest exact) float
 * serialization. Map key ordering is the caller's responsibility; helpers that
 * build spec structures (object.c) sort where the spec requires it.
 *
 * The decoder is bounds-checked and allocation-free (spec §16: device-side
 * parsers face hostile hosts). Indefinite lengths and tags are rejected:
 * nothing in HARP uses them.
 */
#ifndef HARP_CBOR_H
#define HARP_CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- growable output buffer ---- */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool oom;
} harp_cbuf;

void harp_cbuf_init(harp_cbuf *b);
void harp_cbuf_free(harp_cbuf *b);
void harp_cbuf_reset(harp_cbuf *b);
void harp_cbuf_put(harp_cbuf *b, const void *p, size_t n);

/* ---- encoder ---- */

void harp_cbor_uint(harp_cbuf *b, uint64_t v);
void harp_cbor_int(harp_cbuf *b, int64_t v);
void harp_cbor_bytes(harp_cbuf *b, const void *p, size_t n);
void harp_cbor_text(harp_cbuf *b, const char *s);
void harp_cbor_textn(harp_cbuf *b, const char *s, size_t n);
void harp_cbor_array(harp_cbuf *b, uint64_t n); /* header only; emit n items after */
void harp_cbor_map(harp_cbuf *b, uint64_t n);   /* header only; emit n pairs after */
void harp_cbor_bool(harp_cbuf *b, bool v);
void harp_cbor_null(harp_cbuf *b);
void harp_cbor_float(harp_cbuf *b, double v); /* shortest of f16/f32/f64 that is exact */

/* ---- decoder ---- */

typedef struct {
    const uint8_t *p, *end;
    bool err;
} harp_cdec;

void harp_cdec_init(harp_cdec *d, const uint8_t *buf, size_t len);

/* Major type of next item (0..7), or -1 on error/end. Does not consume. */
int harp_cdec_peek(const harp_cdec *d);
/* True if next item is null (0xf6). Does not consume. */
bool harp_cdec_peek_null(const harp_cdec *d);

bool harp_cdec_uint(harp_cdec *d, uint64_t *v);
bool harp_cdec_int(harp_cdec *d, int64_t *v);
bool harp_cdec_bytes(harp_cdec *d, const uint8_t **p, size_t *n);
bool harp_cdec_text(harp_cdec *d, const char **s, size_t *n); /* NOT nul-terminated */
bool harp_cdec_array(harp_cdec *d, uint64_t *n);
bool harp_cdec_map(harp_cdec *d, uint64_t *n);
bool harp_cdec_bool(harp_cdec *d, bool *v);
bool harp_cdec_null(harp_cdec *d);
bool harp_cdec_float(harp_cdec *d, double *v); /* accepts f16/f32/f64 */

/* Skip one complete item (recursively, depth-limited). */
bool harp_cdec_skip(harp_cdec *d);
/* Like skip, but returns the encoded span of the item. */
bool harp_cdec_span(harp_cdec *d, const uint8_t **p, size_t *n);

#endif
