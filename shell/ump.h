/* UMP word construction for the shells (§9.10). The refdev consumes one
 * UMP flavor: MIDI 2.0 message-type-2 (MIDI-1.0 channel voice), group 0 —
 * note on/off and the all-notes-off panic. Centralized here so the wire
 * layout lives in ONE place instead of as bare hex in the VST3 shell, the
 * AU shell, and the runtime.
 *
 * Word layout (big-endian nibbles): 0x2<grp> <status><chan> <data1> <data2>
 *   note on  : 0x2 0 9 c nn vv      note off : 0x2 0 8 c nn 40
 *   all-off  : 0x2 0 B 0 7B 00      (CC 123)
 *
 * The channel nibble `c` (bits 16..19) selects the device part (P2.1):
 * channel 0 is the default and, crucially, leaves the word bit-for-bit
 * identical to the historic channel-less form so existing renders and the
 * device golden stay byte-identical. C has no default arguments — every
 * caller passes a channel explicitly; pass 0 for the legacy behavior.
 */
#ifndef HARP_SHELL_UMP_H
#define HARP_SHELL_UMP_H

#include <stdint.h>

static inline uint32_t ump_note_on(uint32_t note, uint32_t vel, uint32_t channel) {
    return 0x20900000u | ((channel & 0xf) << 16) | ((note & 0x7f) << 8) | (vel & 0x7f);
}
static inline uint32_t ump_note_off(uint32_t note, uint32_t channel) {
    return 0x20800000u | ((channel & 0xf) << 16) | ((note & 0x7f) << 8) |
           0x40; /* 0x40 = default release vel */
}
static inline uint32_t ump_all_notes_off(void) {
    return 0x20B07B00u; /* CC 123 */
}

#endif /* HARP_SHELL_UMP_H */
