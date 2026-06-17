/* voice_alloc.h — the §9.5 voice-allocation POLICY as a pure function.
 *
 * Which voice a note-on takes (and therefore steals) is the integer decision the
 * determinism oracle rests on: the chord/voice-pool golden hashes only reproduce
 * if every board makes the identical choice from the identical ordered event
 * stream. engine.c's voice_alloc() is that choice, but it is a static function on
 * the daemon-private synth_voice/part types, reachable from no host-built target
 * — so the policy was only ever exercised on hardware (voice-steal-test), which
 * cannot even tell WHICH voice was stolen.
 *
 * Factoring the decision out over PLAIN arrays (no device.h, no daemon types)
 * makes it unit-testable in CI on every push while keeping engine.c's real
 * note-on path calling THIS function — so a regression in the steal order is
 * caught off-hardware, before it can ship a wrong-but-deterministic render.
 */
#ifndef HARP_DEVICE_VOICE_ALLOC_H
#define HARP_DEVICE_VOICE_ALLOC_H

#include <stdbool.h>
#include <stdint.h>

/* Pick the voice index a note-on takes from a pool of `n`: the first FREE slot
 * in index order, else the active slot with the smallest alloc_seq (the oldest —
 * deterministic because alloc_seq is unique + monotone per part). Pure: a
 * function of (active[], alloc_seq[]) only. */
static inline int harp_voice_pick(const bool *active, const uint64_t *alloc_seq, int n) {
    for (int i = 0; i < n; i++)
        if (!active[i]) return i;
    int oldest = 0;
    for (int i = 1; i < n; i++)
        if (alloc_seq[i] < alloc_seq[oldest]) oldest = i;
    return oldest;
}

#endif /* HARP_DEVICE_VOICE_ALLOC_H */
