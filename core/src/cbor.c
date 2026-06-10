#include "harp/cbor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- buffer ---- */

static bool ensure(harp_cbuf *b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t *nb = realloc(b->buf, ncap);
    if (!nb) {
        b->oom = true;
        return false;
    }
    b->buf = nb;
    b->cap = ncap;
    return true;
}

void harp_cbuf_init(harp_cbuf *b) { memset(b, 0, sizeof *b); }

void harp_cbuf_free(harp_cbuf *b) {
    free(b->buf);
    memset(b, 0, sizeof *b);
}

void harp_cbuf_reset(harp_cbuf *b) {
    b->len = 0;
    b->oom = false;
}

void harp_cbuf_put(harp_cbuf *b, const void *p, size_t n) {
    if (!ensure(b, n)) return;
    memcpy(b->buf + b->len, p, n);
    b->len += n;
}

/* ---- encoder ---- */

static void put_head(harp_cbuf *b, uint8_t major, uint64_t v) {
    uint8_t h[9];
    size_t n;
    if (v < 24) {
        h[0] = (uint8_t)((major << 5) | v);
        n = 1;
    } else if (v <= 0xff) {
        h[0] = (uint8_t)((major << 5) | 24);
        h[1] = (uint8_t)v;
        n = 2;
    } else if (v <= 0xffff) {
        h[0] = (uint8_t)((major << 5) | 25);
        h[1] = (uint8_t)(v >> 8);
        h[2] = (uint8_t)v;
        n = 3;
    } else if (v <= 0xffffffffu) {
        h[0] = (uint8_t)((major << 5) | 26);
        h[1] = (uint8_t)(v >> 24);
        h[2] = (uint8_t)(v >> 16);
        h[3] = (uint8_t)(v >> 8);
        h[4] = (uint8_t)v;
        n = 5;
    } else {
        h[0] = (uint8_t)((major << 5) | 27);
        for (int i = 0; i < 8; i++) h[1 + i] = (uint8_t)(v >> (56 - 8 * i));
        n = 9;
    }
    harp_cbuf_put(b, h, n);
}

void harp_cbor_uint(harp_cbuf *b, uint64_t v) { put_head(b, 0, v); }

void harp_cbor_int(harp_cbuf *b, int64_t v) {
    if (v < 0)
        put_head(b, 1, (uint64_t)(-1 - v));
    else
        put_head(b, 0, (uint64_t)v);
}

void harp_cbor_bytes(harp_cbuf *b, const void *p, size_t n) {
    put_head(b, 2, n);
    harp_cbuf_put(b, p, n);
}

void harp_cbor_textn(harp_cbuf *b, const char *s, size_t n) {
    put_head(b, 3, n);
    harp_cbuf_put(b, s, n);
}

void harp_cbor_text(harp_cbuf *b, const char *s) { harp_cbor_textn(b, s, strlen(s)); }

void harp_cbor_array(harp_cbuf *b, uint64_t n) { put_head(b, 4, n); }
void harp_cbor_map(harp_cbuf *b, uint64_t n) { put_head(b, 5, n); }

void harp_cbor_bool(harp_cbuf *b, bool v) {
    uint8_t c = v ? 0xf5 : 0xf4;
    harp_cbuf_put(b, &c, 1);
}

void harp_cbor_null(harp_cbuf *b) {
    uint8_t c = 0xf6;
    harp_cbuf_put(b, &c, 1);
}

/* Exact half-precision conversion: true iff f is representable as f16. */
static bool half_from_float(float f, uint16_t *out) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint16_t sign = (uint16_t)((x >> 16) & 0x8000);
    if (isnan(f)) {
        *out = 0x7e00; /* canonical NaN, RFC 8949 §4.2.2 */
        return true;
    }
    if (isinf(f)) {
        *out = (uint16_t)(sign | 0x7c00);
        return true;
    }
    if ((x & 0x7fffffff) == 0) {
        *out = sign; /* ±0 */
        return true;
    }
    int unb = (int)((x >> 23) & 0xff) - 127;
    uint32_t man = x & 0x7fffff;
    if (unb >= -14 && unb <= 15) {
        if (man & 0x1fff) return false; /* needs more mantissa than f16 has */
        *out = (uint16_t)(sign | ((unb + 15) << 10) | (man >> 13));
        return true;
    }
    if (unb < -14) { /* maybe a half subnormal */
        int shift = 13 + (-14 - unb);
        uint64_t full = 0x800000u | man;
        if (shift >= 24) return false;
        if (full & ((1ull << shift) - 1)) return false;
        uint64_t hm = full >> shift;
        if (hm == 0 || hm > 0x3ff) return false;
        *out = (uint16_t)(sign | hm);
        return true;
    }
    return false;
}

