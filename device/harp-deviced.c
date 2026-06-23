/* harp-deviced — HARP reference device daemon (spec draft 0.3).
 *
 * Implements the device side of `harp-core` + `harp-recall` over a TCP dev
 * transport (the framed link of §4.2 over a socket). On the Pi 4B the same
 * daemon will later speak FunctionFS bulk endpoints; the link layer is
 * fd-based so only the accept path changes.
 *
 * NOTE (§4.4): TCP here is a development transport for the simulator and for
 * Linux boards (KR260), not a conformance-bearing `harp` network binding.
 *
 * The "engine" is a bank of named parameters — enough to make state real:
 * knob turns dirty the live ref, snapshots serialize it, refsets restore it.
 *
 * Implementation choices where the spec is silent (candidate HEP notes):
 *   - state.want responds {0: count-of-objects-queued} before sending.
 *   - refset closure validation covers root/tree/blob reachability but does
 *     NOT require snapshot parent ancestry to be present (§15.3 "full
 *     closure" would otherwise mean unbounded history in every bundle).
 */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define HARP_CLOSESOCK(fd) closesocket(fd)
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <pthread.h> /* POSIX pthreads (Windows gets the shim via device.h) */
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
#  include <unistd.h>
#  include <fcntl.h>
#  define HARP_CLOSESOCK(fd) close(fd)
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "rtp.h"
#include "sock_io.h" /* harp_sock_io: recv/send accept path (Winsock SOCKETs reject _read/_write) */

/* THE device. One per daemon; modules share it via device.h. */
device g_dev;

/* On a clean shutdown, unbind the gadget UDC so the USB host sees a real disconnect.
 * A bare daemon restart leaves the UDC bound, so the host — especially the CI rig's
 * PCI-passthrough VM — keeps a stale claim that wedges the next claim; unbinding forces
 * a fresh re-enumeration. Armed only in FFS mode (g_udc_path stays empty in --port, so
 * the handler is a no-op there). Uses only async-signal-safe calls (open/write/_exit).
 * See scripts/hw-tests-linux.sh recover(). */
#ifndef _WIN32
static char g_udc_path[256];
static pid_t g_mdns_pid = 0; /* §4.4.3: the _harp._tcp advertiser child, 0 = none */
static void on_term(int sig) {
    (void)sig;
    if (g_mdns_pid > 0) kill(g_mdns_pid, SIGTERM); /* §4.4.3: stop advertising -> mDNS goodbye */
    if (g_udc_path[0]) {
        int fd = open(g_udc_path, O_WRONLY);
        if (fd >= 0) {
            ssize_t w = write(fd, "\n", 1); /* empty UDC name -> unbind (host disconnect) */
            (void)w;
            close(fd);
        }
    }
    _exit(0);
}

/* §4.4.3: advertise this device as `_harp._tcp` so hosts discover it without a hardcoded
 * address. The TXT record carries `proto` (major.minor) + the framed-link `port` and MUST
 * NOT carry the serial (§16 privacy — identity is fetched via core.hello). The instance
 * name is advisory. Discovery-only: the host may still dial directly. Best-effort via the
 * platform mDNS responder (avahi on Linux, dns-sd on macOS); a missing responder simply
 * means no advertisement. The child is killed on shutdown (on_term) so the responder emits
 * the mDNS goodbye record (§12.3 detach signal). */
static void mdns_advertise(int port) {
    char ports[16], txtproto[24], txtport[24];
    snprintf(ports, sizeof ports, "%d", port);
    snprintf(txtproto, sizeof txtproto, "proto=%d.%d", PROTO_MAJOR, PROTO_MINOR);
    snprintf(txtport, sizeof txtport, "port=%d", port);
    signal(SIGCHLD, SIG_IGN); /* auto-reap the advertiser (no waitpid/popen on the --port path) */
    pid_t pid = fork();
    if (pid < 0) return; /* best-effort */
    if (pid == 0) {
        int n = open("/dev/null", O_RDWR);
        if (n >= 0) { dup2(n, 1); dup2(n, 2); }
#ifdef __APPLE__
        execlp("dns-sd", "dns-sd", "-R", "HARP refdev", "_harp._tcp", "local", ports,
               txtproto, txtport, (char *)NULL);
#else
        execlp("avahi-publish-service", "avahi-publish-service", "HARP refdev", "_harp._tcp",
               ports, txtproto, txtport, (char *)NULL);
#endif
        _exit(127); /* responder tool absent */
    }
    g_mdns_pid = pid;
}
#endif /* !_WIN32 — FFS UDC unbind is Linux gadget only; Windows CI exits via TerminateProcess */

