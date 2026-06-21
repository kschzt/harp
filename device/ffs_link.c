/* §4.3 FunctionFS endpoint framing I/O — see ffs_link.h. Portable (no functionfs.h):
 * just buffered read/write over two fds, so it unit-tests over a socketpair. */
#include "ffs_link.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

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
        ssize_t r = read(f->ep_out, f->rbuf, sizeof f->rbuf);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false; /* -ESHUTDOWN on disable/unbind: session over */
        }
        if (r == 0) continue;
        f->rlen = (size_t)r;
        f->rpos = 0;
    }
    return true;
}

static bool ffs_write_all(harp_io *io, const void *buf, size_t n) {
    ffs_io *f = (ffs_io *)io;
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(f->ep_in, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

harp_io *ffs_io_init(ffs_io *f, int ep_in, int ep_out) {
    memset(f, 0, sizeof *f);
    f->io.read_exact = ffs_read_exact;
    f->io.write_all = ffs_write_all;
    f->ep_in = ep_in;
    f->ep_out = ep_out;
    return &f->io;
}
