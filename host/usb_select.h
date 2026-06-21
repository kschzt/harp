#ifndef HARP_USB_SELECT_H
#define HARP_USB_SELECT_H
/* Pure §15.2 multi-device SELECTION predicate, extracted from usb_open_core
 * (host/usb_io.c) so the safety-critical "never bind a different synth" rule is
 * unit-testable WITHOUT libusb or hardware — same header-only pure-helper pattern
 * as device/voice_alloc.h / device/arp_select.h (host-unit-tested in
 * tests/harp_engine_logic_tests.c). usb_io.c includes this and calls it for the
 * real bind, so the test and the production path share one definition. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Does an enumerated HARP device (already class-matched by find_harp_interface)
 * with this serial + vid:pid satisfy the requested binding?
 *   want != NULL : require an EXACT serial match — reconnect pins the bound device
 *                  and a wrong serial is refused.
 *   want_vp      : require vid:pid == (want_vid, want_pid) — the same-model rule; a
 *                  different model is NEVER bound (don't drive a Digitone as a Digitakt).
 *   both off     : any HARP device — the fresh-any first bind.
 * The FIRST enumerated device for which this returns true AND whose interface then
 * claims (i.e. is unclaimed) is the one bound; the claim + iteration order is the
 * caller's loop, not this predicate. */
static inline bool harp_usb_dev_matches(const char *serial, uint16_t vid, uint16_t pid,
                                        const char *want, bool want_vp,
                                        uint16_t want_vid, uint16_t want_pid) {
    if (want && strcmp(want, serial) != 0) return false;                /* exact serial   */
    if (want_vp && (vid != want_vid || pid != want_pid)) return false;  /* never cross-model */
    return true;
}

#endif /* HARP_USB_SELECT_H */
