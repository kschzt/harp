/* §4.3 FunctionFS bulk-endpoint FRAMING I/O (device/ffs_link.c), unit-tested over a
 * socketpair — the part of the USB gadget transport that is NOT kernel-specific, so it
 * runs in CI with no hardware. Covers: exact framing, reassembly across read boundaries,
 * reads that span multiple writes, write_all, and the session-over-on-dead-endpoint path.
 * (The descriptors / UDC bind / ep0 event loop in ffs.c stay irreducibly Pi-only.)
 * POSIX (socketpair); not built on Windows. */
#include "ffs_link.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (c) g_pass++;                                                  \
        else { g_fail++; fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } \
    } while (0)

int main(void) {
    /* Two socketpairs stand in for the kernel endpoints:
     *   ep_out (host -> device): the device reads f.ep_out; the test writes host_to_dev.
     *   ep_in  (device -> host): the device writes f.ep_in;  the test reads host_from_dev. */
    int op[2], ip[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, op) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, ip) == 0);
    int host_to_dev = op[0], dev_ep_out = op[1];
    int dev_ep_in = ip[0], host_from_dev = ip[1];

    ffs_io f;
    harp_io *io = ffs_io_init(&f, dev_ep_in, dev_ep_out);

    char buf[64];

    /* 1. exact read: one frame in, one read out */
    CHECK(write(host_to_dev, "HELLO", 5) == 5);
    memset(buf, 0, sizeof buf);
    CHECK(io->read_exact(io, buf, 5) && memcmp(buf, "HELLO", 5) == 0);

    /* 2. reassembly: 10 bytes delivered in one write, consumed in pieces (3,4,3) — the
     *    buffered reader must hand back exactly the bytes asked for, in order, no loss. */
    CHECK(write(host_to_dev, "0123456789", 10) == 10);
    memset(buf, 0, sizeof buf);
    CHECK(io->read_exact(io, buf, 3) && memcmp(buf, "012", 3) == 0);
    CHECK(io->read_exact(io, buf, 4) && memcmp(buf, "3456", 4) == 0);
    CHECK(io->read_exact(io, buf, 3) && memcmp(buf, "789", 3) == 0);

    /* 3. a read that SPANS multiple writes: read_exact must pull from the buffer and
     *    loop for more until it has the full count. */
    CHECK(write(host_to_dev, "AB", 2) == 2);
    CHECK(write(host_to_dev, "CD", 2) == 2);
    memset(buf, 0, sizeof buf);
    CHECK(io->read_exact(io, buf, 4) && memcmp(buf, "ABCD", 4) == 0);

    /* 4. write_all: the device writes; the host receives it intact */
    CHECK(io->write_all(io, "WORLD", 5));
    char r[8] = {0};
    CHECK(read(host_from_dev, r, 5) == 5 && memcmp(r, "WORLD", 5) == 0);

    /* 5. session-over: a dead endpoint (DISABLE/unbind closes the file) makes read_exact
     *    return false — not hang, not a torn read. Close the device's read fd so the next
     *    read() fails (-EBADF, the -ESHUTDOWN analogue). */
    CHECK(g_usb_errors == 0); /* §14.2: the clean reads/writes above logged no transport errors */
    close(dev_ep_out);
    memset(buf, 0, sizeof buf);
    CHECK(io->read_exact(io, buf, 1) == false);
    CHECK(g_usb_errors == 1); /* §14.2: the abnormal read error (EBADF) was counted */

    close(host_to_dev);
    close(dev_ep_in);
    close(host_from_dev);
    printf("harp-ffs-link-tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
