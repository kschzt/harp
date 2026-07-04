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
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

static bool sock_read_exact(harp_io *io, void *buf, size_t n) {
    harp_sock_io *t = (harp_sock_io *)io;
    uint8_t *p = buf;
    while (n) {
        int r = recv(t->s, (char *)p, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
        if (r < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        if (r == 0) return false; /* peer closed */
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

/* Non-blocking readiness (harp_io.readable): true iff a recv would return immediately
 * (data queued in the socket buffer), via a 0-timeout select — the same primitive
 * connect_bounded() already uses here, so it is portable to the MinGW device (Winsock
 * select). The device event-consume loop calls this ONLY when it has staged events, to
 * decide whether to keep batching the incoming EVT run or flush now; it never blocks and
 * a false result flushes at once (latency-neutral). One cheap syscall per staged message
 * under a flood, amortised against the evq lock it saves (the lock is contended with the
 * render thread, so cutting its acquisitions is the win). EOF also reads as readable — the
 * loop then recv's, gets the disconnect, and the post-loop flush drains the stage. */
static bool sock_readable(harp_io *io) {
    harp_sock_io *t = (harp_sock_io *)io;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(t->s, &rf);
    struct timeval tv = {0, 0}; /* poll: return immediately */
    return select((int)t->s + 1, &rf, NULL, NULL, &tv) > 0 && FD_ISSET(t->s, &rf);
}

void harp_sock_io_init(harp_sock_io *t, harp_sockhandle s) {
    t->io.read_exact = sock_read_exact;
    t->io.write_all = sock_write_all;
    t->io.corrupt_pct = 0; /* §8.7 fault injection is DEVICE-only; init so stack garbage never makes the host corrupt its own frames */
    t->io.readable = sock_readable; /* §consume batching: 0-timeout select readiness (device event-consume loop) */
    t->s = s;
    t->deadline_ns = 0; /* §16: no deadline by default; the device arms it pre-hello (harp-deviced.c) */
}

/* Bounded TCP connect. A stale/unreachable HOST:PORT (e.g. a device that still
 * advertises over mDNS but is powered down) must never hang the caller: a plain
 * blocking connect() to an unreachable host waits the ~75 s OS default, which —
 * on the shell's connect path — would stall a DAW. Do a non-blocking connect and
 * select() for writability within timeout_ms; restore blocking for the framed
 * link's recv/send. Returns 0 on a completed connect, -1 on failure/timeout. */
static int connect_bounded(harp_sockhandle fd, const struct sockaddr *addr,
                           socklen_t addrlen, int timeout_ms) {
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
    int done = 0;
    if (connect(fd, addr, (int)addrlen) == 0) {
        done = 1; /* immediate (e.g. loopback) */
    } else {
#ifdef _WIN32
        int inprog = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        int inprog = (errno == EINPROGRESS);
#endif
        if (inprog) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            if (select((int)fd + 1, NULL, &wf, NULL, &tv) > 0 && FD_ISSET(fd, &wf)) {
                int err = 0;
                socklen_t el = (socklen_t)sizeof err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el) == 0 && err == 0)
                    done = 1; /* connection completed cleanly */
            }
        }
    }
#ifdef _WIN32
    nb = 0;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    if (fl >= 0) fcntl(fd, F_SETFL, fl);
#endif
    return done ? 0 : -1;
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
        if (connect_bounded(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen, 2000) == 0) break;
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
