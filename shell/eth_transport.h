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
 */
#ifndef HARP_SHELL_ETH_TRANSPORT_H
#define HARP_SHELL_ETH_TRANSPORT_H

#include "transport.h"

extern "C" {
#include "rtp.h"
#include "sock_io.h"
}

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "harp/plat.h" /* monotonic clock for silentMs liveness */

struct EthTransport final : ShellTransport {
    /* Dial HOST:PORT (TCP ctl) and bind an ephemeral UDP port for the RTP audio.
     * Returns nullptr on any failure (a plugin must reconnect, never die). */
    static EthTransport *dial(const char *hostport) {
        harp_sockhandle ctl = harp_sock_dial(hostport);
        if (ctl == HARP_SOCK_INVALID) return nullptr;
        int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (rx < 0) { harp_sock_close(ctl); return nullptr; }
        struct sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = 0; /* ephemeral */
        if (::bind(rx, (struct sockaddr *)&a, sizeof a) != 0) {
            ::close(rx); harp_sock_close(ctl); return nullptr;
        }
        socklen_t al = sizeof a;
        int port = (::getsockname(rx, (struct sockaddr *)&a, &al) == 0) ? ntohs(a.sin_port) : 0;
        return new EthTransport(ctl, rx, port);
    }

    ~EthTransport() override {
        if (rxsock_ >= 0) ::close(rxsock_);
        harp_sock_close(ctl_.s);
    }

    EthTransport(const EthTransport &) = delete;
    EthTransport &operator=(const EthTransport &) = delete;

    /* ---- control plane (TCP framed link) ---- */
    harp_io *ctlIo() override { return &ctl_.io; }
    bool linkPoll(unsigned ms) override {
        struct pollfd p = {ctl_.s, POLLIN, 0};
        return ::poll(&p, 1, (int)ms) > 0 && (p.revents & POLLIN);
    }
    size_t linkPending() override {
        struct pollfd p = {ctl_.s, POLLIN, 0};
        return (::poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) ? 1 : 0;
    }

    /* ---- audio: free-running (RTP), not the host-paced endpoint ---- */
    bool hasAudio() override { return true; }
    int audioRead(void *, int, unsigned) override { return 0; }  /* N/A: audio is RTP */
    bool audioWrite(const void *, int, unsigned) override { return true; } /* no pacing writes */

    bool identity(harp_usb_devinfo *out) override {
        if (!out) return false;
        out->vendor_id = 0;
        out->product_id = 0;
        snprintf(out->serial, sizeof out->serial, "eth:%s:%d", peer_.c_str(), rxport_);
        return true;
    }
    bool isFreeRunning() const override { return true; }
    int  audioPort() const override { return rxport_; } /* audio.start key 6 */

    /* Reader thread: receive ONE RTP packet's stereo frames into `out` (up to
     * maxFrames), waiting up to timeout_ms. Returns frames received (0 = none /
     * timeout / malformed). The caller (reader) writes them into audioRing_. */
    unsigned recvAudio(float *out, unsigned maxFrames, int timeout_ms) override {
        struct pollfd p = {rxsock_, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return 0;
        uint8_t buf[16384];
        ssize_t n = ::recv(rxsock_, buf, sizeof buf, 0);
        if (n < 0) return 0;
        harp_rtp_hdr h;
        const uint8_t *pl;
        size_t pln;
        if (harp_rtp_unpack(buf, (size_t)n, &h, &pl, &pln) != 0) return 0;
        if (pln % (2 * sizeof(float))) return 0; /* stereo frames only */
        unsigned f = (unsigned)(pln / (2 * sizeof(float)));
        if (f > maxFrames) f = maxFrames; /* one packet always fits a sane out */
        memcpy(out, pl, (size_t)f * 2 * sizeof(float));
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
    EthTransport(harp_sockhandle ctl, int rx, int port) : rxsock_(rx), rxport_(port) {
        harp_sock_io_init(&ctl_, ctl);
        struct sockaddr_in pa = {};
        socklen_t pl = sizeof pa;
        if (::getpeername(ctl, (struct sockaddr *)&pa, &pl) == 0) {
            char b[64];
            if (inet_ntop(AF_INET, &pa.sin_addr, b, sizeof b)) peer_ = b;
        }
    }

    harp_sock_io ctl_;
    int          rxsock_ = -1;
    int          rxport_ = 0;
    std::string  peer_;
    std::atomic<unsigned long long> lastArr_{0};
};

#endif /* HARP_SHELL_ETH_TRANSPORT_H */
