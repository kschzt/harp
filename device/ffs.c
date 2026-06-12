/* FunctionFS gadget transport for harp-deviced (Linux only).
 *
 * Implements the USB binding of spec §4.3 on a configfs gadget: a
 * vendor-specific interface (class 0xFF, subclass 0x48 'H', protocol 0x01)
 * with one bulk IN and one bulk OUT endpoint carrying the framed link.
 *
 * The gadget skeleton (idVendor etc., functions/ffs.harp, functionfs mount)
 * is created by scripts/pi-gadget.sh; this code opens ep0, writes the
 * descriptors, binds the UDC, then serves sessions: each ENABLE from the
 * host starts a session loop on the endpoint files; a read failure
 * (DISABLE/unbind -> -ESHUTDOWN) ends it, exactly like a TCP disconnect.
 *
 * Reads use a buffered chunk (multiple of wMaxPacketSize) because a USB
 * read smaller than an arriving packet would error with -EOVERFLOW — the
 * gadget can't know the host's transfer sizes in advance.
 *
 * Known gap vs spec: the BOS platform capability descriptor (§4.3.2) is not
 * exposable through libcomposite; hosts use the interface-class probe
 * fallback permitted by §6.1.
 */
#ifdef __linux__

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "harp/link.h"

#define FFS_READ_CHUNK 16384 /* multiple of 512 (HS wMaxPacketSize) */

/* ---- descriptors ---- */

struct ffs_descs {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count, hs_count, os_count;
    struct {
        struct usb_interface_descriptor intf;
        /* link pair (§4.3), then dedicated audio pair (§8.2) */
        struct usb_endpoint_descriptor_no_audio ep_in, ep_out, ep_audio_in, ep_audio_out;
    } __attribute__((packed)) fs, hs;
    /* MS OS 1.0 Extended Compat ID: Windows auto-binds WinUSB to the
     * interface, so libusb works there with zero driver ceremony (no
     * Zadig). Inert for every other host OS. Pairs with the gadget's
     * configfs os_desc block (scripts/pi-gadget.sh). */
    struct usb_os_desc_header os_hdr;
    struct usb_ext_compat_desc os_compat;
} __attribute__((packed));

struct ffs_strings {
    struct usb_functionfs_strings_head header;
    struct {
        __le16 code;
        char str[17];
    } __attribute__((packed)) lang;
} __attribute__((packed));

static int write_descriptors(int ep0) {
    struct ffs_descs d;
    memset(&d, 0, sizeof d);
    d.header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    d.header.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC |
                             FUNCTIONFS_HAS_MS_OS_DESC);
    d.header.length = htole32(sizeof d);
    d.fs_count = htole32(5);
    d.hs_count = htole32(5);
    d.os_count = htole32(1);

    struct usb_interface_descriptor intf = {
        .bLength = sizeof(struct usb_interface_descriptor),
        .bDescriptorType = USB_DT_INTERFACE,
        .bNumEndpoints = 4,
        .bInterfaceClass = 0xFF, /* vendor-specific */
        .bInterfaceSubClass = 0x48, /* 'H' */
        .bInterfaceProtocol = 0x01, /* framed link */
        .iInterface = 1,
    };
    struct usb_endpoint_descriptor_no_audio ep_in = {
        .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_DIR_IN | 1,
        .bmAttributes = USB_ENDPOINT_XFER_BULK,
    };
    struct usb_endpoint_descriptor_no_audio ep_out = {
        .bLength = sizeof(struct usb_endpoint_descriptor_no_audio),
        .bDescriptorType = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_DIR_OUT | 2,
        .bmAttributes = USB_ENDPOINT_XFER_BULK,
    };

    struct usb_endpoint_descriptor_no_audio ep_audio_in = ep_in, ep_audio_out = ep_out;
    ep_audio_in.bEndpointAddress = USB_DIR_IN | 3;
    ep_audio_out.bEndpointAddress = USB_DIR_OUT | 4;

    d.fs.intf = intf;
    d.fs.ep_in = ep_in;
    d.fs.ep_out = ep_out;
    d.fs.ep_audio_in = ep_audio_in;
    d.fs.ep_audio_out = ep_audio_out;
    d.fs.ep_in.wMaxPacketSize = htole16(64);
    d.fs.ep_out.wMaxPacketSize = htole16(64);
    d.fs.ep_audio_in.wMaxPacketSize = htole16(64);
    d.fs.ep_audio_out.wMaxPacketSize = htole16(64);
    d.hs = d.fs;
    d.hs.ep_in.wMaxPacketSize = htole16(512);
    d.hs.ep_out.wMaxPacketSize = htole16(512);
    d.hs.ep_audio_in.wMaxPacketSize = htole16(512);
    d.hs.ep_audio_out.wMaxPacketSize = htole16(512);

    d.os_hdr.interface = 1; /* per kernel docs: interface COUNT for ext-compat */
    d.os_hdr.dwLength = htole32(sizeof d.os_hdr + sizeof d.os_compat);
    d.os_hdr.bcdVersion = htole16(0x0100);
    d.os_hdr.wIndex = htole16(4); /* extended compat ID */
    d.os_hdr.wCount = htole16(1);
    memcpy(d.os_compat.CompatibleID, "WINUSB\0\0", 8);

    if (write(ep0, &d, sizeof d) != (ssize_t)sizeof d) return -1;

    struct ffs_strings s = {
        .header = {.magic = htole32(FUNCTIONFS_STRINGS_MAGIC),
                   .length = htole32(sizeof s),
                   .str_count = htole32(1),
                   .lang_count = htole32(1)},
        .lang = {htole16(0x0409), "HARP framed link"},
    };
    if (write(ep0, &s, sizeof s) != (ssize_t)sizeof s) return -1;
    return 0;
}

