/* host/sock_io.h — harp_io over a TCP socket (the §8.7 Ethernet control plane).
 *
 * Lifted from harp-probe.c's sock_io: recv/send (not read/write) so it works on
 * Winsock SOCKETs as well as POSIX fds — a DAW host may be Windows, where
 * read()/write() do not operate on sockets. HarpRuntime uses this for the
 * framed link (hello / state / the event wire) to an Ethernet-bound device; the
 * audio plane is RTP/UDP (rtp_udp.h), free-running and entirely separate.
 *
 * Unlike the probe, harp_sock_dial() RETURNS an error instead of exiting — a
 * plugin must fail the connect attempt and let the supervisor reconnect, never
 * die(). On Windows the caller is responsible for WSAStartup() before dialing
 * (the runtime does it once at construction); POSIX needs no such setup.
 */
#ifndef HARP_SOCK_IO_H
#define HARP_SOCK_IO_H

#include <stdbool.h>

#include "harp/link.h" /* harp_io */

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
typedef SOCKET harp_sockhandle;
#  define HARP_SOCK_INVALID INVALID_SOCKET
#else
typedef int harp_sockhandle;
#  define HARP_SOCK_INVALID (-1)
#endif

/* A harp_io whose read_exact/write_all go over a TCP socket. `io` MUST be the
 * first member: the link/client layer holds a harp_io*, and the read/write
 * callbacks recover the socket by casting it straight back to harp_sock_io*. */
typedef struct {
    harp_io         io;
    harp_sockhandle s;
} harp_sock_io;

/* Bind the read_exact/write_all vtable onto an already-connected socket. */
void harp_sock_io_init(harp_sock_io *t, harp_sockhandle s);

/* Resolve and connect to "HOST:PORT" over TCP (IPv4, TCP_NODELAY for low-latency
 * control traffic). Returns the connected socket, or HARP_SOCK_INVALID on any
 * failure (a one-line reason on stderr). Never exits. */
harp_sockhandle harp_sock_dial(const char *hostport);

/* Close a socket returned by harp_sock_dial (closesocket on Windows). */
void harp_sock_close(harp_sockhandle s);

#endif /* HARP_SOCK_IO_H */
