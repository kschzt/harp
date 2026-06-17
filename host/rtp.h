/* Minimal RTP (RFC 3550) packetization for the HARP §8.7 network audio plane.
 *
 * Carries interleaved PCM with the per-packet sequence number (loss/reorder
 * detection — the field the USB §8.2 frame lacked) and the device-clock sample
 * index as the RTP timestamp, which the host regresses against arrival time for
 * sub-frame-precise clock recovery (freerun.h). HARP controls both ends, so the
 * header is the base form only: version 2, no padding/extension/CSRC. The
 * payload type, SSRC, and sample format are negotiated out of band (audio.start
 * / the framed link); this module is just the wire codec.
 */
#ifndef HARP_RTP_H
#define HARP_RTP_H

#include <stddef.h>
#include <stdint.h>

#define HARP_RTP_HDR_BYTES 12

typedef struct {
    uint8_t  pt;          /* payload type (7 bits)        */
    uint8_t  marker;      /* marker bit                   */
    uint16_t seq;         /* sequence number              */
    uint32_t timestamp;   /* device sample index (wraps)  */
    uint32_t ssrc;        /* synchronization source       */
} harp_rtp_hdr;

/* Write a 12-byte RTP header + payload into buf. Returns total bytes written,
 * or -1 if buf is too small. */
int harp_rtp_pack(uint8_t *buf, size_t cap, const harp_rtp_hdr *h,
                  const void *payload, size_t payload_len);

/* Parse a received packet. On success returns 0, fills *h, and points *payload
 * into pkt (not copied) with *payload_len bytes. Returns -1 on a malformed or
 * truncated packet (bad version, short header, CSRC/extension past the end). */
int harp_rtp_unpack(const uint8_t *pkt, size_t len, harp_rtp_hdr *h,
                    const uint8_t **payload, size_t *payload_len);

/* Extend a 32-bit RTP timestamp to a monotonic 64-bit sample index, given the
 * previously extended value. Handles wraparound (the stream runs for hours; the
 * 32-bit field wraps every ~25 h at 48 kHz but recovery must stay continuous).
 * Pass prev64 = 0 and prev32 unused on the first call. */
uint64_t harp_rtp_unwrap_ts(uint32_t ts32, uint64_t prev64);

#endif /* HARP_RTP_H */