/* Bind the gadget to the first available UDC (after descriptors are in). */
static void bind_udc(const char *gadget_path) {
    char udc[128] = "";
    FILE *f = popen("ls /sys/class/udc 2>/dev/null | head -1", "r");
    if (f) {
        if (fgets(udc, sizeof udc, f)) udc[strcspn(udc, "\n")] = 0;
        pclose(f);
    }
    if (!udc[0]) {
        fprintf(stderr, "harp-ffs: no UDC available (is dwc2 in peripheral mode?)\n");
        return;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/UDC", gadget_path);
    FILE *u = fopen(path, "w");
    if (!u) {
        fprintf(stderr, "harp-ffs: cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    if (fprintf(u, "%s\n", udc) < 0 || fflush(u) != 0)
        fprintf(stderr, "harp-ffs: UDC bind failed (already bound?): %s\n",
                strerror(errno));
    else
        fprintf(stderr, "harp-ffs: bound to UDC %s\n", udc);
    fclose(u);
}

/* ---- buffered endpoint io ---- */

typedef struct {
    harp_io io;
    int ep_in;  /* device -> host: we write */
    int ep_out; /* host -> device: we read */
    uint8_t rbuf[FFS_READ_CHUNK];
    size_t rlen, rpos;
} ffs_io;

/* Audio endpoint fds, valid while the interface is enabled (-1 otherwise).
 * The audio thread in harp-deviced writes/reads these directly (§8: the
 * audio plane has dedicated transport resources, separate from the link). */
static int g_audio_in_fd = -1, g_audio_out_fd = -1;

int harp_ffs_audio_in_fd(void) { return g_audio_in_fd; }
int harp_ffs_audio_out_fd(void) { return g_audio_out_fd; }

static bool ffs_read_exact(harp_io *io, void *buf, size_t n) {
    ffs_io *f = (ffs_io *)io;
    uint8_t *p = buf;
    while (n) {
        if (f->rpos < f->rlen) {
            size_t take = f->rlen - f->rpos;
            if (take > n) take = n;
            memcpy(p, f->rbuf + f->rpos, take);
            f->rpos += take;
            p += take;
            n -= take;
            continue;
        }
        ssize_t r = read(f->ep_out, f->rbuf, sizeof f->rbuf);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false; /* -ESHUTDOWN on disable/unbind: session over */
        }
        if (r == 0) continue;
        f->rlen = (size_t)r;
        f->rpos = 0;
    }
    return true;
}

static bool ffs_write_all(harp_io *io, const void *buf, size_t n) {
    ffs_io *f = (ffs_io *)io;
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(f->ep_in, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

/* ---- ep0 event loop ---- */

static void ack_setup(int ep0, const struct usb_ctrlrequest *setup) {
    /* No vendor control requests are defined; ack with a zero-length stage. */
    if (setup->bRequestType & USB_DIR_IN)
        (void)!write(ep0, NULL, 0);
    else
        (void)!read(ep0, NULL, 0);
}

int harp_ffs_serve(const char *ffs_dir, const char *gadget_path,
                   void (*session)(void *ud, harp_io *io), void *ud) {
    char path[512];
    snprintf(path, sizeof path, "%s/ep0", ffs_dir);
    int ep0 = open(path, O_RDWR);
    if (ep0 < 0) {
        fprintf(stderr, "harp-ffs: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (write_descriptors(ep0) != 0) {
        fprintf(stderr, "harp-ffs: descriptor write failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "harp-ffs: descriptors written\n");
    bind_udc(gadget_path);

    for (;;) {
        struct usb_functionfs_event ev;
        ssize_t r = read(ep0, &ev, sizeof ev);
        if (r < (ssize_t)sizeof ev) {
            if (r < 0 && errno == EINTR) continue;
            fprintf(stderr, "harp-ffs: ep0 read failed: %s\n", strerror(errno));
            return 1;
        }
        switch (ev.type) {
            case FUNCTIONFS_BIND:
                fprintf(stderr, "harp-ffs: bound\n");
                break;
            case FUNCTIONFS_ENABLE: {
                fprintf(stderr, "harp-ffs: host enabled interface; session up\n");
                ffs_io fio;
                memset(&fio, 0, sizeof fio);
                fio.io.read_exact = ffs_read_exact;
                fio.io.write_all = ffs_write_all;
                snprintf(path, sizeof path, "%s/ep1", ffs_dir);
                fio.ep_in = open(path, O_RDWR);
                snprintf(path, sizeof path, "%s/ep2", ffs_dir);
                fio.ep_out = open(path, O_RDWR);
                snprintf(path, sizeof path, "%s/ep3", ffs_dir);
                g_audio_in_fd = open(path, O_RDWR);
                snprintf(path, sizeof path, "%s/ep4", ffs_dir);
                g_audio_out_fd = open(path, O_RDWR);
                if (fio.ep_in < 0 || fio.ep_out < 0) {
                    fprintf(stderr, "harp-ffs: endpoint open failed: %s\n",
                            strerror(errno));
                    if (fio.ep_in >= 0) close(fio.ep_in);
                    if (fio.ep_out >= 0) close(fio.ep_out);
                    break;
                }
                /* Serve sessions until the endpoints die (DISABLE/unplug).
                 * A clean core.bye just starts the next session, like the
                 * TCP accept loop. */
                for (;;) {
                    session(ud, &fio.io);
                    /* probe whether the endpoint is still alive */
                    fio.rlen = fio.rpos = 0;
                    uint8_t probe;
                    ssize_t pr = read(fio.ep_out, &probe, 0);
                    if (pr < 0 && errno != EINTR && errno != EAGAIN) break;
                }
                if (g_audio_in_fd >= 0) close(g_audio_in_fd);
                if (g_audio_out_fd >= 0) close(g_audio_out_fd);
                g_audio_in_fd = g_audio_out_fd = -1;
                close(fio.ep_in);
                close(fio.ep_out);
                fprintf(stderr, "harp-ffs: endpoints closed; waiting for enable\n");
                break;
            }
            case FUNCTIONFS_DISABLE:
                fprintf(stderr, "harp-ffs: host disabled interface\n");
                break;
            case FUNCTIONFS_SETUP:
                ack_setup(ep0, &ev.u.setup);
                break;
            case FUNCTIONFS_UNBIND:
            case FUNCTIONFS_SUSPEND:
            case FUNCTIONFS_RESUME:
            default:
                break;
        }
    }
}

#endif /* __linux__ */
