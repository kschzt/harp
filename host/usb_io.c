/* libusb transport for harp-probe.
 *
 * Discovery follows §6.1's class-triple probe (0xFF/0x48/0x01) — the BOS
 * platform-capability path of §4.3 isn't available from a libcomposite
 * gadget, and scanning interface descriptors finds the device regardless.
 *
 * Reads are buffered in wMaxPacketSize multiples: requesting fewer bytes
 * than an arriving packet holds would error with LIBUSB_ERROR_OVERFLOW, so
 * we always post large multiple-of-512 reads and serve callers from the
 * buffer. Read timeout is infinite to match blocking-socket semantics.
 */
#ifdef HAVE_LIBUSB

#include "usb_io.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USB_READ_CHUNK 16384
#define USB_WRITE_RETRY_MS 250
#define USB_WRITE_GIVE_UP_MS 30000

typedef struct {
    harp_io io;
    libusb_context *ctx;
    libusb_device_handle *h;
    int iface;
    uint8_t ep_in, ep_out;             /* framed link */
    uint8_t ep_audio_in, ep_audio_out; /* HARP stream (0 = absent) */
    /* pending IN bytes: grows on drain, compacts when fully consumed */
    harp_cbuf pend;
    size_t pos;
} usb_io;

/* Pull whatever the device has queued on the IN endpoint into pend.
 * timeout_ms = 0 blocks until something arrives. Returns false on hard error. */
static bool usb_fill(usb_io *u, unsigned timeout_ms) {
    uint8_t tmp[USB_READ_CHUNK];
    int got = 0;
    int rc = libusb_bulk_transfer(u->h, u->ep_in, tmp, sizeof tmp, &got, timeout_ms);
    if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
        fprintf(stderr, "harp-usb: bulk in failed: %s\n", libusb_error_name(rc));
        return false;
    }
    if (got) harp_cbuf_put(&u->pend, tmp, (size_t)got);
    return !u->pend.oom;
}

static bool usb_read_exact(harp_io *io, void *buf, size_t n) {
    usb_io *u = (usb_io *)io;
    uint8_t *p = buf;
    while (n) {
        if (u->pos < u->pend.len) {
            size_t take = u->pend.len - u->pos;
            if (take > n) take = n;
            memcpy(p, u->pend.buf + u->pos, take);
            u->pos += take;
            p += take;
            n -= take;
            continue;
        }
        harp_cbuf_reset(&u->pend);
        u->pos = 0;
        if (!usb_fill(u, 0)) return false;
    }
    return true;
}

/* A blocked device (waiting for us to read its IN data before it posts its
 * next OUT read) NAKs our writes. On write timeout, drain IN and retry —
 * this is the single-threaded substitute for an always-pending IN reader. */
static bool usb_write_all(harp_io *io, const void *buf, size_t n) {
    usb_io *u = (usb_io *)io;
    uint8_t *p = (uint8_t *)buf;
    unsigned waited = 0;
    while (n) {
        int chunk = n > INT32_MAX ? INT32_MAX : (int)n;
        int sent = 0;
        int rc = libusb_bulk_transfer(u->h, u->ep_out, p, chunk, &sent, USB_WRITE_RETRY_MS);
        p += sent;
        n -= (size_t)sent;
        if (rc == 0) {
            waited = 0;
            continue;
        }
        if (rc != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "harp-usb: bulk out failed: %s\n", libusb_error_name(rc));
            return false;
        }
        if (!usb_fill(u, 50)) return false; /* unblock the device's IN writes */
        waited += USB_WRITE_RETRY_MS;
        if (waited >= USB_WRITE_GIVE_UP_MS) {
            fprintf(stderr, "harp-usb: bulk out stalled for %u ms, giving up\n", waited);
            return false;
        }
    }
    return true;
}

/* Find the HARP interface in a device's active configuration. The first bulk
 * IN/OUT pair (descriptor order) is the framed link; the second, if present,
 * is the dedicated HARP stream (§8.2). */