void harp_cbor_float(harp_cbuf *b, double v) {
    float f = (float)v;
    if ((double)f == v || isnan(v)) {
        uint16_t h16;
        if (half_from_float(f, &h16)) {
            uint8_t out[3] = {0xf9, (uint8_t)(h16 >> 8), (uint8_t)h16};
            harp_cbuf_put(b, out, 3);
            return;
        }
        uint32_t fb;
        memcpy(&fb, &f, 4);
        uint8_t out[5] = {0xfa, (uint8_t)(fb >> 24), (uint8_t)(fb >> 16),
                          (uint8_t)(fb >> 8), (uint8_t)fb};
        harp_cbuf_put(b, out, 5);
        return;
    }
    uint64_t db;
    memcpy(&db, &v, 8);
    uint8_t out[9];
    out[0] = 0xfb;
    for (int i = 0; i < 8; i++) out[1 + i] = (uint8_t)(db >> (56 - 8 * i));
    harp_cbuf_put(b, out, 9);
}

/* ---- decoder ---- */

void harp_cdec_init(harp_cdec *d, const uint8_t *buf, size_t len) {
    d->p = buf;
    d->end = buf + len;
    d->err = false;
}

static bool need(harp_cdec *d, size_t n) {
    if (d->err || (size_t)(d->end - d->p) < n) {
        d->err = true;
        return false;
    }
    return true;
}

int harp_cdec_peek(const harp_cdec *d) {
    if (d->err || d->p >= d->end) return -1;
    return d->p[0] >> 5;
}

bool harp_cdec_peek_null(const harp_cdec *d) {
    return !d->err && d->p < d->end && d->p[0] == 0xf6;
}

/* Reads initial byte + argument. Rejects indefinite lengths and reserved
 * additional-info values. For major 7, *val is the raw argument bits. */
static bool read_head(harp_cdec *d, uint8_t *major, uint8_t *ai, uint64_t *val) {
    if (!need(d, 1)) return false;
    uint8_t ib = *d->p++;
    *major = ib >> 5;
    *ai = ib & 0x1f;
    if (*ai < 24) {
        *val = *ai;
        return true;
    }
    size_t n;
    switch (*ai) {
        case 24: n = 1; break;
        case 25: n = 2; break;
        case 26: n = 4; break;
        case 27: n = 8; break;
        default: d->err = true; return false; /* 28-30 reserved, 31 indefinite */
    }
    if (!need(d, n)) return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | *d->p++;
    *val = v;
    return true;
}

bool harp_cdec_uint(harp_cdec *d, uint64_t *v) {
    uint8_t major, ai;
    if (!read_head(d, &major, &ai, v)) return false;
    if (major != 0) {
        d->err = true;
        return false;
    }
    return true;
}

bool harp_cdec_int(harp_cdec *d, int64_t *v) {
    uint8_t major, ai;
    uint64_t u;
    if (!read_head(d, &major, &ai, &u)) return false;
    if (major == 0) {
        if (u > INT64_MAX) {
            d->err = true;
            return false;
        }
        *v = (int64_t)u;
        return true;
    }
    if (major == 1) {
        if (u > (uint64_t)INT64_MAX) {
            d->err = true;
            return false;
        }
        *v = -1 - (int64_t)u;
        return true;
    }
    d->err = true;
    return false;
}

static bool read_string(harp_cdec *d, uint8_t want_major, const uint8_t **p, size_t *n) {
    uint8_t major, ai;
    uint64_t len;
    if (!read_head(d, &major, &ai, &len)) return false;
    if (major != want_major || len > (size_t)(d->end - d->p)) {
        d->err = true;
        return false;
    }
    *p = d->p;
    *n = (size_t)len;
    d->p += len;
    return true;
}

