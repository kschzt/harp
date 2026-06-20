/* shell/transport.h — the shell's audio+control transport abstraction.
 *
 * HarpRuntime drives a device through ONE of two bindings: the USB binding
 * (§4.3, host-paced) or the Ethernet/RTP binding (§8.7, device free-running).
 * Both expose the same control plane (a framed harp_io for hello/state/events)
 * and the same audio endpoints (read the device's render frames, write pacing
 * frames). This interface is that common shape; UsbTransport and (next step)
 * EthTransport implement it. The runtime holds a ShellTransport* and never
 * names a concrete binding outside selectDevice().
 *
 * Step 1 (this file's introduction) is a pure refactor: the USB path is lifted
 * VERBATIM behind UsbTransport, so the golden render stays byte-identical. The
 * free-running hooks below are stubs on USB and carry the documented thread
 * affinity the RTP path needs (review B3/M2): pollFree on the network/rx
 * thread, pullFree on the DAW audio thread.
 */
#ifndef HARP_SHELL_TRANSPORT_H
#define HARP_SHELL_TRANSPORT_H

extern "C" {
#include "harp/link.h" /* harp_io */
#include "usb_io.h"    /* harp_usb_devinfo (the vid:pid:serial identity) */
}

struct ShellTransport {
    virtual ~ShellTransport() = default; /* delete == close: frees the binding */

    /* ---- control plane: the framed harp_io the client/link layer talks
     * through (hello, state, the event wire). Stable for the transport's life. */
    virtual harp_io *ctlIo() = 0;

    /* ---- async inbound link drain (pollEcho): one short fill, then "are bytes
     * pending?". 1:1 with the §4.2.1 always-pending inbound read. */
    virtual bool   linkPoll(unsigned timeout_ms) = 0;
    virtual size_t linkPending() = 0;

    /* ---- audio endpoints (§8.2/§8.3). Host-paced bindings read render frames
     * and write pacing frames; a free-running binding's audioWrite is a no-op
     * and its audio arrives via pollFree/pullFree instead. */
    virtual bool hasAudio() = 0;
    virtual int  audioRead(void *buf, int len, unsigned timeout_ms) = 0;
    virtual bool audioWrite(const void *buf, int len, unsigned timeout_ms) = 0;

    /* ---- bound device identity (vid:pid:serial), read back after open. */
    virtual bool identity(harp_usb_devinfo *out) = 0;

    /* §14.4 host-context-C — the binding KIND, the decisive selector the
     * diag-bundle assembler uses to emit key 10 (usb-topology, USB ONLY) vs key
     * 13 (net-topology, Ethernet ONLY) and audio-config key 12 (transport enum).
     * isFreeRunning() cannot distinguish them: a host-paced Ethernet binding
     * (offline bounce) is also non-free-running. USB returns 0, Ethernet 1
     * (matching the design CDDL `transport` enum). */
    enum class Kind { Usb = 0, Ethernet = 1 };
    virtual Kind kind() const = 0;

    /* §14.4 host-context-C — USB topology of the bound device (diag-bundle key
     * 10). USB fills it from libusb; the base (and Ethernet) leaves out->ok false
     * so the assembler omits key 10. Read-only, off the control path. */
    virtual bool usbTopology(harp_usb_topology *out) {
        if (out) out->ok = false;
        return false;
    }

    /* §14.4 host-context-C — the resolved Ethernet peer (diag-bundle key 13.0,
     * "host:port"). Non-empty ONLY for an Ethernet binding; "" on USB so the
     * assembler omits key 13. Read-only. */
    virtual const char *netEndpoint() const { return ""; }

    /* ---- binding mode. USB is host-paced (false); Ethernet/RTP is device
     * free-running (true). Cached into HarpRuntime::freeRunning_ at sessionUp
     * so the audio thread never pays a virtual call to branch on it. */
    virtual bool isFreeRunning() const = 0;

    /* ---- free-running audio plane (§8.7), used only when isFreeRunning().
     * recvAudio() receives ONE RTP packet's worth of slot-interleaved samples into
     * `out` (up to maxFloats), waiting up to timeout_ms; returns the FLOATS received
     * (0 = none). The payload is `nsamples x slots` (the audio.start key-4 union — a
     * single instance is the {0,1} stereo main mix, wider with per-part sinks); the
     * reader splits it by the negotiated union width and demuxes per part. Called
     * ONLY on the runtime's reader thread (NOT the DAW audio thread), which writes
     * the demuxed frames into the stable audioRing_ / per-part sink rings — so the
     * audio thread reads those exactly as on USB and never touches transport_ (no
     * use-after-free when the supervisor reaps the transport on a reconnect; the
     * reader is joined first, like USB). audioPort() is the bound RTP rx port
     * (audio.start key 6); silentMs() is ms since the last packet, so the reader can
     * declare a dead RTP stream (no EOF) and trigger reconnect. USB stubs all three
     * (it never free-runs; its audio is the host-paced endpoint via audioRead). The
     * bit-exact RATE LOOP lives in the feeder and reads audioRing_ occupancy. */
    virtual unsigned recvAudio(float *out, unsigned maxFloats, int timeout_ms, unsigned *dev_ts) {
        (void)out; (void)maxFloats; (void)timeout_ms; (void)dev_ts; return 0;
    }
    virtual int      audioPort() const { return 0; }
    virtual unsigned silentMs() const { return 0; }

    /* §8.3-over-§8.7 host-paced (deterministic offline bounce): the host's TCP
     * audio-listen port (audio.start key 7) that the device connect()s back to.
     * Non-zero ONLY for a host-paced EthTransport (isFreeRunning()==false AND an
     * Ethernet binding); 0 on USB (host-paced over FFS endpoints, no key 7) and on
     * a free-running EthTransport. When non-zero, audioStart emits key 7 and NOT
     * key 6, and audioRead/audioWrite carry the host-paced frames over TCP. */
    virtual int      audioPort7() const { return 0; }
};

#endif /* HARP_SHELL_TRANSPORT_H */
