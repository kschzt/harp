/* §4.3 FunctionFS bulk-endpoint FRAMING I/O (device/ffs_link.c), unit-tested over a
 * socketpair — the part of the USB gadget transport that is NOT kernel-specific, so it
 * runs in CI with no hardware. Covers: exact framing, reassembly across read boundaries,
 * reads that span multiple writes, write_all, and the session-over-on-dead-endpoint path.
 * (The descriptors / UDC bind / ep0 event loop in ffs.c stay irreducibly Pi-only.)
 * POSIX (socketpair); not built on Windows. */
#include "ffs_link.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.h"

/* ---- fault-injection seam: a scripted fake endpoint read/write ----
 * A socketpair can only ever produce healthy bytes or a dead fd (-EBADF); it cannot
 * reproduce the FunctionFS error CLASSES the real dwc2 gadget returns. These mocks drive
 * the ffs_io.rd/wr seam so the §14.2 counting policy (count abnormal, never the clean
 * -ESHUTDOWN disable, transparently retry -EINTR) and partial-read reassembly are asserted
 * directly. g_inj_errno is one-shot (fires once, then clears) so a retry sees what follows. */
static int g_inj_errno;       /* next rd/wr returns -1 with this errno, then clears */
static int g_inj_chunk;       /* persistent: cap each fake read to this many bytes (0 = uncapped) */
static const char *g_inj_src; /* bytes the fake read delivers, in order */
static size_t g_inj_len, g_inj_pos;

static ssize_t fake_rd(int fd, void *buf, size_t n) {
    (void)fd;
    if (g_inj_errno) { errno = g_inj_errno; g_inj_errno = 0; return -1; }
    size_t avail = g_inj_len - g_inj_pos, take = avail < n ? avail : n;
    if (g_inj_chunk && take > (size_t)g_inj_chunk) take = (size_t)g_inj_chunk;
    memcpy(buf, g_inj_src + g_inj_pos, take);
    g_inj_pos += take;
    return (ssize_t)take;
}
static ssize_t fake_wr(int fd, const void *buf, size_t n) {
    (void)fd;
    (void)buf;
    if (g_inj_errno) { errno = g_inj_errno; g_inj_errno = 0; return -1; }
    return (ssize_t)n; /* accept everything, including the 0-byte ZLP terminator */
}

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

    /* 6. FunctionFS error-class fault injection — the part the socketpair cannot reach.
     *    Wire a second link onto the scripted mocks (no real fds) and assert the precise
     *    §14.2 policy per error class, plus partial-read reassembly and the ZLP path. */
    ffs_io fz;
    harp_io *z = ffs_io_init(&fz, -1, -1);
    fz.rd = fake_rd;
    fz.wr = fake_wr;
    uint64_t base = atomic_load_explicit(&g_usb_errors, memory_order_relaxed);

    /* 6a. abnormal WRITE error (a bulk-IN STALL surfaces as -EPIPE): write_all fails AND
     *     §14.2 counts it — a torn transfer the host must see. */
    g_inj_errno = EPIPE;
    CHECK(z->write_all(z, "x", 1) == false);
    CHECK(atomic_load_explicit(&g_usb_errors, memory_order_relaxed) == base + 1);

    /* 6b. clean -ESHUTDOWN WRITE (host DISABLE / UDC unbind): write_all fails but is NOT
     *     counted — an orderly session end, not a transport fault. */
    g_inj_errno = ESHUTDOWN;
    CHECK(z->write_all(z, "x", 1) == false);
    CHECK(atomic_load_explicit(&g_usb_errors, memory_order_relaxed) == base + 1); /* unchanged */

    /* 6c. abnormal READ error -> false + counted (the read-side mirror of 6a). */
    g_inj_errno = EPIPE;
    CHECK(z->read_exact(z, buf, 4) == false);
    CHECK(atomic_load_explicit(&g_usb_errors, memory_order_relaxed) == base + 2);

    /* 6d. clean -ESHUTDOWN READ -> false, NOT counted (the read-side mirror of 6b — this is
     *     the everyday disable path on every host teardown). */
    g_inj_errno = ESHUTDOWN;
    CHECK(z->read_exact(z, buf, 4) == false);
    CHECK(atomic_load_explicit(&g_usb_errors, memory_order_relaxed) == base + 2); /* unchanged */

    /* 6e. -EINTR is transparently retried, never surfaced and never counted: one EINTR,
     *     then the real bytes arrive and read_exact succeeds. */
    g_inj_src = "OK";
    g_inj_len = 2;
    g_inj_pos = 0;
    g_inj_chunk = 0;
    g_inj_errno = EINTR;
    memset(buf, 0, sizeof buf);
    CHECK(z->read_exact(z, buf, 2) && memcmp(buf, "OK", 2) == 0);
    CHECK(atomic_load_explicit(&g_usb_errors, memory_order_relaxed) == base + 2); /* EINTR uncounted */

    /* 6f. partial kernel reads: a 5-byte frame dribbled back ONE byte per read() must still
     *     reassemble exactly — the dwc2 short-packet behavior the socketpair won't force. */
    g_inj_src = "FRAME";
    g_inj_len = 5;
    g_inj_pos = 0;
    g_inj_chunk = 1;
    memset(buf, 0, sizeof buf);
    CHECK(z->read_exact(z, buf, 5) && memcmp(buf, "FRAME", 5) == 0);

    /* 6g. a frame whose length is an exact multiple of 512 emits the ZLP terminator (§4.3)
     *     and still succeeds through the seam (fake_wr accepts the trailing 0-byte write),
     *     logging no error — the macOS hello-hang fix path, exercised without hardware. */
    g_inj_chunk = 0;
    char big[512];
    memset(big, 'Z', sizeof big);
    CHECK(z->write_all(z, big, sizeof big));
    CHECK(atomic_load_explicit(&g_usb_errors, memory_order_relaxed) == base + 2); /* clean */

    close(host_to_dev);
    close(dev_ep_in);
    close(host_from_dev);
    return check_report("harp-ffs-link-tests");
}