bool harp_cdec_bytes(harp_cdec *d, const uint8_t **p, size_t *n) {
    return read_string(d, 2, p, n);
}

bool harp_cdec_text(harp_cdec *d, const char **s, size_t *n) {
    return read_string(d, 3, (const uint8_t **)s, n);
}

static bool read_count(harp_cdec *d, uint8_t want_major, uint64_t *n) {
    uint8_t major, ai;
    if (!read_head(d, &major, &ai, n)) return false;
    if (major != want_major) {
        d->err = true;
        return false;
    }
    /* Bound container counts by remaining bytes: every item costs >= 1 byte.
     * Prevents huge-count allocation attacks at callers (spec §16). */
    uint64_t cost = (want_major == 5) ? (*n) * 2 : *n;
    if (cost > (uint64_t)(d->end - d->p)) {
        d->err = true;
        return false;
    }
    return true;
}

bool harp_cdec_array(harp_cdec *d, uint64_t *n) { return read_count(d, 4, n); }
bool harp_cdec_map(harp_cdec *d, uint64_t *n) { return read_count(d, 5, n); }

bool harp_cdec_bool(harp_cdec *d, bool *v) {
    if (!need(d, 1)) return false;
    if (*d->p == 0xf4) {
        *v = false;
        d->p++;
        return true;
    }
    if (*d->p == 0xf5) {
        *v = true;
        d->p++;
        return true;
    }
    d->err = true;
    return false;
}

bool harp_cdec_null(harp_cdec *d) {
    if (!need(d, 1)) return false;
    if (*d->p != 0xf6) {
        d->err = true;
        return false;
    }
    d->p++;
    return true;
}

static double half_to_double(uint16_t h) {
    int sign = (h >> 15) & 1;
    int exp = (h >> 10) & 0x1f;
    int man = h & 0x3ff;
    double v;
    if (exp == 0)
        v = ldexp(man, -24);
    else if (exp == 31)
        v = man ? NAN : INFINITY;
    else
        v = ldexp(1024 + man, exp - 25);
    return sign ? -v : v;
}

bool harp_cdec_float(harp_cdec *d, double *v) {
    uint8_t major, ai;
    uint64_t u;
    if (!read_head(d, &major, &ai, &u)) return false;
    if (major != 7) {
        d->err = true;
        return false;
    }
    if (ai == 25) {
        *v = half_to_double((uint16_t)u);
        return true;
    }
    if (ai == 26) {
        uint32_t fb = (uint32_t)u;
        float f;
        memcpy(&f, &fb, 4);
        *v = f;
        return true;
    }
    if (ai == 27) {
        memcpy(v, &u, 8);
        return true;
    }
    d->err = true;
    return false;
}

#define HARP_CBOR_MAX_DEPTH 32

static bool skip_item(harp_cdec *d, int depth) {
    if (depth > HARP_CBOR_MAX_DEPTH) {
        d->err = true;
        return false;
    }
    uint8_t major, ai;
    uint64_t val;
    if (!read_head(d, &major, &ai, &val)) return false;
    switch (major) {
        case 0:
        case 1:
            return true;
        case 2:
        case 3:
            if (val > (size_t)(d->end - d->p)) {
                d->err = true;
                return false;
            }
            d->p += val;
            return true;
        case 4:
            for (uint64_t i = 0; i < val; i++)
                if (!skip_item(d, depth + 1)) return false;
            return true;
        case 5:
            for (uint64_t i = 0; i < val * 2; i++)
                if (!skip_item(d, depth + 1)) return false;
            return true;
        case 6: /* tag: skip the tagged item */
            return skip_item(d, depth + 1);
        case 7:
            return true; /* argument already consumed by read_head */
    }
    d->err = true;
    return false;
}

bool harp_cdec_skip(harp_cdec *d) { return skip_item(d, 0); }

bool harp_cdec_span(harp_cdec *d, const uint8_t **p, size_t *n) {
    const uint8_t *start = d->p;
    if (!harp_cdec_skip(d)) return false;
    *p = start;
    *n = (size_t)(d->p - start);
    return true;
}
