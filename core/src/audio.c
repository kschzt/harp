#include "harp/audio.h"

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void harp_audio_hdr_encode(const harp_audio_hdr *h, uint8_t out[HARP_AUDIO_HDR_LEN]) {
    out[0] = h->fver;
    out[1] = h->dirflags;
    put16(out + 2, h->slots);
    put32(out + 4, h->epoch);
    put32(out + 8, (uint32_t)h->ts);
    put32(out + 12, (uint32_t)(h->ts >> 32));
    put16(out + 16, h->nsamples);
    put16(out + 18, h->fmt);
}

bool harp_audio_hdr_decode(const uint8_t in[HARP_AUDIO_HDR_LEN], harp_audio_hdr *h) {
    h->fver = in[0];
    h->dirflags = in[1];
    h->slots = get16(in + 2);
    h->epoch = get32(in + 4);
    h->ts = (uint64_t)get32(in + 8) | ((uint64_t)get32(in + 12) << 32);
    h->nsamples = get16(in + 16);
    h->fmt = get16(in + 18);
    if (h->fver != HARP_AUDIO_FVER) return false;
    if (h->fmt != HARP_AUDIO_FMT_F32) return false;
    if (h->slots == 0 || h->nsamples == 0) return false;
    return true;
}
