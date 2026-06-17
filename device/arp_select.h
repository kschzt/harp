/* arp_select.h — the §9.7 arpeggiator note-SELECTION as a pure function.
 *
 * arp_fire_due() in engine.c picks WHICH latched note sounds on each step for the
 * four modes (up / down / up-down-no-repeated-endpoints / as-played), applies the
 * octave span, pitch-sorts the latch for up/down (but not as-played), and clamps
 * a note that octaves above 127 back to the un-octaved pitch. That is pure
 * integer math with several easy-to-break branches (the up-down reflection, the
 * selection sort, the >127 clamp), yet it lived as a static function on the
 * daemon-private `part` type — reachable from no host target. Its only exercise
 * was tempo-lock-test, on hardware, which checks the 16th-note GRID but never
 * decodes the pitch, so it cannot tell up from down from up-down from as-played
 * (and every other script runs the arp OFF). A broken reflection or sort would
 * render a wrong-but-deterministic groove and pass.
 *
 * Factoring the selection out over PLAIN types (no device.h) makes all four modes
 * unit-testable in CI while engine.c's real arp keeps calling THIS function. The
 * groove golden (golden-test.sh) exercises mode 1 end-to-end on hardware, pinning
 * the extraction byte-identical; the unit test pins the other three.
 */
#ifndef HARP_DEVICE_ARP_SELECT_H
#define HARP_DEVICE_ARP_SELECT_H

#include <stdint.h>

/* The chosen step: `note` to sound (octave applied, clamped <=127 by dropping the
 * octave) and `sel` = the latch index it came from (for the velocity lookup). */
typedef struct {
    int note;
    int sel;
} harp_arp_pick;

/* Select the note for arp `step` (0-based, monotone). `mode`: 1 up, 2 down,
 * 3 up-down (no repeated endpoints), 4 as-played (anything else == up). `latch`:
 * the held notes in PRESS order, `nlatch` > 0 of them. `octaves`: the extra
 * octave span (param k12; 0 = one octave). `order` is caller scratch of at least
 * `nlatch` ints (the pitch-sorted index permutation) — passed in so this header
 * needs no fixed bound and stays decoupled from engine.c's ARP_LATCH_MAX. */
static inline harp_arp_pick harp_arp_select(int mode, int step, const uint8_t *latch,
                                            int nlatch, int octaves, int *order) {
    int span = nlatch * (octaves + 1); /* notes x octaves */
    int s = step % span;
    int idx, oct;
    switch (mode) {
        default:
        case 1: /* up */
            idx = s % nlatch;
            oct = s / nlatch;
            break;
        case 2: /* down */
            idx = (span - 1 - s) % nlatch;
            oct = (span - 1 - s) / nlatch;
            break;
        case 3: { /* up-down (no repeated endpoints) */
            int cycle = span > 1 ? 2 * span - 2 : 1;
            int t = step % cycle;
            if (t >= span) t = cycle - t;
            idx = t % nlatch;
            oct = t / nlatch;
            break;
        }
        case 4: /* as played */
            idx = s % nlatch;
            oct = s / nlatch;
            break;
    }
    /* up/down sort by pitch; as-played keeps press order */
    for (int i = 0; i < nlatch; i++) order[i] = i;
    if (mode != 4)
        for (int i = 0; i < nlatch; i++)
            for (int j = i + 1; j < nlatch; j++)
                if (latch[order[j]] < latch[order[i]]) {
                    int t = order[i];
                    order[i] = order[j];
                    order[j] = t;
                }
    int note = latch[order[idx]] + 12 * oct;
    if (note > 127) note = latch[order[idx]];
    harp_arp_pick r = { note, order[idx] };
    return r;
}

#endif /* HARP_DEVICE_ARP_SELECT_H */
