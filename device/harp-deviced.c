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
#  include <poll.h> /* graceful (coverage) accept wait — poll() bounds the wait portably */
#  include <pthread.h> /* POSIX pthreads (Windows gets the shim via device.h) */
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/uio.h>
#  include <unistd.h>
#  include <fcntl.h>
#  define HARP_CLOSESOCK(fd) close(fd)
#endif
#include <errno.h>
#include <signal.h> /* sig_atomic_t for the clean-shutdown flag (the POSIX block re-includes it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "rtp.h"
#include "sock_io.h" /* harp_sock_io: recv/send accept path (Winsock SOCKETs reject _read/_write) */
#include "harp/plat.h" /* harp_now_ns: the §16 pre-hello read deadline */
#include "conn_ratelimit.h" /* §16(b): harp_peer_penalized / harp_peer_penalize */

/* THE device. One per daemon; modules share it via device.h. */
device g_dev;

/* Clean-shutdown / coverage seam. When HARP_CLEAN_EXIT is set in the environment (the CI coverage
 * lane only — scripts/device-coverage-drive.sh), SIGTERM/SIGINT ask the eth (--port) accept loop to
 * RETURN from main() rather than take the immediate _exit(0) in the signal handler, so a --coverage
 * build runs its normal atexit teardown and FLUSHES the per-TU .gcda (a _exit() in a signal handler
 * does not, so a SIGKILLed/SIGTERMed daemon otherwise yields ZERO device-line coverage). Production
 * leaves HARP_CLEAN_EXIT unset, so device shutdown is byte-for-byte the existing immediate-exit path
 * off the coverage lane. Set/observed only on the POSIX --port path (the FFS gadget still unbinds the
 * UDC + _exit()s; Windows CI stops the daemon via TerminateProcess; the coverage build is Linux). */
static int g_graceful_shutdown = 0;             /* HARP_CLEAN_EXIT set -> accept loop returns cleanly */
static volatile sig_atomic_t g_accept_stop = 0; /* set by on_term to break the eth accept loop */

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
        _exit(0); /* FFS gadget: async-signal-safe unbind done; exit now (UDC unbind is HW-critical) */
    }
    if (g_graceful_shutdown) {
        g_accept_stop = 1; /* eth --port + HARP_CLEAN_EXIT: let the accept loop return -> gcov flush */
        return;
    }
    _exit(0); /* default (production): immediate exit, unchanged */
}

/* §4.4.3: advertise this device as `_harp._tcp` so hosts discover it without a hardcoded
 * address. The TXT record carries `proto` (major.minor) + the framed-link `port` and MUST
 * NOT carry the serial (§16 privacy — identity is fetched via core.hello). The instance
 * name is advisory. Discovery-only: the host may still dial directly. Best-effort via the
 * platform mDNS responder (avahi on Linux, dns-sd on macOS); a missing responder simply
 * means no advertisement. The child is killed on shutdown (on_term) so the responder emits
 * the mDNS goodbye record (§12.3 detach signal). */
