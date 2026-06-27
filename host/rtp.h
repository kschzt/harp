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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HARP_RTP_HDR_BYTES 12

/* §8.7 wide-union multi-packet audio. A free-running RTP frame carrying more than
 * HARP_RTP_MAX_GROUP_SLOTS output slots would exceed the OS max UDP datagram (macOS
 * net.inet.udp.maxdgram = 9216 ≈ 8 slots × 256 samples × 4 B), so the device can't send
 * it. Such a frame is SPLIT into <=HARP_RTP_MAX_GROUP_SLOTS-slot groups, each sent as a
 * pt=HARP_RTP_PT_GROUP packet whose payload is a 4-byte sub-header
 * [slot_off:u8, n_slots:u8, total_slots:u8, flags:u8] then ns×n_slots interleaved floats.
 * All groups of one frame share the RTP timestamp; the host reassembles by timestamp. A
 * <=8-slot union stays ONE pt=HARP_RTP_PT_AUDIO packet (no sub-header) — byte-identical to
 * the pre-multipacket wire, so every existing 2-/4-slot test path is untouched. */
#define HARP_RTP_PT_AUDIO 96        /* single-packet frame (payload = raw slot-interleaved floats) */
#define HARP_RTP_PT_GROUP 97        /* multi-packet group (payload = 4-B sub-header + floats) */
#define HARP_RTP_MAX_GROUP_SLOTS 8  /* 8 slots × 256 samples × 4 B = 8192 < 9216 maxdgram */
#define HARP_RTP_GROUP_HDR_BYTES 4  /* [slot_off, n_slots, total_slots, flags] */

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

/* §8.7 RTP sequence/loss accounting (PURE, host-unit-tested in harp_engine_logic_tests.c).
 * gap = seq - last_seq - 1 (uint16, wraparound-safe). A FORWARD packet — gap < 0x8000, i.e.
 * in-order (gap 0) or a genuine skip (0 < gap < 0x8000) — reports `gap` lost packets and sets
 * *advance = true (the caller advances its high-water last_seq). A reordered or duplicate packet —
 * gap >= 0x8000, the seq went BACKWARD — reports 0 loss and sets *advance = false, so the caller
 * must NOT rewind last_seq; rewinding would make the NEXT in-order packet compute a huge spurious
 * loss. (§8.7 forbids CONCEALING loss, not over-reporting, so the prior unconditional-rewind bug
 * was bounded — it over-counted rather than hid loss.) */
static inline uint16_t harp_rtp_loss_gap(uint16_t last_seq, uint16_t seq, bool *advance) {
    uint16_t gap = (uint16_t)(seq - last_seq - 1);
    if (gap < 0x8000) { *advance = true; return gap; } /* forward: in-order or a real skip */
    *advance = false;                                  /* reorder/duplicate: keep last_seq */
    return 0;
}

#endif /* HARP_RTP_H */
