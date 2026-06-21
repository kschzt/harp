#ifndef HARP_FFS_LINK_H
#define HARP_FFS_LINK_H
/* The §4.3 FunctionFS bulk-endpoint FRAMING I/O, factored out of device/ffs.c (which
 * is locked to <linux/usb/functionfs.h>) so the buffered read/write logic — the part
 * that isn't kernel-gadget-specific — can be unit-tested over a socketpair on any POSIX
 * host, with no hardware. ffs.c wires this onto the real ep1/ep2 endpoint files; the
 * test wires it onto socketpairs. (The descriptors, UDC bind, and ep0 event loop stay
 * in ffs.c and remain Pi-only.) */
#include "harp/link.h"
#include <stddef.h>
#include <stdint.h>

#define FFS_READ_CHUNK 16384 /* multiple of 512 (HS wMaxPacketSize) */

typedef struct {
    harp_io io;
    int ep_in;  /* device -> host: we write */
    int ep_out; /* host -> device: we read  */
    uint8_t rbuf[FFS_READ_CHUNK];
    size_t rlen, rpos;
} ffs_io;

/* Wire the buffered framing onto two endpoint fds (real FunctionFS endpoints, or a
 * socketpair in a test). Sets io.read_exact/write_all; returns &f->io for the session.
 * read_exact reassembles across kernel read boundaries; a read error that isn't EINTR
 * (-ESHUTDOWN on DISABLE/unbind, or a dead endpoint fd) ends the session by returning
 * false — exactly like a TCP disconnect. A 0-length read (a ZLP) is retried. */
harp_io *ffs_io_init(ffs_io *f, int ep_in, int ep_out);

#endif /* HARP_FFS_LINK_H */
