/* HARP core — control-plane envelope (spec §5.2). */
#ifndef HARP_ENVELOPE_H
#define HARP_ENVELOPE_H

#include <stdbool.h>
#include <stdint.h>

#include "harp/cbor.h"

enum {
    HARP_MSG_REQUEST = 0,
    HARP_MSG_RESPONSE = 1,
    HARP_MSG_ERROR = 2,
    HARP_MSG_NOTIFICATION = 3,
};

#define HARP_METHOD_MAX 64

typedef struct {
    uint32_t msgtype;
    uint64_t rid;
    char method[HARP_METHOD_MAX];
    bool has_body;
    const uint8_t *body; /* encoded CBOR item, points into the parsed buffer */
    size_t body_len;
} harp_env;

bool harp_env_parse(const uint8_t *buf, size_t len, harp_env *e);

/* Writes the envelope map header and keys 0..2 (and key 3 if has_body);
 * caller appends exactly one CBOR item as the body when has_body. */
void harp_env_head(harp_cbuf *out, uint32_t msgtype, uint64_t rid,
                   const char *method, bool has_body);

/* Convenience: full error message (spec §5.3). details may be NULL. */
void harp_env_error(harp_cbuf *out, uint64_t rid, const char *method,
                    const char *code, const char *message /* nullable */);

#endif
