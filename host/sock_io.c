/* See sock_io.h. harp_io over a TCP socket, lifted from harp-probe.c's sock_io
 * (recv/send for Winsock portability) but with a non-fatal dial() for the
 * plugin runtime. */
#include "sock_io.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

static bool sock_read_exact(harp_io *io, void *buf, size_t n) {
    harp_sock_io *t = (harp_sock_io *)io;
    uint8_t *p = buf;
    while (n) {
        int r = recv(t->s, (char *)p, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
        if (r < 0) {
#ifdef _WIN32
            int werr = WSAGetLastError();
            if (werr == WSAEINTR) continue;
            fprintf(stderr, "harp: sock_read_exact recv ERROR r=%d WSA=%d (still needed %zu)\n", r, werr, n);
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        if (r == 0) {
#ifdef _WIN32
            fprintf(stderr, "harp: sock_read_exact recv EOF r=0 (peer closed, still needed %zu)\n", n);
#endif
            return false; /* peer closed */
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool sock_write_all(harp_io *io, const void *buf, size_t n) {
    harp_sock_io *t = (harp_sock_io *)io;
    const uint8_t *p = buf;
    while (n) {
        int r = send(t->s, (const char *)p, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
        if (r < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

void harp_sock_io_init(harp_sock_io *t, harp_sockhandle s) {
    t->io.read_exact = sock_read_exact;
    t->io.write_all = sock_write_all;
    t->io.corrupt_pct = 0; /* §8.7 fault injection is DEVICE-only; init so stack garbage never makes the host corrupt its own frames */
    t->s = s;
}

harp_sockhandle harp_sock_dial(const char *hostport) {
    char host[256];
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport) {
        fprintf(stderr, "sock_dial: device address must be HOST:PORT\n");
        return HARP_SOCK_INVALID;
    }
    size_t hl = (size_t)(colon - hostport);
    if (hl >= sizeof host) {
        fprintf(stderr, "sock_dial: host too long\n");
        return HARP_SOCK_INVALID;
    }
    memcpy(host, hostport, hl);
    host[hl] = 0;

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || !res) {
        fprintf(stderr, "sock_dial: cannot resolve %s\n", host);
        return HARP_SOCK_INVALID;
    }
    harp_sockhandle fd = HARP_SOCK_INVALID;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == HARP_SOCK_INVALID) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        harp_sock_close(fd);
        fd = HARP_SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (fd == HARP_SOCK_INVALID) {
        fprintf(stderr, "sock_dial: cannot connect to %s\n", hostport);
        return HARP_SOCK_INVALID;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return fd;
}

void harp_sock_close(harp_sockhandle s) {
    if (s == HARP_SOCK_INVALID) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}
