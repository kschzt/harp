/* libFuzzer: the §4.3 FunctionFS bulk-endpoint PARSE path (device/ffs_link.c) —
 * the buffered endpoint reassembly the real USB gadget runs on the Pi, fuzzed with
 * NO hardware. device/ffs.c wires ffs_link's read_exact/write_all onto the kernel
 * ep1/ep2 files; only the descriptors + UDC bind + ep0 event loop are irreducibly
 * Pi-only. This target replays the fuzz input through the ffs_io.rd SEAM (the same
 * indirection the unit test uses) with ARBITRARY, input-controlled chunk boundaries,
 * so ffs_read_exact's cross-read reassembly (the dwc2 short-packet behavior) is
 * stressed; the reassembled bytes then drive harp_link_recv (§4.2 framing) exactly
 * as the gadget does. This closes the USB parse surface that fuzz-link — a trivial
 * exact-size mem_io that never fragments a read — cannot reach.
 *
 * EOF is signalled to the reader as -ESHUTDOWN (the clean host-DISABLE analogue),
 * so the framing loop unwinds cleanly WITHOUT spinning on ffs_read_exact's r==0
 * (ZLP) retry and WITHOUT polluting the §14.2 g_usb_errors counter. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ffs_link.h"
#include "harp/link.h"

static const uint8_t *g_data;
static size_t g_len, g_pos, g_chunk;

/* endpoint-read seam: hand back the next <= g_chunk bytes of the fuzz input; once
 * drained, fail with -ESHUTDOWN so the framing loop ends (never returns 0, which
 * ffs_read_exact treats as a ZLP and retries -> would spin at EOF). */
static ssize_t fz_rd(int fd, void *buf, size_t n) {
    (void)fd;
    if (g_pos >= g_len) {
        errno = ESHUTDOWN; /* clean disable: session over, NOT counted (§14.2) */
        return -1;
    }
    size_t take = g_len - g_pos;
    if (take > n) take = n;
    if (g_chunk && take > g_chunk) take = g_chunk;
    memcpy(buf, g_data + g_pos, take);
    g_pos += take;
    return (ssize_t)take;
}

/* recv-only fuzzer: sink the device->host write side (no host to read it). */
static ssize_t fz_wr(int fd, const void *buf, size_t n) {
    (void)fd;
    (void)buf;
    return (ssize_t)n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* First byte selects the endpoint-read fragmentation so the mutator explores
     * EVERY reassembly boundary: 0 => uncapped whole-buffer reads (one big kernel
     * read); 1..255 => that many bytes per read (down to the 1-byte dribble that
     * forces maximal cross-read reassembly). The remainder is the inbound pipe. */
    g_chunk = size ? data[0] : 0;
    g_data = size ? data + 1 : data;
    g_len = size ? size - 1 : 0;
    g_pos = 0;

    ffs_io f;
    harp_io *io = ffs_io_init(&f, -1, -1); /* no real fds — the seam drives the I/O */
    f.rd = fz_rd;
    f.wr = fz_wr;

    harp_link l;
    harp_link_init(&l);
    harp_cbuf msg;
    harp_cbuf_init(&msg);
    uint8_t stream;
    for (int i = 0; i < 512; i++)
        if (harp_link_recv(io, &l, &stream, &msg) != 0) break;
    harp_cbuf_free(&msg);
    harp_link_free(&l);
    return 0;
}