#ifdef __linux__
/* FunctionFS gadget transport (device/ffs.c) */
int harp_ffs_serve(const char *ffs_dir, const char *gadget_path,
                   void (*session)(void *ud, harp_io *io), void *ud);

static void ffs_session_cb(void *ud, harp_io *io) {
    harp_deviced_run_session(ud, io);
    fprintf(stderr, "harp-deviced: usb session ended; awaiting reattach\n");
}
#endif

static uint64_t bump_boot_count(const char *dir) {
    char path[600];
    snprintf(path, sizeof path, "%s/bootcount", dir);
    uint64_t n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%llu", (unsigned long long *)&n) != 1) n = 0;
        fclose(f);
    }
    n++;
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%llu\n", (unsigned long long)n);
        fclose(f);
    }
    return n;
}


/* §8.7: send one rendered block as an RTP/UDP datagram (header + samples via
 * iovec, no copy). No-op unless --rtp-out armed this socket. Called from the
 * engine's free-running render loop; sockets live here, not in engine.c. */
/* §8.7 fault injection: --drop-rtp-pct N deterministically drops ~N% of outgoing RTP
 * datagrams (0 = off), to exercise the host's free-running RTP loss tolerance. */
static int g_rtp_drop_pct = 0;
/* §8.7 fault injection: --corrupt-ctl-pct N flips one byte in ~N% of outgoing FRAMED CBOR
 * (ctl + echo + evt, via harp_link_send), set per-io on the device's host link only. */
static int g_corrupt_ctl_pct = 0;

void audio_rtp_emit(audio_state *a, const float *samples, size_t payload_bytes, uint64_t msc) {
    if (a->rtp_fd < 0) return;
    uint16_t seq = a->rtp_seq++;
    /* drop ~N% of datagrams, but ADVANCE the seq first so the host sees a genuine gap
     * (real loss), not a stall. Knuth multiplicative hash = reproducible (no rand) yet
     * well-spread across the sequence. */
    if (g_rtp_drop_pct > 0 && (seq * 2654435761u) % 100u < (uint32_t)g_rtp_drop_pct) return;
    uint8_t hdr[HARP_RTP_HDR_BYTES];
    harp_rtp_hdr h = {96, 0, seq, (uint32_t)msc, a->rtp_ssrc};
    harp_rtp_pack(hdr, sizeof hdr, &h, NULL, 0);
#ifdef _WIN32
    /* WSASend with a 2-element WSABUF gathers header+samples into ONE datagram on a
     * connected UDP socket — the Winsock equivalent of sendmsg's 2-iovec gather. */
    WSABUF bufs[2] = {{(ULONG)HARP_RTP_HDR_BYTES, (char *)hdr},
                      {(ULONG)payload_bytes, (char *)samples}};
    DWORD sent = 0;
    (void)WSASend((SOCKET)a->rtp_fd, bufs, 2, &sent, 0, NULL, NULL);
#else
    struct iovec iov[2] = {{hdr, HARP_RTP_HDR_BYTES}, {(void *)samples, payload_bytes}};
    struct msghdr m = {0};
    m.msg_iov = iov;
    m.msg_iovlen = 2;
    (void)sendmsg(a->rtp_fd, &m, 0);
#endif
}

/* §8.7: open the negotiated RTP audio destination — a UDP socket connect()'d to
 * the TCP peer's IP (peer_ip_net, network order) on the host-chosen `port`
 * (audio.start key 6). connect() lets the render loop sendmsg() with no per-call
 * address. Returns the fd, or -1 on bad args / socket failure. */
int audio_open_rtp_dest(uint32_t peer_ip_net, int port) {
    if (peer_ip_net == 0 || port <= 0 || port > 65535) return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = peer_ip_net;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        HARP_CLOSESOCK(fd);
        return -1;
    }
    return fd;
}