static void mdns_advertise(int port, const char *name) {
    char ports[16], txtproto[24], txtport[24];
    if (!name || !name[0]) name = "HARP refdev"; /* default instance name (the refdev's) */
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
        execlp("dns-sd", "dns-sd", "-R", name, "_harp._tcp", "local", ports,
               txtproto, txtport, (char *)NULL);
#else
        execlp("avahi-publish-service", "avahi-publish-service", name, "_harp._tcp",
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
    ((device *)ud)->ctl_sock = HARP_SOCK_INVALID; /* §16: USB/FFS has no eth control socket */
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
/* §8.7 fault injection: --reorder-rtp-pct N holds ~N% of outgoing RTP datagrams one deep and sends
 * each AFTER the next, so the host sees seq N+1 before seq N (a genuine out-of-order arrival, not
 * loss) — exercising the host's reorder handling (it must not rewind its high-water seq). */
static int g_rtp_reorder_pct = 0;
/* §8.7 fault injection: --corrupt-ctl-pct N flips one byte in ~N% of outgoing FRAMED CBOR
 * (ctl + echo + evt, via harp_link_send), set per-io on the device's host link only. */
static int g_corrupt_ctl_pct = 0;
/* §16(b) rate-limit TEST SEAM ONLY: the per-peer pre-hello shed/penalize path keys on the peer's
 * IP and EXEMPTS loopback (127.*), but every DoS test connects from 127.0.0.1 — so the penalize/
 * penalized branch is otherwise unreachable in test. --force-peer-ip A.B.C.D (or env HARP_FORCE_PEER_IP)
 * makes the accept loop treat the connecting peer as that IP for the RATE-LIMIT DECISION ONLY (the
 * harp_peer_penalize / harp_peer_penalized key), so a non-loopback value actually executes the shed
 * path under test. NOT for production use — the real RTP destination still uses the true peer IP.
 * Network-order (matching getpeername's sin_addr.s_addr); 0 = off / unset = the real behavior. */
static uint32_t g_force_peer_ip = 0;

/* §16 DoS: a deadline-bounded read_exact for the eth control socket's PRE-HELLO phase. Re-arms
 * SO_RCVTIMEO to the REMAINING budget (harp_sock_io.deadline_ns) before each recv, so a slow-
 * trickle half-open (a byte just under the per-recv inactivity timeout — which alone resets on
 * every byte) cannot hold the single-threaded accept loop past the wall-clock deadline: the
 * timeout shrinks to 0 at the deadline and the next recv fails. deadline_ns == 0 (cleared on
 * core.hello) reads normally. Overrides harp_sock_io's standard read_exact; the duplicated recv
 * loop keeps harp_now_ns out of the host-shared sock_io.c. */
static bool device_prehello_read_exact(harp_io *io, void *buf, size_t n) {
    harp_sock_io *t = (harp_sock_io *)io;
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        if (t->deadline_ns) {
            uint64_t now = harp_now_ns();
            if (now >= t->deadline_ns) return false; /* pre-hello budget exhausted */
            uint64_t rem_ms = (t->deadline_ns - now) / 1000000ull;
            if (rem_ms > 5000) rem_ms = 5000; /* §16: 5s inactivity cap WITHIN the 10s total — a
                                               * stalled half-open drops at 5s, a trickle at the 10s deadline */
            harp_sock_recv_timeout_ms(t->s, (int)(rem_ms ? rem_ms : 1));
        }
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

/* Send one RTP packet (header + samples) as a single datagram (the 2-buffer gather is platform-specific). */
static void rtp_send_one(audio_state *a, const harp_rtp_hdr *h, const float *samples, size_t payload_bytes) {
    uint8_t hdr[HARP_RTP_HDR_BYTES];
    harp_rtp_pack(hdr, sizeof hdr, h, NULL, 0);
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

void audio_rtp_emit(audio_state *a, const float *samples, size_t payload_bytes, uint64_t msc) {
    if (a->rtp_fd < 0) return;
    /* WIDE UNION (>8 slots): one ns×S×4 datagram would exceed the OS max UDP datagram
     * (macOS net.inet.udp.maxdgram = 9216), so the send silently fails and the host gets
     * NOTHING. Split the frame into <=8-slot pt=97 groups, each a 4-byte sub-header
     * [off, cnt, total, flags] + ns×cnt gathered floats, all sharing the RTP timestamp; the
     * host reassembles by timestamp (see rtp.h). <=8 slots stays the byte-identical pt=96
     * path below. The render thread is the SOLE caller, so the gather buffer is static. */
    unsigned S = a->n_out_slots ? a->n_out_slots : 2;
    if (S > HARP_RTP_MAX_GROUP_SLOTS) {
        unsigned ns = (unsigned)(payload_bytes / ((size_t)S * sizeof(float)));
        static uint8_t grp[HARP_RTP_GROUP_HDR_BYTES +
                           AUDIO_MAX_NSAMPLES * HARP_RTP_MAX_GROUP_SLOTS * sizeof(float)];
        for (unsigned off = 0; off < S; off += HARP_RTP_MAX_GROUP_SLOTS) {
            unsigned cnt = (S - off > HARP_RTP_MAX_GROUP_SLOTS) ? HARP_RTP_MAX_GROUP_SLOTS : (S - off);
            uint16_t gseq = a->rtp_seq++;
            /* §8.7 test loss injection still applies per packet; ADVANCE seq first (drop = gap). */
            if (g_rtp_drop_pct > 0 && (gseq * 2654435761u) % 100u < (uint32_t)g_rtp_drop_pct) continue;
            grp[0] = (uint8_t)off; grp[1] = (uint8_t)cnt; grp[2] = (uint8_t)S; grp[3] = 0;
            float *gf = (float *)(grp + HARP_RTP_GROUP_HDR_BYTES);
            for (unsigned s = 0; s < ns; s++)
                for (unsigned c = 0; c < cnt; c++)
                    gf[(size_t)s * cnt + c] = samples[(size_t)s * S + off + c];
            harp_rtp_hdr gh = {HARP_RTP_PT_GROUP, 0, gseq, (uint32_t)msc, a->rtp_ssrc};
            rtp_send_one(a, &gh, (const float *)grp,
                         HARP_RTP_GROUP_HDR_BYTES + (size_t)ns * cnt * sizeof(float));
        }
        return;
    }
    uint16_t seq = a->rtp_seq++;
    /* drop ~N% of datagrams, but ADVANCE the seq first so the host sees a genuine gap
     * (real loss), not a stall. Knuth multiplicative hash = reproducible (no rand) yet
     * well-spread across the sequence. */
    if (g_rtp_drop_pct > 0 && (seq * 2654435761u) % 100u < (uint32_t)g_rtp_drop_pct) return;
    harp_rtp_hdr h = {96, 0, seq, (uint32_t)msc, a->rtp_ssrc};
    /* §8.7 reorder injection (test only): hold this packet ONE deep and send it AFTER the next, so
     * the host sees seq N+1 before seq N (out-of-order arrival, not loss). The render thread is the
     * sole caller, so a static one-deep hold (float-aligned for the sample copy) is safe. */
    static harp_rtp_hdr held_h;
    static float held_pl[2048];
    static size_t held_n = 0;
    static int held_countdown = 0; /* >0: a packet is held, released after this many later packets */
    if (held_countdown > 0) {
        rtp_send_one(a, &h, samples, payload_bytes);   /* the in-order stream keeps flowing */
        if (--held_countdown == 0)                      /* release the held packet — now well out of order */
            rtp_send_one(a, &held_h, held_pl, held_n);
        return;
    }
    if (g_rtp_reorder_pct > 0 && payload_bytes <= sizeof held_pl &&
        (seq * 2654435761u) % 100u < (uint32_t)g_rtp_reorder_pct) {
        held_h = h;
        memcpy(held_pl, samples, payload_bytes);
        held_n = payload_bytes;
        held_countdown = 3; /* release after 3 later packets — a deep enough rewind to expose the bug */
        return;
    }
    rtp_send_one(a, &h, samples, payload_bytes);
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
    uint32_t in_lat = 0, out_lat = 0;                 /* --in-lat / --out-lat N: §6.4 latency-profile keys 1/2 (converter latency, samples) */
    const char *engine_ver = NULL;                    /* --engine-ver X.Y.Z: §12.2 test seam, override reported engine semver */
    const char *product = NULL;                       /* --product STRING: identity product/model + panel + mDNS instance name (NULL => harp-refdev) */
    const char *engine_name = NULL;                   /* --engine-name STRING: identity engine name (NULL => ENGINE_ID; media-type unaffected) */
    bool pmh_flip = false;                            /* --param-map-hash-flip: TEST seam (§9.3/§13.4) —
                                                       * advertise a 1-bit-altered param-map-hash to mimic
                                                       * an engine-update param-map change, so the shell's
                                                       * recall-drift WARNING can be exercised in CI */
    bool mdns = false; /* --mdns: §4.4.3 advertise _harp._tcp (off by default; the eth-suite
                        * dials directly and the simulator shouldn't pollute the local segment) */
    /* §16(b) rate-limit TEST SEAM ONLY: env HARP_FORCE_PEER_IP is the no-argv route (e.g. when the
     * test can't reach the daemon's command line); an explicit --force-peer-ip below still wins. */
    {
        const char *fenv = getenv("HARP_FORCE_PEER_IP");
        struct in_addr fa;
        if (fenv && *fenv && inet_pton(AF_INET, fenv, &fa) == 1) g_force_peer_ip = fa.s_addr;
    }
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
        else if (strcmp(argv[i], "--reorder-rtp-pct") == 0 && i + 1 < argc)
            g_rtp_reorder_pct = atoi(argv[++i]); /* §8.7 fault injection: reorder ~N% of RTP */
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
        else if (strcmp(argv[i], "--in-lat") == 0 && i + 1 < argc)
            in_lat = (uint32_t)atoi(argv[++i]);  /* §6.4: declare analog-in path latency (latency-profile key 1) */
        else if (strcmp(argv[i], "--out-lat") == 0 && i + 1 < argc)
            out_lat = (uint32_t)atoi(argv[++i]); /* §6.4: declare analog-out path latency (latency-profile key 2) */
        else if (strcmp(argv[i], "--engine-ver") == 0 && i + 1 < argc)
            engine_ver = argv[++i]; /* §12.2 test seam: report this engine semver instead of ENGINE_VERSION */
        else if (strcmp(argv[i], "--force-peer-ip") == 0 && i + 1 < argc) {
            /* §16(b) rate-limit TEST SEAM ONLY: see g_force_peer_ip — force the rate-limit
             * decision to treat the peer as this (non-loopback) IP so the shed path runs. */
            struct in_addr fa;
            if (inet_pton(AF_INET, argv[++i], &fa) == 1) g_force_peer_ip = fa.s_addr;
        }
        else if (strcmp(argv[i], "--product") == 0 && i + 1 < argc)
            product = argv[++i]; /* identity product/model + panel product + mDNS instance name */
        else if (strcmp(argv[i], "--engine-name") == 0 && i + 1 < argc)
            engine_name = argv[++i]; /* identity engine name (PARAMS_MEDIA / recall format unaffected) */
        else {
            fprintf(stderr,
                    "usage: harp-deviced [--state-dir DIR] [--serial S] [--product NAME] [--engine-name NAME] "
                    "[--panel-sock PATH] [--tone HZ] [--no-rate-lock] [--rt-floor N] [--rt-nsamples N] [--in-lat N] [--out-lat N] [--engine-ver X.Y.Z] "
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
    /* Coverage lane only: ask the eth accept loop to return from main() on SIGTERM/SIGINT so a
     * --coverage build flushes its .gcda (see g_graceful_shutdown). Empty/unset => off (production). */
    {
        const char *ce = getenv("HARP_CLEAN_EXIT");
        g_graceful_shutdown = (ce && ce[0]) ? 1 : 0;
    }
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
    d->in_lat = in_lat;           /* §6.4 latency-profile key 1 (converter analog-in; 0 = pure-digital refdev) */
    d->out_lat = out_lat;         /* §6.4 latency-profile key 2 (converter analog-out) */
    d->engine_ver = engine_ver;   /* §12.2 test seam: NULL => ENGINE_VERSION */
    d->ctl_sock = HARP_SOCK_INVALID; /* §16: armed per-connection by the eth accept loop */
    d->product = product;         /* identity product/model + panel + mDNS name; NULL => "harp-refdev" */
    d->engine_name = engine_name; /* identity engine name; NULL => ENGINE_ID (media-type unaffected) */
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
    /* Bounded bind-retry on EADDRINUSE — rapid-restart robustness. A supervisor cycling the
     * daemon, or a just-killed predecessor whose listening socket the kernel has not finished
     * reaping, can briefly hold the port even with SO_REUSEADDR (that flag forgives a TIME_WAIT
     * socket, not a still-live one). Poll up to ~2 s so a clean restart binds the moment the port
     * frees; a genuine conflict (another daemon truly holding it) still fails, just after the window.
     * Normal start binds on the first attempt, so this adds no latency unless the port is contended. */
    int bound = 0;
    for (int attempt = 0; attempt < 40; attempt++) { /* 40 × 50 ms ≈ 2 s */
        if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) == 0) { bound = 1; break; }
#ifdef _WIN32
        if (WSAGetLastError() != WSAEADDRINUSE) break;
#else
        if (errno != EADDRINUSE) break;
#endif
        harp_sleep_ns(50000000ull); /* 50 ms */
    }
    if (!bound || listen(sfd, 4) != 0) {
        fprintf(stderr, "harp-deviced: cannot listen on port %d: %s\n", port,
                strerror(errno));
        return 1;
    }
    fprintf(stderr, "harp-deviced: serial %s, state %s, listening on %d (boot %llu)\n",
            d->serial, state_dir, port, (unsigned long long)d->boot_count);
#ifndef _WIN32
    if (mdns) mdns_advertise(port, d->product); /* §4.4.3: advertise _harp._tcp (instance name = --product, default "HARP refdev") */
#else
    (void)mdns; /* mDNS advertise is POSIX-only (Windows is host-side, not a refdev target) */
#endif

    /* §16 DoS: the half-open bound IS the per-connection time limit — a 5s pre-hello
     * SO_RCVTIMEO (armed below) plus a 10s total pre-hello deadline (run_session) drop a
     * connection that never completes core.hello, so no peer can tie up the single-threaded
     * accept loop indefinitely. (A global connection-COUNT rate limit was evaluated and
     * removed: a token bucket false-sheds a legitimate client arriving right after a burst —
     * e.g. the T9 abuse-test's post-flood recovery probe — while a flood of quick connect/
     * close is not a DoS, since each is accepted, handled, and closed immediately. For a
     * single-threaded daemon the per-connection TIME bound is what prevents the hang.) */
    for (;;) {
#ifndef _WIN32
        /* Coverage lane only: when HARP_CLEAN_EXIT is set, wait on the listen socket with poll() (a
         * 200 ms tick) instead of a parked accept(), so the signal handler need only set g_accept_stop
         * and the loop returns from main() within one tick — flushing .gcda. poll() is portable
         * (macOS + Linux); SO_RCVTIMEO does NOT bound accept() on Darwin, and shutdown()/close() to
         * unblock a parked accept() is not portable either. Off (a plain blocking accept, zero added
         * wakeups) in production. Guarded out of the Windows build (graceful mode is never armed there
         * — on_term is POSIX-only — and MinGW has WSAPoll, not poll()). */
        if (g_graceful_shutdown) {
            struct pollfd pfd;
            pfd.fd = sfd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 200);
            if (g_accept_stop) break;        /* clean shutdown requested -> return from main */
            if (pr <= 0) continue;           /* timeout or EINTR: loop back, re-check the stop flag */
        }
#endif
        harp_sockhandle cfd = accept(sfd, NULL, NULL);
        if (cfd == HARP_SOCK_INVALID) {
            if (g_accept_stop) break; /* coverage lane: clean shutdown requested -> return from main */
#ifndef _WIN32
            if (errno == EINTR) continue;
            /* a poll-readable connection that was reset before accept() (rare) -> keep serving */
            if (g_graceful_shutdown && (errno == EAGAIN || errno == EWOULDBLOCK ||
                                        errno == ECONNABORTED)) continue;
#endif
            break;
        }
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        /* §16 DoS: arm a 5s pre-hello recv timeout — a half-open peer that never sends
         * core.hello is dropped (harp_link_recv returns -1 on the timeout, run_session
         * breaks) instead of blocking the single-threaded accept loop forever. */
        d->ctl_sock = cfd;
        harp_sock_recv_timeout_ms(cfd, 5000);
        /* §8.7: remember the peer's IP so a TCP-session audio.start can negotiate
         * an RTP audio destination (its host:port = peer-IP + key-6 port). */
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        d->rtp_peer_ip = (getpeername(cfd, (struct sockaddr *)&peer, &plen) == 0 &&
                          peer.sin_family == AF_INET)
                             ? peer.sin_addr.s_addr
                             : 0;
        /* §16(b) rate-limit: a NON-loopback peer that just failed pre-hello (held the slot without
         * completing core.hello) is shed for ~2s — capping a serial slow-loris from one segment node
         * without false-shedding a hello-completing reconnect (the per-IP key is exact). Loopback (the
         * local refdev / host+device on one machine / CI) is trusted + never rate-limited, so the
         * shared 127.0.0.1 cannot confound the per-IP key. The first network-order byte is the first
         * octet (127.* = loopback), endian-independent. */
        /* §16(b): the rate-limit decision keys on `peer_ip`. In production this is the real peer IP
         * (d->rtp_peer_ip). TEST SEAM ONLY: g_force_peer_ip (--force-peer-ip / HARP_FORCE_PEER_IP)
         * substitutes a forced (non-loopback) IP HERE so a 127.0.0.1 DoS test exercises the shed/
         * penalize path; the real RTP destination above (d->rtp_peer_ip) is left untouched. */
        uint32_t peer_ip = g_force_peer_ip ? g_force_peer_ip : d->rtp_peer_ip;
        bool rl_peer = peer_ip && ((const unsigned char *)&peer_ip)[0] != 127u;
        if (rl_peer && harp_peer_penalized(d->prehello_penalty, 16, peer_ip, harp_now_ns())) {
            d->ctl_sock = HARP_SOCK_INVALID;
            d->rtp_peer_ip = 0;
            HARP_CLOSESOCK(cfd);
            continue;
        }
        /* §16 DoS: ONE recv/send io on both platforms (Winsock rejects _read/_write anyway), with a
         * deadline-bounded pre-hello read so a slow-trickle half-open can't hold the accept loop. */
        harp_sock_io tio;
        harp_sock_io_init(&tio, cfd);
        tio.io.read_exact = device_prehello_read_exact;   /* §16: deadline-aware pre-hello read */
        tio.deadline_ns = harp_now_ns() + 10000000000ull; /* §16: 10 s total pre-hello budget */
        d->ctl_io = &tio;
        tio.io.corrupt_pct = g_corrupt_ctl_pct; /* §8.7 fault injection: device->host frames only */
        harp_deviced_run_session(d, &tio.io);
        /* §16(b): penalize ONLY a connection that failed PRE-HELLO (a half-open / slow-loris) — a
         * hello-completing session is never shed, so a legitimate reconnect is always admitted. */
        if (rl_peer && !atomic_load_explicit(&d->hello_done, memory_order_acquire))
            harp_peer_penalize(d->prehello_penalty, 16, &d->prehello_penalty_idx, peer_ip,
                               harp_now_ns(), 2000000000ull);
        d->ctl_io = NULL;
        d->ctl_sock = HARP_SOCK_INVALID; /* §16: session over — disarm the pre-hello timeout */
        d->rtp_peer_ip = 0; /* session over — forget the peer */
        HARP_CLOSESOCK(cfd);
        fprintf(stderr, "harp-deviced: session ended; awaiting reattach\n");
    }
    HARP_CLOSESOCK(sfd); /* release the listen socket on a clean accept-loop exit (coverage lane) */
    if (g_accept_stop) fprintf(stderr, "harp-deviced: clean shutdown (HARP_CLEAN_EXIT) — flushing\n");
    return 0;
}
