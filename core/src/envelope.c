#include "harp/envelope.h"

#include <string.h>

bool harp_env_parse(const uint8_t *buf, size_t len, harp_env *e) {
    memset(e, 0, sizeof *e);
    harp_cdec d;
    harp_cdec_init(&d, buf, len);
    uint64_t n;
    if (!harp_cdec_map(&d, &n)) return false;
    bool have_type = false, have_rid = false, have_method = false;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        switch (key) {
            case 0: {
                uint64_t t;
                if (!harp_cdec_uint(&d, &t) || t > 3) return false;
                e->msgtype = (uint32_t)t;
                have_type = true;
                break;
            }
            case 1:
                if (!harp_cdec_uint(&d, &e->rid)) return false;
                have_rid = true;
                break;
            case 2: {
                const char *s;
                size_t sl;
                if (!harp_cdec_text(&d, &s, &sl) || sl >= HARP_METHOD_MAX) return false;
                memcpy(e->method, s, sl);
                e->method[sl] = 0;
                have_method = true;
                break;
            }
            case 3:
                if (!harp_cdec_span(&d, &e->body, &e->body_len)) return false;
                e->has_body = true;
                break;
            default:
                if (!harp_cdec_skip(&d)) return false;
        }
    }
    return have_type && have_rid && have_method;
}

void harp_env_head(harp_cbuf *out, uint32_t msgtype, uint64_t rid, const char *method,
                   bool has_body) {
    harp_cbor_map(out, has_body ? 4 : 3);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, msgtype);
    harp_cbor_uint(out, 1);
    harp_cbor_uint(out, rid);
    harp_cbor_uint(out, 2);
    harp_cbor_text(out, method);
    if (has_body) harp_cbor_uint(out, 3);
}

void harp_env_error(harp_cbuf *out, uint64_t rid, const char *method, const char *code,
                    const char *message) {
    harp_env_head(out, HARP_MSG_ERROR, rid, method, true);
    harp_cbor_map(out, message ? 2 : 1);
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, code);
    if (message) {
        harp_cbor_uint(out, 1);
        harp_cbor_text(out, message);
    }
}
