#include "harp/link.h"

#include <errno.h>
#include <string.h>

/* The fd-backed transport below uses raw byte read/write. On POSIX this serves
 * sockets, pipes, and FunctionFS endpoint files alike. On Windows _read/_write
 * work for CRT fds (files/pipes) only — sockets there are a separate handle
 * namespace served by a Winsock recv/send harp_io, so the fd path is the
 * file/pipe transport and the socket transport is registered separately. */
#ifdef _WIN32
#  include <io.h> /* _read, _write */
#else
#  include <unistd.h>
#endif

void harp_link_init(harp_link *l) {
    for (int i = 0; i < 4; i++) harp_cbuf_init(&l->acc[i]);
}

void harp_link_free(harp_link *l) {
    for (int i = 0; i < 4; i++) harp_cbuf_free(&l->acc[i]);
}

/* Frames are <= 65536+8 bytes, so a size_t length always fits the platform
 * read/write count; clamp defensively on Windows where _read takes unsigned. */
#ifdef _WIN32
static int io_read(int fd, void *p, size_t n) {
    return _read(fd, p, n > 0x7fffffffu ? 0x7fffffffu : (unsigned)n);
}
static int io_write(int fd, const void *p, size_t n) {
    return _write(fd, p, n > 0x7fffffffu ? 0x7fffffffu : (unsigned)n);
}
#else
#  include <sys/types.h> /* ssize_t */
static ssize_t io_read(int fd, void *p, size_t n) { return read(fd, p, n); }
static ssize_t io_write(int fd, const void *p, size_t n) { return write(fd, p, n); }
#endif

bool harp_read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        long r = (long)io_read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue; /* POSIX only; _read never sets it */
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
        long r = (long)io_write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue; /* POSIX only; _write never sets it */
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
    f->io.corrupt_pct = 0; /* §8.7 fault injection off by default; the device opts in per-io */
    f->io.readable = NULL; /* fd/pipe transport: no readiness probe -> the consume loop never batches */
    f->rfd = rfd;
    f->wfd = wfd;
}

/* §4.2.1 per-stream reassembled-message cap. OBJ is credit-gated; the 16 MiB OBJ cap is a hard
 * safety bound so a peer ignoring its credit window can't grow the accumulator without limit. */
static size_t harp_stream_msg_cap(uint8_t stream) {
    switch (stream) {
        case HARP_STREAM_CTL: return HARP_CTL_MAX_PAYLOAD;
        case HARP_STREAM_EVT: return HARP_EVT_MAX_PAYLOAD;
        case HARP_STREAM_LOG: return HARP_LOG_MAX_PAYLOAD;
        default:              return HARP_OBJ_MAX_PAYLOAD; /* OBJ: credit-gated + a hard cap */
    }
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
        /* §4.2.1: enforce the per-stream message bound during reassembly. An over-cap
         * ctl/evt/log message is malformed (-> session reset, §12.4); checking mid-
         * reassembly also stops a peer growing the accumulator without ever sending FIN. */
        size_t cap = harp_stream_msg_cap(h.stream);
        if (cap && acc->len > cap) return -2;
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
    static unsigned cf = 0; /* deterministic frame counter for §8.7 --corrupt-ctl-pct */
    const uint8_t *p = data;
    do {
        size_t chunk = len > HARP_FRAME_MAX_PAYLOAD ? HARP_FRAME_MAX_PAYLOAD : len;
        harp_frame_hdr h = {HARP_FRAME_FVER, stream,
                            (uint16_t)(chunk == len ? HARP_FLAG_FIN : 0), (uint32_t)chunk};
        uint8_t hdr[HARP_FRAME_HDR_LEN];
        harp_frame_hdr_encode(&h, hdr);
        if (!io->write_all(io, hdr, sizeof hdr)) return -1;
        if (chunk) {
            unsigned fn = cf++;
            if (io->corrupt_pct > 0 && io->corrupt_pct <= 100 && ((fn + 1u) * 2654435761u) % 100u < (unsigned)io->corrupt_pct) {
                /* §8.7 fault injection (device-only --corrupt-ctl-pct): flip ONE payload byte
                 * so the host decodes a corrupt frame and must survive (no crash/UB). The host
                 * never sets corrupt_pct, so host->device framing is untouched. Split-write,
                 * no copy. */
                size_t bi = fn % chunk;
                uint8_t bad = p[bi] ^ 0xffu;
                if (bi && !io->write_all(io, p, bi)) return -1;
                if (!io->write_all(io, &bad, 1)) return -1;
                if (bi + 1 < chunk && !io->write_all(io, p + bi + 1, chunk - bi - 1)) return -1;
            } else if (!io->write_all(io, p, chunk)) {
                return -1;
            }
        }
        p += chunk;
        len -= chunk;
    } while (len);
    return 0;
}
