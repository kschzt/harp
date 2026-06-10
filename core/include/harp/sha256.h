/* HARP core — SHA-256 (FIPS 180-4). */
#ifndef HARP_SHA256_H
#define HARP_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t block[64];
    size_t blocklen;
} harp_sha256;

void harp_sha256_init(harp_sha256 *c);
void harp_sha256_update(harp_sha256 *c, const void *data, size_t len);
void harp_sha256_final(harp_sha256 *c, uint8_t out[32]);
void harp_sha256_digest(const void *data, size_t len, uint8_t out[32]);

#endif
