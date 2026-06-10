#include "harp/frame.h"

void harp_frame_hdr_encode(const harp_frame_hdr *h, uint8_t out[HARP_FRAME_HDR_LEN]) {
    out[0] = h->fver;
    out[1] = h->stream;
    out[2] = (uint8_t)(h->flags & 0xff);
    out[3] = (uint8_t)(h->flags >> 8);
    out[4] = (uint8_t)(h->length & 0xff);
    out[5] = (uint8_t)(h->length >> 8);
    out[6] = (uint8_t)(h->length >> 16);
    out[7] = (uint8_t)(h->length >> 24);
}

bool harp_frame_hdr_decode(const uint8_t in[HARP_FRAME_HDR_LEN], harp_frame_hdr *h) {
    h->fver = in[0];
    h->stream = in[1];
    h->flags = (uint16_t)(in[2] | (in[3] << 8));
    h->length = (uint32_t)in[4] | ((uint32_t)in[5] << 8) | ((uint32_t)in[6] << 16) |
                ((uint32_t)in[7] << 24);
    if (h->fver != HARP_FRAME_FVER) return false;
    if (h->flags & ~HARP_FLAG_FIN) return false; /* reserved flags MUST be 0 */
    if (h->length > HARP_FRAME_MAX_PAYLOAD) return false;
    return true;
}