/* §8.3-over-§8.7: open the negotiated HOST-PACED audio channel — a TCP socket
 * connect()'d to the TCP peer's IP on the host-chosen port (audio.start key 7).
 * TCP (lossless, ordered) is required because host-paced delivery must be byte-
 * exact (unlike free-running RTP, which may drop). TCP_NODELAY so a faster-than-
 * real-time offline pull isn't Nagle-delayed. Returns the fd, or -1 on failure.
 * Called from the AUDIO thread (engine.c), never the session thread. */
int audio_open_tcp_paced(uint32_t peer_ip_net, int port) {
    if (peer_ip_net == 0 || port <= 0 || port > 65535) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = peer_ip_net;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        HARP_CLOSESOCK(fd);
        return -1;
    }
    return fd;
}

/* Close a negotiated RTP destination (no-op if none / on USB). Called from
 * audio_stop once the render thread is joined, so the fd outlives no sender. */
void audio_rtp_close(audio_state *a) {
    if (a->rtp_fd >= 0) {
        HARP_CLOSESOCK(a->rtp_fd);
        a->rtp_fd = -1;
    }
}

/* Standalone §8.7 emit mode (--rtp-out HOST:PORT): free-running mono main-mix
 * over RTP/UDP, paced by the device clock. The real engine, no host needed —
 * the device side of the network audio plane. Blocks until killed. */
static int run_rtp_out(device *d, const char *hostport) {
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport) { fprintf(stderr, "--rtp-out needs HOST:PORT\n"); return 2; }
    char host[256];
    size_t hl = (size_t)(colon - hostport);
    if (hl >= sizeof host) return 2;
    memcpy(host, hostport, hl); host[hl] = 0;
    int rport = atoi(colon + 1);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)rport);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            fprintf(stderr, "--rtp-out: cannot resolve %s\n", host); return 1;
        }
        a.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { fprintf(stderr, "--rtp-out: connect failed\n"); return 1; }

    audio_state *au = &d->audio;
    au->fd = au->out_fd = -1;             /* no framed transport — RTP only       */
    au->rtp_fd = fd; au->rtp_ssrc = 0x48415250u /* "HARP" */; au->rtp_seq = 0;
    au->mode = 0; au->rate = 48000; au->nsamples = 256; au->epoch = 1;
    au->out_slots[0] = 0; au->n_out_slots = 1;   /* mono: main-mix slot 0         */
    atomic_store_explicit(&au->running, true, memory_order_relaxed);
    fprintf(stderr, "harp-deviced: §8.7 RTP emit -> %s:%d (48k mono, free-running)\n", host, rport);
    audio_thread(d);                       /* render+emit in this thread until killed */
    return 0;
}

