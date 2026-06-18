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

    /* ---- binding mode. USB is host-paced (false); Ethernet/RTP is device
     * free-running (true). Cached into HarpRuntime::freeRunning_ at sessionUp
     * so the audio thread never pays a virtual call to branch on it. */
    virtual bool isFreeRunning() const = 0;

    /* ---- free-running audio plane (§8.7), used only when isFreeRunning().
     * recvAudio() receives ONE RTP packet's worth of interleaved stereo frames
     * into `out` (up to maxFrames), waiting up to timeout_ms; returns the frames
     * received (0 = none). It is called ONLY on the runtime's reader thread
     * (NOT the DAW audio thread), which writes them into the stable audioRing_ —
     * so the audio thread reads audioRing_ exactly as on USB and never touches
     * transport_ (no use-after-free when the supervisor reaps the transport on a
     * reconnect; the reader is joined first, like USB). audioPort() is the bound
     * RTP rx port (audio.start key 6); silentMs() is ms since the last packet,
     * so the reader can declare a dead RTP stream (no EOF) and trigger reconnect.
     * USB stubs all three (it never free-runs; its audio is the host-paced
     * endpoint via audioRead). The bit-exact RATE LOOP lives in the feeder and
     * reads audioRing_ occupancy directly. */
    virtual unsigned recvAudio(float *out, unsigned maxFrames, int timeout_ms) {
        (void)out; (void)maxFrames; (void)timeout_ms; return 0;
    }
    virtual int      audioPort() const { return 0; }
    virtual unsigned silentMs() const { return 0; }
};

#endif /* HARP_SHELL_TRANSPORT_H */
