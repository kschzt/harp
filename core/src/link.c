#include "harp/link.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

void harp_link_init(harp_link *l) {
    for (int i = 0; i < 4; i++) harp_cbuf_init(&l->acc[i]);
}

void harp_link_free(harp_link *l) {
    for (int i = 0; i < 4; i++) harp_cbuf_free(&l->acc[i]);
}

bool harp_read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false; /* EOF */
        p += r;
        n -= (size_t)r;
    }
    return true;
}

bool harp_write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool fd_read_exact(harp_io *io, void *buf, size_t n) {
    return harp_read_exact(((harp_io_fd *)io)->rfd, buf, n);
}

static bool fd_write_all(harp_io *io, const void *buf, size_t n) {
    return harp_write_all(((harp_io_fd *)io)->wfd, buf, n);
}

void harp_io_fd_init(harp_io_fd *f, int rfd, int wfd) {
    f->io.read_exact = fd_read_exact;
    f->io.write_all = fd_write_all;
    f->rfd = rfd;
    f->wfd = wfd;
}

int harp_link_recv(harp_io *io, harp_link *l, uint8_t *stream, harp_cbuf *msg) {
    uint8_t hdr[HARP_FRAME_HDR_LEN];
    uint8_t payload[HARP_FRAME_MAX_PAYLOAD];
    for (;;) {
        if (!io->read_exact(io, hdr, sizeof hdr)) return -1;
        harp_frame_hdr h;
        if (!harp_frame_hdr_decode(hdr, &h)) return -2; /* fatal, §4.2 */
        if (h.length && !io->read_exact(io, payload, h.length)) return -1;
        if (h.stream >= 4) continue; /* vendor-experimental: skip */
        harp_cbuf *acc = &l->acc[h.stream];
        harp_cbuf_put(acc, payload, h.length);
        if (acc->oom) return -2;
        if (h.flags & HARP_FLAG_FIN) {
            harp_cbuf_reset(msg);
            harp_cbuf_put(msg, acc->buf, acc->len);
            harp_cbuf_reset(acc);
            *stream = h.stream;
            return msg->oom ? -2 : 0;
        }
    }
}

int harp_link_send(harp_io *io, uint8_t stream, const void *data, size_t len) {
    const uint8_t *p = data;
    do {
        size_t chunk = len > HARP_FRAME_MAX_PAYLOAD ? HARP_FRAME_MAX_PAYLOAD : len;
        harp_frame_hdr h = {HARP_FRAME_FVER, stream,
                            (uint16_t)(chunk == len ? HARP_FLAG_FIN : 0), (uint32_t)chunk};
        uint8_t hdr[HARP_FRAME_HDR_LEN];
        harp_frame_hdr_encode(&h, hdr);
        if (!io->write_all(io, hdr, sizeof hdr)) return -1;
        if (chunk && !io->write_all(io, p, chunk)) return -1;
        p += chunk;
        len -= chunk;
    } while (len);
    return 0;
}