int main(int argc, char **argv) {
    const char *state_dir = "./refdev-state";
    const char *serial = "SIM-0001";
    const char *ffs_dir = NULL;
    const char *gadget = "/sys/kernel/config/usb_gadget/harp";
    int port = 47800;
    const char *panel_sock = "/tmp/harp-panel.sock"; /* "" disables */
    const char *rtp_out = NULL;                       /* §8.7 emit dest HOST:PORT */
    double tone_hz = 0.0;                             /* --tone HZ: SINAD test tone, 0=engine */
    bool no_rate_lock = false;                        /* --no-rate-lock: force host ASRC fallback */
    uint32_t rt_floor = 0;                            /* --rt-floor N: declared safe ethTargetFrames floor (frames), identity key 14 */
    uint32_t rt_nsamples = 0;                         /* --rt-nsamples N: declared RTP packet size (frames), identity key 14 sub-key 1 */
    bool pmh_flip = false;                            /* --param-map-hash-flip: TEST seam (§9.3/§13.4) —
                                                       * advertise a 1-bit-altered param-map-hash to mimic
                                                       * an engine-update param-map change, so the shell's
                                                       * recall-drift WARNING can be exercised in CI */
    bool mdns = false; /* --mdns: §4.4.3 advertise _harp._tcp (off by default; the eth-suite
                        * dials directly and the simulator shouldn't pollute the local segment) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc)
            state_dir = argv[++i];
        else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc)
            serial = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ffs") == 0 && i + 1 < argc)
            ffs_dir = argv[++i];
        else if (strcmp(argv[i], "--gadget") == 0 && i + 1 < argc)
            gadget = argv[++i];
        else if (strcmp(argv[i], "--panel-sock") == 0 && i + 1 < argc)
            panel_sock = argv[++i];
        else if (strcmp(argv[i], "--rtp-out") == 0 && i + 1 < argc)
            rtp_out = argv[++i];
        else if (strcmp(argv[i], "--drop-rtp-pct") == 0 && i + 1 < argc)
            g_rtp_drop_pct = atoi(argv[++i]); /* §8.7 fault injection: drop ~N% of RTP */
        else if (strcmp(argv[i], "--corrupt-ctl-pct") == 0 && i + 1 < argc)
            g_corrupt_ctl_pct = atoi(argv[++i]); /* §8.7 fault injection: flip ~N% of framed CBOR */
        else if (strcmp(argv[i], "--tone") == 0 && i + 1 < argc)
            tone_hz = atof(argv[++i]);
        else if (strcmp(argv[i], "--no-rate-lock") == 0)
            no_rate_lock = true; /* §8.7: drop audio.rate-lock from hello → host ASRC path */
        else if (strcmp(argv[i], "--param-map-hash-flip") == 0)
            pmh_flip = true; /* §13.4 test seam: advertise a drifted param-map-hash */
        else if (strcmp(argv[i], "--mdns") == 0)
            mdns = true; /* §4.4.3: advertise _harp._tcp (--port mode; avahi on Linux, dns-sd on macOS) */
        else if (strcmp(argv[i], "--rt-floor") == 0 && i + 1 < argc)
            rt_floor = (uint32_t)atoi(argv[++i]); /* §6.4: declare the safe ethTargetFrames floor (identity key 14) */
        else if (strcmp(argv[i], "--rt-nsamples") == 0 && i + 1 < argc)
            rt_nsamples = (uint32_t)atoi(argv[++i]); /* §6.4: declare the RTP packet size (identity key 14 sub-key 1) */
        else {
            fprintf(stderr,
                    "usage: harp-deviced [--state-dir DIR] [--serial S] "
                    "[--panel-sock PATH] [--tone HZ] [--no-rate-lock] [--rt-floor N] [--rt-nsamples N] "
                    "[--port P | --ffs FFS_DIR [--gadget CONFIGFS_PATH]]\n");
            return 2;
        }
    }
#ifdef _WIN32
    { WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
          fprintf(stderr, "harp-deviced: WSAStartup failed\n");
          return 1;
      } }
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term); /* clean shutdown unbinds the UDC (FFS mode) -> host re-enumerates */
    signal(SIGINT, on_term);
#endif

    device *d = &g_dev;
    memset(d, 0, sizeof *d);
    d->io = NULL;
    d->audio.tone_hz = tone_hz; /* test/measurement tone (render_output); 0 = engine */
    d->audio.epoch = 1; /* §7.1: the device clock epoch starts at 1 — epoch 0 is reserved as the
                         * (0,0)="now" event-timestamp sentinel, so a live epoch is never 0. */
    d->no_rate_lock = no_rate_lock; /* §8.7 ASRC fallback test hook (hello capability gate) */
    /* §6.4 rt-profile: a device's safe RT setpoints are device- AND link-specific, so they are
     * supplied per-deployment via --rt-floor / --rt-nsamples (its on-device service config), not
     * hardcoded here. Absent => 0 => the host keeps its conservative 2048/256 defaults. */
    d->rt_floor = rt_floor;       /* §6.4 rt-profile: emitted as identity key 14 sub-key 0 when nonzero */
    d->rt_nsamples = rt_nsamples; /* §6.4 rt-profile: emitted as identity key 14 sub-key 1 when nonzero */
    snprintf(d->serial, sizeof d->serial, "%s", serial);
    if (harp_store_open(&d->store, state_dir) != 0) {
        fprintf(stderr, "harp-deviced: cannot open state dir %s\n", state_dir);
        return 1;
    }
    d->boot_count = bump_boot_count(state_dir);
    compute_param_map_hash(d);
    if (pmh_flip) d->param_map_hash.b[1] ^= 0x01; /* §13.4 test seam: drift the advertised hash.
        Flip a DIGEST byte, not the algorithm byte b[0]: a real param-map change keeps alg=0x01,
        and §10.2 now rejects an unknown algorithm byte at parse, so the host must be able to read
        the identity to observe the drift. */
    pthread_mutex_init(&d->send_mu, NULL);
    pthread_mutex_init(&d->state_mu, NULL);

