/* HARP core — framed link (spec §4.2).
 *
 *   u8   fver    = 0x01
 *   u8   stream
 *   u16  flags   little-endian; bit 0 = FIN, others reserved (MUST be 0)
 *   u32  length  little-endian; <= 65536
 *   u8[] payload
 */
#ifndef HARP_FRAME_H
#define HARP_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HARP_FRAME_FVER 0x01
#define HARP_FRAME_HDR_LEN 8
#define HARP_FRAME_MAX_PAYLOAD 65536u
#define HARP_FLAG_FIN 0x0001u

/* §4.2.1 per-stream reassembled-MESSAGE bounds. ctl is 64 KiB (small RPC envelopes —
 * the largest in practice is a ~3 KiB diag.bundle; bulk data rides OBJ); evt and log
 * are 4 KiB (single events / single log records). OBJ is flow-controlled by core.credit, but
 * ALSO carries a generous hard cap (16 MiB) so a peer that IGNORES its credit window cannot grow
 * the reassembly accumulator without bound (§16/§4.2.1 memory safety). Enforced in harp_link_recv
 * during reassembly: an over-cap message is malformed -> session reset (§12.4), and capping
 * mid-reassembly also bounds the accumulator against a peer that never sends FIN. */
#define HARP_CTL_MAX_PAYLOAD 65536u
#define HARP_EVT_MAX_PAYLOAD 4096u
#define HARP_LOG_MAX_PAYLOAD 4096u
#define HARP_OBJ_MAX_PAYLOAD (16u * 1024 * 1024) /* §16: finite safety cap; credit is the primary bound */

#define HARP_STREAM_CTL 0
#define HARP_STREAM_EVT 1
#define HARP_STREAM_OBJ 2
#define HARP_STREAM_LOG 3

typedef struct {
    uint8_t fver;
    uint8_t stream;
    uint16_t flags;
    uint32_t length;
} harp_frame_hdr;

void harp_frame_hdr_encode(const harp_frame_hdr *h, uint8_t out[HARP_FRAME_HDR_LEN]);
/* Returns false on malformed header (bad fver, oversize length, reserved flags):
 * fatal to the session per §4.2. */
bool harp_frame_hdr_decode(const uint8_t in[HARP_FRAME_HDR_LEN], harp_frame_hdr *h);

#endif