static bool find_harp_interface(libusb_device *dev, int *iface, uint8_t *ep_in,
                                uint8_t *ep_out, uint8_t *ep_ain, uint8_t *ep_aout) {
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0) return false;
    bool found = false;
    for (int i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const struct libusb_interface_descriptor *alt = &cfg->interface[i].altsetting[0];
        if (alt->bInterfaceClass != 0xFF || alt->bInterfaceSubClass != 0x48 ||
            alt->bInterfaceProtocol != 0x01 || alt->bNumEndpoints < 2)
            continue;
        uint8_t in[2] = {0, 0}, out[2] = {0, 0};
        int nin = 0, nout = 0;
        for (int e = 0; e < alt->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
            if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
                if (nin < 2) in[nin++] = ep->bEndpointAddress;
            } else {
                if (nout < 2) out[nout++] = ep->bEndpointAddress;
            }
        }
        if (nin >= 1 && nout >= 1) {
            *iface = alt->bInterfaceNumber;
            *ep_in = in[0];
            *ep_out = out[0];
            *ep_ain = in[1];
            *ep_aout = out[1];
            found = true;
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}

harp_io *harp_usb_open(void) {
    usb_io *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    if (libusb_init(&u->ctx) != 0) {
        fprintf(stderr, "harp-usb: libusb init failed\n");
        free(u);
        return NULL;
    }
    libusb_device **list;
    ssize_t n = libusb_get_device_list(u->ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        int iface;
        uint8_t ep_in, ep_out, ep_ain = 0, ep_aout = 0;
        if (!find_harp_interface(list[i], &iface, &ep_in, &ep_out, &ep_ain, &ep_aout))
            continue;
        struct libusb_device_descriptor dd;
        libusb_get_device_descriptor(list[i], &dd);
        if (libusb_open(list[i], &u->h) != 0) {
            fprintf(stderr, "harp-usb: found HARP device %04x:%04x but cannot open it\n",
                    dd.idVendor, dd.idProduct);
            continue;
        }
        libusb_set_auto_detach_kernel_driver(u->h, 1);
        if (libusb_claim_interface(u->h, iface) != 0) {
            fprintf(stderr, "harp-usb: cannot claim interface %d\n", iface);
            libusb_close(u->h);
            u->h = NULL;
            continue;
        }
        u->iface = iface;
        u->ep_in = ep_in;
        u->ep_out = ep_out;
        u->ep_audio_in = ep_ain;
        u->ep_audio_out = ep_aout;
        u->io.read_exact = usb_read_exact;
        u->io.write_all = usb_write_all;
        char serial[64] = "?";
        if (dd.iSerialNumber)
            libusb_get_string_descriptor_ascii(u->h, dd.iSerialNumber,
                                               (unsigned char *)serial, sizeof serial);
        fprintf(stderr,
                "harp-usb: claimed %04x:%04x serial %s (iface %d, ep in %02x out %02x)\n",
                dd.idVendor, dd.idProduct, serial, iface, ep_in, ep_out);
        libusb_free_device_list(list, 1);
        return &u->io;
    }
    libusb_free_device_list(list, 1);
    fprintf(stderr, "harp-usb: no HARP device on the bus (class FF/48/01 scan)\n");
    libusb_exit(u->ctx);
    free(u);
    return NULL;
}

bool harp_usb_has_audio(harp_io *io) {
    return io && ((usb_io *)io)->ep_audio_in != 0;
}

int harp_usb_audio_read(harp_io *io, void *buf, int len, unsigned timeout_ms) {
    usb_io *u = (usb_io *)io;
    int got = 0;
    int rc = libusb_bulk_transfer(u->h, u->ep_audio_in, buf, len, &got, timeout_ms);
    if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) return got;
    fprintf(stderr, "harp-usb: audio bulk in failed: %s\n", libusb_error_name(rc));
    return -1;
}

void harp_usb_close(harp_io *io) {
    if (!io) return;
    usb_io *u = (usb_io *)io;
    if (u->h) {
        libusb_release_interface(u->h, u->iface);
        libusb_close(u->h);
    }
    harp_cbuf_free(&u->pend);
    libusb_exit(u->ctx);
    free(u);
}

#endif /* HAVE_LIBUSB */
