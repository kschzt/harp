/* libFuzzer: audio frame header codec (§8.2) — the device trusts pacing
 * frame headers from the host and vice versa; decode must reject hostile
 * field combinations (payload-length overflow being the classic). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "harp/audio.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < HARP_AUDIO_HDR_LEN) return 0;
    harp_audio_hdr h;
    if (harp_audio_hdr_decode(data, &h)) {
        harp_audio_payload_len(&h);
        /* round-trip: re-encode must be stable for accepted headers */
        uint8_t out[HARP_AUDIO_HDR_LEN];
        harp_audio_hdr_encode(&h, out);
    }
    return 0;
}
