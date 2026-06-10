/* HARP core — audio stream frames (spec §8.2).
 *
 *   u8   fver        = 0x01
 *   u8   dirflags    bit 0: direction (0 D→H, 1 H→D); bit 1: discontinuity
 *   u16  slots       channel count in this frame
 *   u32  epoch
 *   u64  ts          stream timestamp of first sample (device MSC free-running,
 *                    host SSI host-paced)
 *   u16  nsamples    samples per channel
 *   u16  fmt         0x0001 = float32 LE
 *   payload          interleaved by slot index, nsamples × slots
 *
 * All fields little-endian.
 */
#ifndef HARP_AUDIO_H
#define HARP_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HARP_AUDIO_HDR_LEN 20
#define HARP_AUDIO_FVER 0x01
#define HARP_AUDIO_DIR_H2D 0x01
#define HARP_AUDIO_DISCONT 0x02
#define HARP_AUDIO_FMT_F32 0x0001

typedef struct {
    uint8_t fver;
    uint8_t dirflags;
    uint16_t slots;
    uint32_t epoch;
    uint64_t ts;
    uint16_t nsamples;
    uint16_t fmt;
} harp_audio_hdr;

void harp_audio_hdr_encode(const harp_audio_hdr *h, uint8_t out[HARP_AUDIO_HDR_LEN]);
/* false on malformed header (bad fver / unknown fmt / zero nsamples).
 * slots == 0 is valid: a host-paced pacing frame with no input channels. */
bool harp_audio_hdr_decode(const uint8_t in[HARP_AUDIO_HDR_LEN], harp_audio_hdr *h);

/* payload byte count for a decoded header (float32) */
static inline size_t harp_audio_payload_len(const harp_audio_hdr *h) {
    return (size_t)h->slots * h->nsamples * 4;
}

#endif
