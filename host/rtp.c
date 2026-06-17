/* See rtp.h. RFC 3550 base header, no padding/extension/CSRC on send; tolerant
 * of CSRC/extension on receive (HARP peers don't emit them, but parse safely). */
#include "rtp.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int harp_rtp_pack(uint8_t *buf, size_t cap, const harp_rtp_hdr *h,
                  const void *payload, size_t payload_len) {
    if (cap < HARP_RTP_HDR_BYTES + payload_len) return -1;
    buf[0] = 0x80;                                   /* V=2, P=0, X=0, CC=0      */
    buf[1] = (uint8_t)((h->marker ? 0x80 : 0) | (h->pt & 0x7F));
    put16(buf + 2, h->seq);
    put32(buf + 4, h->timestamp);
    put32(buf + 8, h->ssrc);
    if (payload_len) memcpy(buf + HARP_RTP_HDR_BYTES, payload, payload_len);
    return (int)(HARP_RTP_HDR_BYTES + payload_len);
}

int harp_rtp_unpack(const uint8_t *pkt, size_t len, harp_rtp_hdr *h,
                    const uint8_t **payload, size_t *payload_len) {
    if (len < HARP_RTP_HDR_BYTES) return -1;
    if ((pkt[0] >> 6) != 2) return -1;               /* version must be 2        */
    unsigned cc = pkt[0] & 0x0F;
    int ext = (pkt[0] >> 4) & 1;
    size_t off = HARP_RTP_HDR_BYTES + (size_t)cc * 4;
    if (len < off) return -1;
    if (ext) {                                        /* skip one extension hdr   */
        if (len < off + 4) return -1;
        size_t extlen = (size_t)get16(pkt + off + 2) * 4;
        off += 4 + extlen;
        if (len < off) return -1;
    }
    h->marker = (pkt[1] >> 7) & 1;
    h->pt = pkt[1] & 0x7F;
    h->seq = get16(pkt + 2);
    h->timestamp = get32(pkt + 4);
    h->ssrc = get32(pkt + 8);
    *payload = pkt + off;
    *payload_len = len - off;
    return 0;
}

uint64_t harp_rtp_unwrap_ts(uint32_t ts32, uint64_t prev64) {
    int32_t delta = (int32_t)(ts32 - (uint32_t)prev64);   /* signed: handles wrap + reorder */
    return prev64 + delta;
}
