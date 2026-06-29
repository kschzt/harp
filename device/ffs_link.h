#ifndef HARP_FFS_LINK_H
#define HARP_FFS_LINK_H
/* The §4.3 FunctionFS bulk-endpoint FRAMING I/O, factored out of device/ffs.c (which
 * is locked to <linux/usb/functionfs.h>) so the buffered read/write logic — the part
 * that isn't kernel-gadget-specific — can be unit-tested over a socketpair on any POSIX
 * host, with no hardware. ffs.c wires this onto the real ep1/ep2 endpoint files; the
 * test wires it onto socketpairs. (The descriptors, UDC bind, and ep0 event loop stay
 * in ffs.c and remain Pi-only.) */
#include "harp/link.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t for the rd/wr seam */

/* §14.2 usb_errors: abnormal FunctionFS transport errors — read/write failures that are
 * neither EINTR nor the clean -ESHUTDOWN disable. Lives here (not in the device struct)
 * because the framing is deliberately decoupled from device.h; emit_counters reads it on
 * Linux (it stays 0 on the TCP/eth dev transport, which has no FunctionFS path). */
extern _Atomic uint64_t g_usb_errors;

#define FFS_READ_CHUNK 16384 /* multiple of 512 (HS wMaxPacketSize) */

typedef struct {
    harp_io io;
    int ep_in;  /* device -> host: we write */
    int ep_out; /* host -> device: we read  */
    uint8_t rbuf[FFS_READ_CHUNK];
    size_t rlen, rpos;
    /* Syscall seam: the endpoint read/write, indirected so a unit test can inject the
     * FunctionFS error classes the socketpair can't reach — a -EINTR (retry), the clean
     * -ESHUTDOWN disable (must NOT count §14.2), an abnormal STALL/EPIPE (must count), and
     * partial kernel reads (reassembly). ffs_io_init defaults these to read(2)/write(2),
     * so ffs.c and the real gadget are byte-for-byte unchanged. */
    ssize_t (*rd)(int, void *, size_t);
    ssize_t (*wr)(int, const void *, size_t);
} ffs_io;

/* Wire the buffered framing onto two endpoint fds (real FunctionFS endpoints, or a
 * socketpair in a test). Sets io.read_exact/write_all; returns &f->io for the session.
 * read_exact reassembles across kernel read boundaries; a read error that isn't EINTR
 * (-ESHUTDOWN on DISABLE/unbind, or a dead endpoint fd) ends the session by returning
 * false — exactly like a TCP disconnect. A 0-length read (a ZLP) is retried. */
harp_io *ffs_io_init(ffs_io *f, int ep_in, int ep_out);

#endif /* HARP_FFS_LINK_H */
