/* libusb transport for harp-probe (host side of the §4.3 USB binding). */
#ifndef HARP_USB_IO_H
#define HARP_USB_IO_H

#include "harp/link.h"

/* Scan the bus for a HARP device (vendor-specific interface class
 * 0xFF/0x48/0x01, per §4.3/§6.1 class-triple probe), claim the interface,
 * and return a transport. NULL on failure (message on stderr). */
harp_io *harp_usb_open(void);
void harp_usb_close(harp_io *io);

#endif
