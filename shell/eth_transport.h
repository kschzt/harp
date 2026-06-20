/* shell/eth_transport.h — the §8.7 Ethernet binding as a ShellTransport.
 *
 * The control plane is a TCP framed link (harp_sock_io, hello/state/events); the
 * audio plane is RTP/UDP. This is the BIT-EXACT (host-locked) path: the device
 * free-runs and emits its stereo main mix over RTP; the host plays it 1:1 (no
 * resampling = bit-exact), and the runtime's feeder closes the loop with an
 * audio.trim rate correction so the device emits at exactly the host's
 * consumption rate. Jitter then only moves the buffer; it cannot distort the
 * samples. (Validated by tools/eth-bitexact-test: 127 dB, kria→Mac.)
 *
 * IMPORTANT — no audio-thread access to this object: recvAudio() is driven by
 * the runtime's READER thread, which writes the frames into the runtime's stable
 * audioRing_. The DAW audio thread reads audioRing_ (exactly as on USB) and never
 * touches transport_, so reaping this transport on a reconnect can't race the
 * audio thread (sessionDown joins the reader before delete, like USB). This
 * object therefore owns NO threads of its own — just the two sockets.
 *
 * PORTABILITY: a DAW host may be Windows, so the UDP audio socket uses the same
 * Winsock-portable handle type (harp_sockhandle) and close (harp_sock_close) as
 * the TCP control plane (sock_io), recv() not read(), and WSAPoll()/poll() behind
 * one helper. winsock2.h arrives via sock_io.h; WSAStartup is the runtime's job
 * (done once at construction, before any dial()).
 */
#ifndef HARP_SHELL_ETH_TRANSPORT_H
#define HARP_SHELL_ETH_TRANSPORT_H

#include "transport.h"

extern "C" {
#include "rtp.h"
#include "sock_io.h" /* harp_sockhandle, HARP_SOCK_INVALID; pulls winsock2.h on _WIN32 */
}

#ifdef _WIN32
#  include <ws2tcpip.h> /* inet_ntop, socklen_t (winsock2.h already in via sock_io.h) */
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h> /* TCP_NODELAY for the host-paced audio socket */
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex> /* one-shot accept of the device's host-paced connect-back */
#include <string>

/* Suppress SIGPIPE on a send to a peer that closed: MSG_NOSIGNAL on Linux,
 * SO_NOSIGPIPE on the accepted socket on macOS/BSD. A plugin must never take a
 * signal from a dead device. */
#ifdef MSG_NOSIGNAL
#  define HARP_ETH_SENDFLAGS MSG_NOSIGNAL
#else
#  define HARP_ETH_SENDFLAGS 0
#endif

#include "harp/plat.h" /* monotonic clock for silentMs liveness */

struct EthTransport final : ShellTransport {
    /* Dial HOST:PORT (TCP ctl). The AUDIO plane depends on the mode:
     *  - free-running (live, hostPaced=false): bind an ephemeral UDP port for the
     *    RTP audio (the low-latency, non-deterministic bit-exact playback path).
     *  - host-paced (offline bounce, hostPaced=true): LISTEN on an ephemeral TCP
     *    port for the device's connect-back (audio.start key 7), and run NO RTP —
     *    the device renders exact SSI ranges on demand over TCP (deterministic).
     * Returns nullptr on any failure (a plugin must reconnect, never die). */
    static EthTransport *dial(const char *hostport, bool hostPaced) {
        harp_sockhandle ctl = harp_sock_dial(hostport);
        if (ctl == HARP_SOCK_INVALID) return nullptr;
        struct sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = 0; /* ephemeral */
        socklen_t al = sizeof a;
        if (hostPaced) {
            harp_sockhandle ls = ::socket(AF_INET, SOCK_STREAM, 0);
            if (ls == HARP_SOCK_INVALID) { harp_sock_close(ctl); return nullptr; }
            if (::bind(ls, (struct sockaddr *)&a, (socklen_t)sizeof a) != 0 ||
                ::listen(ls, 1) != 0) {
                harp_sock_close(ls); harp_sock_close(ctl); return nullptr;
            }
            int port = (::getsockname(ls, (struct sockaddr *)&a, &al) == 0) ? ntohs(a.sin_port) : 0;
            return new EthTransport(ctl, HARP_SOCK_INVALID, 0, true, ls, port);
        }
        harp_sockhandle rx = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (rx == HARP_SOCK_INVALID) { harp_sock_close(ctl); return nullptr; }
        if (::bind(rx, (struct sockaddr *)&a, (socklen_t)sizeof a) != 0) {
            harp_sock_close(rx); harp_sock_close(ctl); return nullptr;
        }
        int port = (::getsockname(rx, (struct sockaddr *)&a, &al) == 0) ? ntohs(a.sin_port) : 0;
        return new EthTransport(ctl, rx, port, false, HARP_SOCK_INVALID, 0);
    }

