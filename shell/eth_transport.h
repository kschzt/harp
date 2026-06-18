/* shell/eth_transport.h — the §8.7 Ethernet binding as a ShellTransport.
 *
 * The control plane is a TCP framed link (harp_sock_io, hello/state/events); the
 * audio plane is RTP/UDP, received into a jitter FIFO by a dedicated rx thread.
 * This is the BIT-EXACT (host-locked) path: pullFree() hands the DAW the device's
 * samples 1:1 — NO resampling, hence NO libsamplerate — and the runtime's feeder
 * closes the loop by reading fillFrames() and streaming an audio.trim rate
 * correction back over the framed link, so the device emits at exactly the host's
 * consumption rate. Jitter then only moves the FIFO fill (which the loop holds);
 * it cannot distort the samples. (Validated end-to-end by tools/eth-bitexact-test:
 * 127 dB, 0 glitch, kria→Mac.) ASRC free-running is the future fallback.
 *
 * Thread model (mirrors the proven topology): the rx thread is the sole FIFO
 * PRODUCER (recv→push); the DAW audio thread is the sole CONSUMER (pullFree). The
 * indices are release/acquire atomics — a real SPSC ring, same discipline as
 * host/freerun.c (TSan-clean there).
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
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "harp/plat.h" /* monotonic clock for silentMs liveness */

struct EthTransport final : ShellTransport {
    /* Dial HOST:PORT (TCP ctl) and bind an ephemeral UDP port for the RTP audio.
     * Returns nullptr on any failure (a plugin must reconnect, never die). Starts
     * the rx thread. */
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
        stop_.store(true, std::memory_order_release);
        if (rxThread_.joinable()) rxThread_.join();
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

    /* ---- bit-exact hooks ---- */
    int audioPort() const override { return rxport_; } /* audio.start key 6 */
    unsigned fillFrames() const override {
        return (unsigned)(head_.load(std::memory_order_acquire) -
                          tail_.load(std::memory_order_acquire));
    }
    /* DAW audio thread: pull n stereo frames 1:1 from the FIFO; silence-pad on a
     * (transient) underrun. NO resampling = bit-exact. */
    unsigned pullFree(float *out, unsigned n) override {
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        uint64_t head = head_.load(std::memory_order_acquire);
        unsigned avail = (unsigned)(head - tail);
        unsigned take = n < avail ? n : avail;
        for (unsigned i = 0; i < take; i++) {
            unsigned s = (unsigned)((tail + i) & (FIFO - 1));
            out[i * 2] = fifo_[s * 2];
            out[i * 2 + 1] = fifo_[s * 2 + 1];
        }
        for (unsigned i = take; i < n; i++) { out[i * 2] = 0; out[i * 2 + 1] = 0; }
        tail_.store(tail + take, std::memory_order_release);
        return take;
    }
    unsigned silentMs() const override {
        unsigned long long last = lastArr_.load(std::memory_order_relaxed);
        if (!last) return 0xFFFFFFFFu;
        unsigned long long now = harp_now_ns();
        return now > last ? (unsigned)((now - last) / 1000000ull) : 0;
    }

private:
    static constexpr unsigned FIFO = 1u << 16; /* 65536 stereo frames (~1.4 s @48k) */

    EthTransport(harp_sockhandle ctl, int rx, int port) : rxsock_(rx), rxport_(port) {
        harp_sock_io_init(&ctl_, ctl);
        struct sockaddr_in pa = {};
        socklen_t pl = sizeof pa;
        if (::getpeername(ctl, (struct sockaddr *)&pa, &pl) == 0) {
            char b[64];
            if (inet_ntop(AF_INET, &pa.sin_addr, b, sizeof b)) peer_ = b;
        }
        rxThread_ = std::thread([this] { rxLoop(); });
    }

    void rxLoop() {
        uint8_t buf[16384];
        while (!stop_.load(std::memory_order_acquire)) {
            struct pollfd p = {rxsock_, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            ssize_t n = ::recv(rxsock_, buf, sizeof buf, 0);
            if (n < 0) continue;
            harp_rtp_hdr h;
            const uint8_t *pl;
            size_t pln;
            if (harp_rtp_unpack(buf, (size_t)n, &h, &pl, &pln) != 0) continue;
            if (pln % (2 * sizeof(float))) continue; /* stereo frames only */
            lastArr_.store(harp_now_ns(), std::memory_order_relaxed);
            push((const float *)pl, (unsigned)(pln / (2 * sizeof(float))));
        }
    }
    /* rx thread (sole producer): publish frames; drop on overflow (the feeder's
     * trim keeps the fill centered, so overflow is a torn-session edge, not normal). */
    void push(const float *src, unsigned n) {
        uint64_t head = head_.load(std::memory_order_relaxed);
        uint64_t tail = tail_.load(std::memory_order_acquire);
        unsigned space = (unsigned)(FIFO - (head - tail));
        if (n > space) n = space;
        for (unsigned i = 0; i < n; i++) {
            unsigned s = (unsigned)((head + i) & (FIFO - 1));
            fifo_[s * 2] = src[i * 2];
            fifo_[s * 2 + 1] = src[i * 2 + 1];
        }
        head_.store(head + n, std::memory_order_release);
    }

    harp_sock_io ctl_;
    int          rxsock_ = -1;
    int          rxport_ = 0;
    std::string  peer_;
    std::thread  rxThread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> head_{0}, tail_{0}; /* SPSC: rx-thread head, audio-thread tail */
    std::atomic<unsigned long long> lastArr_{0};
    float fifo_[FIFO * 2]; /* interleaved stereo */
};

#endif /* HARP_SHELL_ETH_TRANSPORT_H */
