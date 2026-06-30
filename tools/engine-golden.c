/* tools/engine-golden — debt #19: the DEVICE engine-render golden, cloud-gated.
 *
 * The shell-side host-paced bounce is already cloud-gated (offline-golden-eth.sh); this pins the
 * RAW device DSP — render_output() / engine_voices_cold() driven DIRECTLY, with no shell, no
 * transport, no USB, no daemon. It plays a FIXED note sequence through the engine's DEFAULT golden
 * path (n_out_slots = 2, slots {0,1} -> render_with_events, the byte-identical stereo main mix that
 * the VST3/AU/CLAP goldens also render) and FNV-1a hashes every rendered float.
 *
 * Deterministic by construction (cold voices + reset evq + fixed factory params), so the program
 * renders the sequence TWICE and checks:
 *   - hash(run1) == hash(run2): the synth DSP is deterministic (no uninitialised state, no race);
 *   - rms > 0: the engine actually made sound (the never-silent contract, at the DSP layer);
 *   - and, when the caller supplies an expected hash for this platform, the absolute value — the
 *     regression oracle. libm sin/exp differ across OSes, so that pin is PER-OS (the driving
 *     script holds the table); determinism + non-silence are asserted on every platform.
 *
 * Prints `engine-render-hash:` / `engine-render-rms:` so a fresh CI run reveals the per-OS hash to
 * pin. Exit 0 on a deterministic, non-silent render; 1 otherwise.
 */
#include "device.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NS 256       /* samples per render block */
#define RATE 48000

static uint64_t fnv1a(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* Render the fixed sequence once from a cold engine; return the FNV-1a hash over every rendered
 * float and (out) the mean-square energy so the caller can assert non-silence. */
static uint64_t render_once(double *out_ms) {
    audio_state a;
    memset(&a, 0, sizeof a);
    a.rate = RATE;
    a.tone_hz = 0.0;   /* synth path, NOT the measurement tone (which would bypass the voices) */
    a.n_out_slots = 2; /* the DEFAULT golden path: stereo main mix on slots {0,1} */
    a.out_slots[0] = 0;
    a.out_slots[1] = 1;

    evq_reset_for_new_stream();
    engine_voices_cold();

    /* A fixed, musically meaningful sequence: a C-major triad entered as an arpeggio, held, then
     * released together — exercises voice_note_on, the voice pool, the envelopes, and the summing
     * path. ts is the absolute stream sample position at which the event applies. */
    static const struct {
        uint64_t ts;
        uint8_t kind;
        uint32_t note;
        float vel;
    } seq[] = {
        {0, DEV_EV_NOTE_ON, 60, 0.80f},
        {RATE / 4, DEV_EV_NOTE_ON, 64, 0.70f},
        {RATE / 2, DEV_EV_NOTE_ON, 67, 0.90f},
        {RATE * 3 / 2, DEV_EV_NOTE_OFF, 60, 0.0f},
        {RATE * 3 / 2, DEV_EV_NOTE_OFF, 64, 0.0f},
        {RATE * 3 / 2, DEV_EV_NOTE_OFF, 67, 0.0f},
    };
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        dev_event ev = {0};
        ev.ts = seq[i].ts;
        ev.kind = seq[i].kind;
        ev.a = seq[i].note;
        ev.v = seq[i].vel;
        ev.channel = 0;
        evq_push(ev);
    }

    uint64_t h = 1469598103934665603ull;
    double sumsq = 0.0;
    uint64_t count = 0;
    float out[NS * 34];
    const uint32_t nblocks = (RATE * 2) / NS; /* 2 seconds */
    uint64_t pos = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        memset(out, 0, sizeof out);
        uint16_t slots = render_output(&a, out, NS, (float)RATE, pos);
        size_t nf = (size_t)slots * NS;
        h = fnv1a(h, out, nf * sizeof(float));
        for (size_t i = 0; i < nf; i++) sumsq += (double)out[i] * (double)out[i];
        count += nf;
        pos += NS;
    }
    *out_ms = count ? sumsq / (double)count : 0.0;
    return h;
}

int main(void) {
    double ms1 = 0.0, ms2 = 0.0;
    uint64_t h1 = render_once(&ms1);
    uint64_t h2 = render_once(&ms2);
    double rms = sqrt(ms1);

    printf("engine-render-hash: %016llx\n", (unsigned long long)h1);
    printf("engine-render-rms: %.6f\n", rms);

    if (h1 != h2) {
        fprintf(stderr, "ENGINE-GOLDEN FAIL: non-deterministic render (%016llx != %016llx)\n",
                (unsigned long long)h1, (unsigned long long)h2);
        return 1;
    }
    if (!(rms > 1e-4)) {
        fprintf(stderr, "ENGINE-GOLDEN FAIL: silent render (rms %.6f) — the engine made no sound\n", rms);
        return 1;
    }
    return 0;
}
