/* §4.3 FunctionFS endpoint framing I/O — see ffs_link.h. Portable (no functionfs.h):
 * just buffered read/write over two fds, so it unit-tests over a socketpair. */
#include "ffs_link.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

_Atomic uint64_t g_usb_errors = 0; /* §14.2 — see ffs_link.h */

static bool ffs_read_exact(harp_io *io, void *buf, size_t n) {
    ffs_io *f = (ffs_io *)io;
    uint8_t *p = buf;
    while (n) {
        if (f->rpos < f->rlen) {
            size_t take = f->rlen - f->rpos;
            if (take > n) take = n;
            memcpy(p, f->rbuf + f->rpos, take);
            f->rpos += take;
            p += take;
            n -= take;
            continue;
        }
        ssize_t r = f->rd(f->ep_out, f->rbuf, sizeof f->rbuf);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno != ESHUTDOWN) /* §14.2: count abnormal errors, not the clean disable */
                atomic_fetch_add_explicit(&g_usb_errors, 1, memory_order_relaxed);
            return false; /* -ESHUTDOWN on disable/unbind: session over */
        }
        if (r == 0) continue;
        f->rlen = (size_t)r;
        f->rpos = 0;
    }
    return true;
}

/* Non-blocking readiness (harp_io.readable): true iff bytes are ALREADY sitting in the
 * userspace reassembly buffer, i.e. the next ffs_read_exact can make progress without a
 * read(2) syscall. This is the batching signal for the device event-consume loop — the host
 * streams several small EVT messages that the kernel hands us in one ≤16 KiB chunk, so after
 * decoding one message rpos<rlen means the next is already here and can be decoded+staged
 * under the same evq lock. No syscall; when the buffer drains (rpos==rlen) it returns false
 * and the loop flushes the staged run at once (nothing is held back). It is deliberately
 * conservative — it does NOT read(2) to see whether the kernel has more — so it never blocks. */
static bool ffs_readable(harp_io *io) {
    ffs_io *f = (ffs_io *)io;
    return f->rpos < f->rlen;
}

static bool ffs_write_all(harp_io *io, const void *buf, size_t n) {
    ffs_io *f = (ffs_io *)io;
    const uint8_t *p = buf;
    size_t total = n;
    while (n) {
        ssize_t r = f->wr(f->ep_in, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno != ESHUTDOWN) /* §14.2: count abnormal errors, not the clean disable */
                atomic_fetch_add_explicit(&g_usb_errors, 1, memory_order_relaxed);
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    /* ZLP terminator (§4.3): a framed message whose length is an exact multiple of
     * the bulk-IN wMaxPacketSize (512 @ high-speed, see ffs.c) ends on a FULL packet
     * with no short packet to terminate the transfer. Linux's host stack auto-
     * terminates, but macOS libusb BLOCKS the bulk-IN read waiting for the
     * terminator — so the hello/identity round-trip hangs (the device sessions up
     * but never "answers" on a Mac; CI on Linux is unaffected, which is why this
     * survived). Emit a zero-length packet so every message ends short. A 0-byte
     * write is a harmless no-op over the socketpair the unit test uses. */
    if (total && (total % 512) == 0) {
        ssize_t r;
        do { r = f->wr(f->ep_in, "", 0); } while (r < 0 && errno == EINTR);
        if (r < 0 && errno != ESHUTDOWN) /* message already delivered; ZLP failure is non-fatal */
            atomic_fetch_add_explicit(&g_usb_errors, 1, memory_order_relaxed);
    }
    return true;
}

harp_io *ffs_io_init(ffs_io *f, int ep_in, int ep_out) {
    memset(f, 0, sizeof *f);
    f->io.read_exact = ffs_read_exact;
    f->io.write_all = ffs_write_all;
    f->io.readable = ffs_readable; /* §consume batching: userspace-buffered readiness, no syscall */
    f->ep_in = ep_in;
    f->ep_out = ep_out;
    f->rd = read; /* seam defaults to the real syscalls (ffs.c path unchanged); */
    f->wr = write; /* a test overrides these to inject endpoint errors. */
    return &f->io;
}
