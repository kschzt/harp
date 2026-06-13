/* Shared determinism helpers for the CLI test hosts (vst3-host, au-host).
 *
 * harp_fnv1a is the load-bearing one: cross-format "byte-identical"
 * (the AU shell hashing the same as the VST3 shell) is only meaningful
 * if BOTH hosts hash with the exact same function. Duplicating it invited
 * silent drift — so it lives here, once.
 */
#ifndef HARP_TOOLS_RENDER_CHECK_H
#define HARP_TOOLS_RENDER_CHECK_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* FNV-1a over the raw float32 capture buffer. */
static inline uint64_t harp_fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* PCM16 stereo WAV — a listening aid, never hashed (the hash is over the
 * float buffer). Returns false on open failure; the caller reports. */
static inline bool harp_write_wav16(const std::string &path,
                                    const std::vector<float> &interleaved,
                                    uint32_t channels, uint32_t rate) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint32_t data_len = (uint32_t)interleaved.size() * 2;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t riff = 36 + data_len;
    memcpy(hdr + 4, &riff, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16;
    uint16_t pcm = 1, bits = 16;
    uint16_t align = (uint16_t)(channels * 2);
    uint32_t byterate = rate * align;
    memcpy(hdr + 16, &fmtlen, 4);
    memcpy(hdr + 20, &pcm, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byterate, 4);
    memcpy(hdr + 32, &align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_len, 4);
    fwrite(hdr, 1, 44, f);
    for (float v : interleaved) {
        if (v > 1.f) v = 1.f;
        if (v < -1.f) v = -1.f;
        int16_t s = (int16_t)(v * 32767.f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return true;
}

#endif /* HARP_TOOLS_RENDER_CHECK_H */
