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
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>

#include "device.h"
#include "rtp.h"

/* THE device. One per daemon; modules share it via device.h. */
device g_dev;

/* On a clean shutdown, unbind the gadget UDC so the USB host sees a real disconnect.
 * A bare daemon restart leaves the UDC bound, so the host — especially the CI rig's
 * PCI-passthrough VM — keeps a stale claim that wedges the next claim; unbinding forces
 * a fresh re-enumeration. Armed only in FFS mode (g_udc_path stays empty in --port, so
 * the handler is a no-op there). Uses only async-signal-safe calls (open/write/_exit).
 * See scripts/hw-tests-linux.sh recover(). */
static char g_udc_path[256];
static void on_term(int sig) {
    (void)sig;
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
void audio_rtp_emit(audio_state *a, const float *samples, size_t payload_bytes, uint64_t msc) {
    if (a->rtp_fd < 0) return;
    uint8_t hdr[HARP_RTP_HDR_BYTES];
    harp_rtp_hdr h = {96, 0, a->rtp_seq++, (uint32_t)msc, a->rtp_ssrc};
    harp_rtp_pack(hdr, sizeof hdr, &h, NULL, 0);
    struct iovec iov[2] = {{hdr, HARP_RTP_HDR_BYTES}, {(void *)samples, payload_bytes}};
    struct msghdr m = {0};
    m.msg_iov = iov;
    m.msg_iovlen = 2;
    (void)sendmsg(a->rtp_fd, &m, 0);
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
        close(fd);
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
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = peer_ip_net;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Close a negotiated RTP destination (no-op if none / on USB). Called from
 * audio_stop once the render thread is joined, so the fd outlives no sender. */
void audio_rtp_close(audio_state *a) {
    if (a->rtp_fd >= 0) {
        close(a->rtp_fd);
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
        else if (strcmp(argv[i], "--tone") == 0 && i + 1 < argc)
            tone_hz = atof(argv[++i]);
        else if (strcmp(argv[i], "--no-rate-lock") == 0)
            no_rate_lock = true; /* §8.7: drop audio.rate-lock from hello → host ASRC path */
        else {
            fprintf(stderr,
                    "usage: harp-deviced [--state-dir DIR] [--serial S] "
                    "[--panel-sock PATH] [--tone HZ] [--no-rate-lock] "
                    "[--port P | --ffs FFS_DIR [--gadget CONFIGFS_PATH]]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term); /* clean shutdown unbinds the UDC (FFS mode) -> host re-enumerates */
    signal(SIGINT, on_term);

    device *d = &g_dev;
    memset(d, 0, sizeof *d);
    d->io = NULL;
    d->audio.tone_hz = tone_hz; /* test/measurement tone (render_output); 0 = engine */
    d->no_rate_lock = no_rate_lock; /* §8.7 ASRC fallback test hook (hello capability gate) */
    snprintf(d->serial, sizeof d->serial, "%s", serial);
    if (harp_store_open(&d->store, state_dir) != 0) {
        fprintf(stderr, "harp-deviced: cannot open state dir %s\n", state_dir);
        return 1;
    }
    d->boot_count = bump_boot_count(state_dir);
    compute_param_map_hash(d);
    pthread_mutex_init(&d->send_mu, NULL);
    pthread_mutex_init(&d->state_mu, NULL);

    if (panel_sock[0]) {
        static struct panel_args pa;
        pa.d = d;
        pa.path = panel_sock;
        pthread_t pt;
        pthread_create(&pt, NULL, panel_main, &pa);
        pthread_detach(pt);
    }

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

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
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

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        /* §8.7: remember the peer's IP so a TCP-session audio.start can negotiate
         * an RTP audio destination (its host:port = peer-IP + key-6 port). */
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        d->rtp_peer_ip = (getpeername(cfd, (struct sockaddr *)&peer, &plen) == 0 &&
                          peer.sin_family == AF_INET)
                             ? peer.sin_addr.s_addr
                             : 0;
        harp_io_fd tio;
        harp_io_fd_init(&tio, cfd, cfd);
        harp_deviced_run_session(d, &tio.io);
        d->rtp_peer_ip = 0; /* session over — forget the peer */
        close(cfd);
        fprintf(stderr, "harp-deviced: session ended; awaiting reattach\n");
    }
    return 0;
}