#ifndef _WIN32
    if (panel_sock[0]) {
        static struct panel_args pa;
        pa.d = d;
        pa.path = panel_sock;
        pthread_t pt;
        pthread_create(&pt, NULL, panel_main, &pa);
        pthread_detach(pt);
    }
#else
    (void)panel_sock; /* AF_UNIX front panel is POSIX-only; gated out of the Windows build */
#endif

    /* Recall across power cycles: load the live ref if clean; first boot
     * snapshots the factory state so the ref is born. */
    harp_ref live;
    if (harp_store_ref_read(&d->store, LIVE_REF, &live) == 0) {
        if (live.unborn) {
            harp_hash snap;
            if (do_snapshot(d, "factory state", &snap, NULL) == 0)
                fprintf(stderr, "harp-deviced: initialized factory state\n");
        } else if (!live.dirty) {
            if (engine_load_snapshot(d, &live.hash) == 0)
                fprintf(stderr, "harp-deviced: restored live/project (gen %llu)\n",
                        (unsigned long long)live.generation);
        }
    }

    if (rtp_out)
        return run_rtp_out(d, rtp_out);

    if (ffs_dir) {
#ifdef __linux__
        snprintf(g_udc_path, sizeof g_udc_path, "%s/UDC", gadget); /* arm the shutdown unbind */
        fprintf(stderr, "harp-deviced: serial %s, state %s, USB gadget via %s (boot %llu)\n",
                d->serial, state_dir, ffs_dir, (unsigned long long)d->boot_count);
        return harp_ffs_serve(ffs_dir, gadget, ffs_session_cb, d);
#else
        (void)gadget;
        fprintf(stderr, "harp-deviced: --ffs requires Linux\n");
        return 2;
#endif
    }

    harp_sockhandle sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(sfd, 4) != 0) {
        fprintf(stderr, "harp-deviced: cannot listen on port %d: %s\n", port,
                strerror(errno));
        return 1;
    }
    fprintf(stderr, "harp-deviced: serial %s, state %s, listening on %d (boot %llu)\n",
            d->serial, state_dir, port, (unsigned long long)d->boot_count);
#ifndef _WIN32
    if (mdns) mdns_advertise(port); /* §4.4.3: advertise _harp._tcp now the port is bound */
#else
    (void)mdns; /* mDNS advertise is POSIX-only (Windows is host-side, not a refdev target) */
#endif

    for (;;) {
        harp_sockhandle cfd = accept(sfd, NULL, NULL);
        if (cfd == HARP_SOCK_INVALID) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        /* §8.7: remember the peer's IP so a TCP-session audio.start can negotiate
         * an RTP audio destination (its host:port = peer-IP + key-6 port). */
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        d->rtp_peer_ip = (getpeername(cfd, (struct sockaddr *)&peer, &plen) == 0 &&
                          peer.sin_family == AF_INET)
                             ? peer.sin_addr.s_addr
                             : 0;
#ifdef _WIN32
        /* Winsock SOCKETs reject _read/_write, so the accept path uses recv/send. */
        harp_sock_io tio;
        harp_sock_io_init(&tio, cfd);
#else
        harp_io_fd tio;
        harp_io_fd_init(&tio, cfd, cfd);
#endif
        tio.io.corrupt_pct = g_corrupt_ctl_pct; /* §8.7 fault injection: device->host frames only */
        harp_deviced_run_session(d, &tio.io);
        d->rtp_peer_ip = 0; /* session over — forget the peer */
        HARP_CLOSESOCK(cfd);
        fprintf(stderr, "harp-deviced: session ended; awaiting reattach\n");
    }
    return 0;
}
