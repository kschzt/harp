/* libusb transport for harp-probe (host side of the §4.3 USB binding). */
#ifndef HARP_USB_IO_H
#define HARP_USB_IO_H

#include "harp/link.h"

/* Scan the bus for a HARP device (vendor-specific interface class
 * 0xFF/0x48/0x01, per §4.3/§6.1 class-triple probe), claim the interface,
 * and return a transport. NULL on failure (message on stderr). */
harp_io *harp_usb_open(void);
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
