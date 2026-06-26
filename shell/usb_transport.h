/* shell/usb_transport.h — the §4.3 USB binding as a ShellTransport.
 *
 * A literal 1:1 forwarder to the host/usb_io.h functions over a claimed
 * harp_io. No state, no allocation, no logic: every method is the exact call
 * HarpRuntime made before the ShellTransport refactor, so the inlined codegen
 * (and thus the golden render) is byte-identical. USB is host-paced, so the
 * free-running hooks stay the base-class stubs. The transport OWNS the claim:
 * its destructor closes it (delete transport_ == harp_usb_close), while the
 * persistent libusb context stays with the runtime (created once, reused).
 */
#ifndef HARP_SHELL_USB_TRANSPORT_H
#define HARP_SHELL_USB_TRANSPORT_H

#include "transport.h"

struct UsbTransport final : ShellTransport {
    explicit UsbTransport(harp_io *io) : io_(io) {}
    ~UsbTransport() override { if (io_) harp_usb_close(io_); }

    UsbTransport(const UsbTransport &) = delete;
    UsbTransport &operator=(const UsbTransport &) = delete;

    harp_io *ctlIo() override { return io_; }
    bool   linkPoll(unsigned ms) override { return harp_usb_link_poll(io_, ms); }
    size_t linkPending() override { return harp_usb_link_pending(io_); }
    void   setCtlTimeout(unsigned ms) override { harp_usb_set_ctl_timeout(io_, ms); }
    bool hasAudio() override { return harp_usb_has_audio(io_); }
    int  audioRead(void *buf, int len, unsigned ms) override {
        return harp_usb_audio_read(io_, buf, len, ms);
    }
    bool audioWrite(const void *buf, int len, unsigned ms) override {
        return harp_usb_audio_write(io_, buf, len, ms);
    }
    bool identity(harp_usb_devinfo *out) override { return harp_usb_devident(io_, out); }
    bool isFreeRunning() const override { return false; }
    Kind kind() const override { return Kind::Usb; }
    /* §14.4 host-context-C: the bound device's USB topology (diag-bundle key 10),
     * read off the control path. Forwards to host/usb_io.c (libusb topology walk). */
    bool usbTopology(harp_usb_topology *out) override { return harp_usb_get_topology(io_, out); }

private:
    harp_io *io_ = nullptr;
};

#endif /* HARP_SHELL_USB_TRANSPORT_H */
