/* libusb transport for harp-probe (host side of the §4.3 USB binding). */
#ifndef HARP_USB_IO_H
#define HARP_USB_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "harp/link.h"

/* Scan the bus for a HARP device (vendor-specific interface class
 * 0xFF/0x48/0x01, per §4.3/§6.1 class-triple probe), claim the interface,
 * and return a transport. NULL on failure (message on stderr). */
/* A HARP device's USB identity, as seen pre-hello at the descriptor layer.
 * (vendor_id, product_id) is the "same hardware model" key; serial picks a
 * specific unit. Distinct synth models carry distinct product_ids. */
typedef struct {
    uint16_t vendor_id, product_id;
    char serial[64];
} harp_usb_devinfo;

harp_io *harp_usb_open(void);
/* Open a specific device by USB serial (NULL = first match). */
harp_io *harp_usb_open_serial(const char *serial);
/* Open+claim by the multi-device policy: exact serial if want_serial set;
 * else first unclaimed of model (want_vid,want_pid) if want_vp; else first
 * unclaimed HARP device of any model. Claim is the mutual exclusion. */
harp_io *harp_usb_open_match(const char *want_serial, bool want_vp,
                             uint16_t want_vid, uint16_t want_pid);
/* A persistent libusb context for a long-lived caller (the plugin
 * supervisor): create once, reuse it across many open/close cycles via
 * harp_usb_open_match_ctx, destroy once at teardown. Avoids per-attempt
 * libusb_init/exit churn (fragile on Windows, esp. device-less). Opaque so
 * callers need not include libusb.h; returns NULL on failure. */
void *harp_usb_ctx_create(void);
void harp_usb_ctx_destroy(void *ctx);
/* Like harp_usb_open_match but borrows a caller-owned context (from
 * harp_usb_ctx_create). The returned transport does not own the context;
 * harp_usb_close leaves it intact for the next open. */
harp_io *harp_usb_open_match_ctx(void *ctx, const char *want_serial, bool want_vp,
                                 uint16_t want_vid, uint16_t want_pid);
/* Read the bound device's USB identity back out (after open). */
bool harp_usb_devident(harp_io *io, harp_usb_devinfo *out);
/* Read-only enumeration of all HARP devices on the bus (no claim). Fills
 * up to cap entries; returns total count (may exceed cap). */
size_t harp_usb_enumerate(harp_usb_devinfo *out, size_t cap);
void harp_usb_close(harp_io *io);

/* The dedicated audio endpoint pair (§8.2), if the device exposes one.
 * Audio bypasses harp_io message framing: read raw stream chunks directly. */
bool harp_usb_has_audio(harp_io *io);
/* Read one bulk transfer from the audio IN endpoint. Returns byte count,
 * 0 on timeout, -1 on error. */
int harp_usb_audio_read(harp_io *io, void *buf, int len, unsigned timeout_ms);
/* Write to the audio OUT endpoint (host-paced pacing/input frames, §8.3). */
bool harp_usb_audio_write(harp_io *io, const void *buf, int len, unsigned timeout_ms);

/* Asynchronous link traffic (event echoes, notifications): one short-
 * timeout bulk read into the link buffer; true if bytes are now pending. */
bool harp_usb_link_poll(harp_io *io, unsigned timeout_ms);
size_t harp_usb_link_pending(harp_io *io);

#endif