    ~EthTransport() override {
        fprintf(stderr, "harp-shell: ~EthTransport dtor hostPaced=%d audioSock=%lld listen=%lld — closing\n",
                (int)hostPaced_, (long long)audioSock_, (long long)audioListen_);
        if (audioSock_ != HARP_SOCK_INVALID) harp_sock_close(audioSock_);
        if (audioListen_ != HARP_SOCK_INVALID) harp_sock_close(audioListen_);
        if (rxsock_ != HARP_SOCK_INVALID) harp_sock_close(rxsock_);
        harp_sock_close(ctl_.s);
    }

    EthTransport(const EthTransport &) = delete;
    EthTransport &operator=(const EthTransport &) = delete;

    /* ---- control plane (TCP framed link) ---- */
    harp_io *ctlIo() override { return &ctl_.io; }
    bool linkPoll(unsigned ms) override { return readable(ctl_.s, (int)ms); }
    size_t linkPending() override { return readable(ctl_.s, 0) ? 1 : 0; }

    /* ---- audio ----
     * free-running: RTP (recvAudio); audioRead/audioWrite stay no-op stubs exactly
     *   as before, so the proven 127 dB live path is untouched.
     * host-paced: the SAME raw harp_audio frames the USB FFS endpoints carry, over
     *   the accepted TCP audio socket — so the runtime's host-paced feeder + reader
     *   tail + pullAudioBlocking run verbatim (only the byte carrier changed). */
    bool hasAudio() override { return true; }
    int audioRead(void *buf, int len, unsigned ms) override {
        if (!hostPaced_) return 0; /* free-running: audio is RTP (recvAudio), not this */
        if (!ensureAudioAccepted(ms)) return 0; /* device not connected yet => no data (not dead) */
        if (!readable(audioSock_, (int)ms)) return 0; /* timeout, no data */
        int n = (int)::recv(audioSock_, (char *)buf, len, 0);
        if (n <= 0) {
#ifdef _WIN32
            static int rd = 0; if (rd < 3) { rd++; fprintf(stderr, "harp-shell: audioRead recv=%d WSA=%d\n", n, n < 0 ? WSAGetLastError() : 0); }
#endif
            return -1; /* 0 = peer closed, <0 = error => device gone */
        }
        lastArr_.store(harp_now_ns(), std::memory_order_relaxed);
        return n;
    }
    bool audioWrite(const void *buf, int len, unsigned ms) override {
        if (!hostPaced_) return true; /* free-running: no pacing writes */
        if (!ensureAudioAccepted(ms)) {
            static bool warned = false;
            if (!warned) { warned = true; fprintf(stderr, "harp-shell: host-paced pacing deferred — device connect-back not yet accepted\n"); }
            return false; /* not connected yet; feeder retries */
        }
        const char *p = (const char *)buf;
        int left = len;
        while (left > 0) {
            int n = (int)::send(audioSock_, p, left, HARP_ETH_SENDFLAGS);
#ifdef _WIN32
            { static int wr = 0; if (wr < 3) { wr++; fprintf(stderr, "harp-shell: audioWrite send=%d/%d WSA=%d\n", n, left, n <= 0 ? WSAGetLastError() : 0); } }
#endif
            if (n <= 0) return false;
            p += n;
            left -= n;
        }
        return true;
    }

    bool identity(harp_usb_devinfo *out) override {
        if (!out) return false;
        out->vendor_id = 0;
        out->product_id = 0;
        snprintf(out->serial, sizeof out->serial, "eth:%s:%d", peer_.c_str(), rxport_);
        return true;
    }
    bool isFreeRunning() const override { return !hostPaced_; }
    int  audioPort() const override { return rxport_; }        /* audio.start key 6 (RTP) */
    int  audioPort7() const override { return audioListenPort_; } /* key 7 (host-paced TCP) */

    /* Reader thread: receive ONE RTP packet's slot-interleaved samples into `out`
     * (up to maxFloats), waiting up to timeout_ms. Returns FLOATS received (0 = none
     * / timeout / malformed). The payload is nsamples x slots (the negotiated union);
     * the caller splits it by the union width and demuxes per part. dev_ts (if
     * non-null) gets the packet's 32-bit RTP timestamp = the device sample index,
     * which the ASRC clock-recovery (host/freerun) regresses against arrival time. */
    unsigned recvAudio(float *out, unsigned maxFloats, int timeout_ms, unsigned *dev_ts) override {
        if (!readable(rxsock_, timeout_ms)) return 0;
        int n = (int)::recv(rxsock_, (char *)rxpkt_, (int)sizeof rxpkt_, 0);
        if (n < 0) return 0;
        harp_rtp_hdr h;
        const uint8_t *pl;
        size_t pln;
        if (harp_rtp_unpack(rxpkt_, (size_t)n, &h, &pl, &pln) != 0) return 0;
        if (dev_ts) *dev_ts = h.timestamp;
        if (pln % sizeof(float)) return 0; /* whole float samples only */
        unsigned f = (unsigned)(pln / sizeof(float)); /* slot-interleaved floats */
        if (f > maxFloats) f = maxFloats; /* one packet always fits a sane out */
        memcpy(out, pl, (size_t)f * sizeof(float));
        lastArr_.store(harp_now_ns(), std::memory_order_relaxed);
        return f;
    }
    unsigned silentMs() const override {
        unsigned long long last = lastArr_.load(std::memory_order_relaxed);
        if (!last) return 0; /* no packet yet — startup, not a stall */
        unsigned long long now = harp_now_ns();
        return now > last ? (unsigned)((now - last) / 1000000ull) : 0;
    }

private:
    EthTransport(harp_sockhandle ctl, harp_sockhandle rx, int port,
                 bool hostPaced, harp_sockhandle alisten, int aport)
        : rxsock_(rx), rxport_(port), hostPaced_(hostPaced),
          audioListen_(alisten), audioListenPort_(aport) {
        harp_sock_io_init(&ctl_, ctl);
        struct sockaddr_in pa = {};
        socklen_t pl = sizeof pa;
        if (::getpeername(ctl, (struct sockaddr *)&pa, &pl) == 0) {
            char b[64];
            if (inet_ntop(AF_INET, &pa.sin_addr, b, sizeof b)) peer_ = b;
        }
    }

