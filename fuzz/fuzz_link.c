/* libFuzzer: framed-link reassembly (§4.2) — the very first parser that
 * touches peer bytes. A memory-backed harp_io replays the fuzz input as
 * the inbound byte pipe; recv until EOF/violation. Exercises header
 * validation, FIN reassembly, per-stream accumulation, oversize and
 * truncation handling. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "harp/link.h"

typedef struct {
    harp_io io;
    const uint8_t *p, *end;
} mem_io;

static bool mem_read(harp_io *io, void *buf, size_t n) {
    mem_io *m = (mem_io *)io;
    if ((size_t)(m->end - m->p) < n) return false; /* EOF mid-read */
    memcpy(buf, m->p, n);
    m->p += n;
    return true;
}

static bool mem_write(harp_io *io, const void *buf, size_t n) {
    (void)io;
    (void)buf;
    (void)n;
    return true; /* discard: recv path only */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    mem_io m;
    m.io.read_exact = mem_read;
    m.io.write_all = mem_write;
    m.p = data;
    m.end = data + size;

    harp_link l;
    harp_link_init(&l);
    harp_cbuf msg;
    harp_cbuf_init(&msg);
    uint8_t stream;
    for (int i = 0; i < 256; i++)
        if (harp_link_recv(&m.io, &l, &stream, &msg) != 0) break;
    harp_cbuf_free(&msg);
    harp_link_free(&l);
    return 0;
}