    /* host-paced only: accept the device's ONE connect-back into audioSock_, once.
     * Idempotent + thread-safe — the sessionUp drain (supervisor thread) normally
     * wins the accept before the reader/feeder spawn, but the mutex covers the case
     * where a slow connect leaves it to the reader/feeder. Returns true once the
     * audio socket is live. TCP_NODELAY + (BSD/macOS) SO_NOSIGPIPE on the accepted fd. */
    bool ensureAudioAccepted(unsigned ms) {
        if (audioSock_ != HARP_SOCK_INVALID) return true;
        std::lock_guard<std::mutex> lk(acceptMu_);
        if (audioSock_ != HARP_SOCK_INVALID) return true;       /* another thread won */
        if (audioListen_ == HARP_SOCK_INVALID) return false;
        if (!readable(audioListen_, (int)ms)) return false;     /* device not connected yet */
        harp_sockhandle s = ::accept(audioListen_, nullptr, nullptr);
        if (s == HARP_SOCK_INVALID) return false;
        int one = 1;
        ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
        ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
        audioSock_ = s;
#ifdef _WIN32
        { struct sockaddr_in la = {}, pa = {}; socklen_t ll = sizeof la, pl = sizeof pa;
          ::getsockname(s, (struct sockaddr *)&la, &ll); ::getpeername(s, (struct sockaddr *)&pa, &pl);
          fprintf(stderr, "harp-shell: accepted audioSock=%lld local=:%d peer=:%d\n",
                  (long long)s, ntohs(la.sin_port), ntohs(pa.sin_port)); }
#endif
        fprintf(stderr, "harp-shell: host-paced audio connect-back accepted (key 7)\n");
        return true;
    }

    /* one-fd readability poll, portable across POSIX poll() and Winsock WSAPoll().
     * "Readable" INCLUDES the error/hangup conditions (POLLERR/POLLHUP/POLLNVAL):
     * a reset or half-closed peer must let the subsequent recv() RUN so it returns
     * 0/-1 and the caller sees "device gone" — NOT silently report "no data, still
     * alive", which on Windows (WSAPoll signals a RST as POLLERR/POLLHUP, never
     * POLLRDNORM) would spin the reader forever on a dead host-paced socket. */
    static bool readable(harp_sockhandle s, int timeout_ms) {
#ifdef _WIN32
        WSAPOLLFD p;
        p.fd = s;
        p.events = POLLRDNORM;
        p.revents = 0;
        return WSAPoll(&p, 1, timeout_ms) > 0 &&
               (p.revents & (POLLRDNORM | POLLHUP | POLLERR | POLLNVAL));
#else
        struct pollfd p = {s, POLLIN, 0};
        return ::poll(&p, 1, timeout_ms) > 0 &&
               (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL));
#endif
    }

    harp_sock_io    ctl_;
    harp_sockhandle rxsock_ = HARP_SOCK_INVALID;
    int             rxport_ = 0;
    std::string     peer_;
    std::atomic<unsigned long long> lastArr_{0};
    /* §8.3-over-§8.7 host-paced: live ONLY when hostPaced_. audioListen_ is the
     * ephemeral TCP listener (key 7) the device dials back; audioSock_ is the one
     * accepted connection carrying H->D pacing + D->H rendered frames. */
    bool            hostPaced_ = false;
    harp_sockhandle audioListen_ = HARP_SOCK_INVALID;
    int             audioListenPort_ = 0;
    harp_sockhandle audioSock_ = HARP_SOCK_INVALID;
    std::mutex      acceptMu_;
    /* one RTP datagram: the widest union is nsamples(256) x 34 slots x 4B ≈ 34 KB,
     * so 64 KB covers it (incl. the RTP header) with margin. Reader-thread only. */
    uint8_t rxpkt_[65536];
};

#endif /* HARP_SHELL_ETH_TRANSPORT_H */
