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
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harp/link.h"
#include "ffs_link.h" /* §4.3 endpoint framing I/O (ffs_io, ffs_io_init) — unit-tested */
#include "log_ring.h" /* §4.2 stream `log` ring — route the USB-gadget log lines (§14.4) */

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

/* The UDC this gadget is bound to (discovered in bind_udc); reused by the §4.3 self-heal
 * soft-reconnect to drop+raise the pull-up, and to read the UDC state. */
static char g_udc[128] = "";

/* Write a UDC name (or "" to unbind) into <gadget>/UDC. Returns true on success. */
static bool write_udc(const char *gadget_path, const char *val) {
    char path[256];
    snprintf(path, sizeof path, "%s/UDC", gadget_path);
    FILE *u = fopen(path, "w");
    if (!u) {
        harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fprintf(u, "%s\n", val) >= 0 && fflush(u) == 0;
    if (fclose(u) != 0) ok = false;
    return ok;
}

/* Bind the gadget to the first available UDC (after descriptors are in). */
static void bind_udc(const char *gadget_path) {
    FILE *f = popen("ls /sys/class/udc 2>/dev/null | head -1", "r");
    if (f) {
        if (fgets(g_udc, sizeof g_udc, f)) g_udc[strcspn(g_udc, "\n")] = 0;
        pclose(f);
    }
    if (!g_udc[0]) {
        harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: no UDC available (is dwc2 in peripheral mode?)\n");
        return;
    }
    /* Drop the pull-up before (re)binding so every start re-enumerates clean. A predecessor
     * that was SIGKILLed (no on_term) leaves the UDC still bound; a bare write_udc(g_udc) then
     * returns EBUSY ("already bound") and the host keeps its stale claim. Unbinding first
     * (then a short settle so the host observes the disconnect) clears that stale binding and
     * guarantees a fresh disconnect+enumerate on this start. This is the in-process twin of the
     * daemon's on_term unbind (harp-deviced.c on_term). No-op on a fresh boot: the UDC is already
     * empty, so the unbind just writes "" to an unbound gadget. */
    write_udc(gadget_path, ""); /* clear any stale binding left by a SIGKILLed predecessor */
    usleep(150 * 1000);         /* let the host observe the disconnect before we re-advertise */
    if (write_udc(gadget_path, g_udc))
        harp_devlog(HARP_LOG_INFO, "ffs", "harp-ffs: bound to UDC %s\n", g_udc);
    else
        harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: UDC bind failed (already bound?): %s\n", strerror(errno));
}

/* True if the UDC reports a host has fully configured us (SET_CONFIGURATION done). This is the
 * "a host is present and expects us to work" signal: in the wedge it stays 'configured' even
 * though the FunctionFS function never got re-ENABLEd, which is exactly what distinguishes a
 * stuck claim from an unplugged/idle bus (where the state is 'not attached'/'addressed'). */
static bool host_configured(void) {
    if (!g_udc[0]) return false;
    char path[200];
    snprintf(path, sizeof path, "/sys/class/udc/%s/state", g_udc);
    FILE *s = fopen(path, "r");
    if (!s) return false;
    char st[32] = "";
    if (!fgets(st, sizeof st, s)) st[0] = 0;
    fclose(s);
    return strncmp(st, "configured", 10) == 0;
}

/* §4.3 self-heal (USB never-silent): force a host re-enumeration by re-binding the UDC
 * (unbind -> rebind = drop+raise the D+ pull-up). The host sees a disconnect/reconnect and
 * MUST re-issue SET_CONFIGURATION, which re-ENABLEs the FunctionFS function. This is the in-
 * process equivalent of the daemon restart that's known to recover the gadget, but without
 * dropping the process/state. Used when a host keeps us 'configured' yet never re-ENABLEs the
 * function — the macOS-through-a-USB-hub wedge, where the hub holds the device 'configured'
 * across a session close so the host skips SET_CONFIGURATION on re-claim and we'd sit silent. */
static void soft_reconnect(const char *gadget_path) {
    if (!g_udc[0]) return;
    harp_devlog(HARP_LOG_WARN, "ffs", "harp-ffs: self-heal — host configured but no re-ENABLE; soft-reconnect "
                    "(UDC re-bind) to force re-enumeration\n");
    if (!write_udc(gadget_path, "")) /* unbind: device disappears from the bus */
        harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: self-heal unbind failed: %s\n", strerror(errno));
    usleep(150 * 1000); /* let the host observe the disconnect before we re-advertise */
    if (!write_udc(gadget_path, g_udc)) /* rebind: device reappears -> host re-enumerates */
        harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: self-heal rebind failed: %s\n", strerror(errno));
}

/* ---- buffered endpoint io: ffs_io + the framing read/write live in ffs_link.c
 *      (portable, socketpair-unit-tested); ffs.c wires it to the real endpoints. ---- */

/* Audio endpoint fds, valid while the interface is enabled (-1 otherwise).
 * The audio thread in harp-deviced writes/reads these directly (§8: the
 * audio plane has dedicated transport resources, separate from the link). */
static int g_audio_in_fd = -1, g_audio_out_fd = -1;

int harp_ffs_audio_in_fd(void) { return g_audio_in_fd; }
int harp_ffs_audio_out_fd(void) { return g_audio_out_fd; }

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
        harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: cannot open %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (write_descriptors(ep0) != 0) {
        harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: descriptor write failed: %s\n", strerror(errno));
        return 1;
    }
    harp_devlog(HARP_LOG_INFO, "ffs", "harp-ffs: descriptors written\n");
    bind_udc(gadget_path);

    /* §4.3 self-heal state (USB never-silent). Once a host has enabled us at least once, the
     * outer wait for the next ENABLE is bounded: if the grace elapses with the host still
     * 'configured' but no re-ENABLE, soft-reconnect to force re-enumeration so we never sit
     * silent. HARP_FFS_REBIND_MS tunes the grace (0 disables). `rebound` caps it to ONE
     * soft-reconnect per stuck-claim episode (any ep0 event clears it), so a genuinely idle or
     * unplugged host is not re-enumerated in a tight loop, and the first enumeration (before any
     * ENABLE) is never disturbed. */
    int rebind_ms = 4000;
    const char *rebind_env = getenv("HARP_FFS_REBIND_MS");
    if (rebind_env && rebind_env[0]) {
        /* Validate, don't atoi(): a typo'd env (e.g. "4s") would atoi to 0 and SILENTLY disable
         * the never-silent self-heal — the opposite of what the operator wants. Accept only a full
         * integer in [0, 600000] (0 explicitly disables); otherwise keep the default and warn. */
        char *end = NULL;
        long v = strtol(rebind_env, &end, 10);
        if (end != rebind_env && *end == '\0' && v >= 0 && v <= 600000)
            rebind_ms = (int)v;
        else
            harp_devlog(HARP_LOG_WARN, "ffs", "harp-ffs: ignoring invalid HARP_FFS_REBIND_MS='%s' (keeping %dms)\n",
                    rebind_env, rebind_ms);
    }
    bool ever_enabled = false, rebound = false;

    for (;;) {
        struct usb_functionfs_event ev;
        if (rebind_ms > 0 && ever_enabled) {
            struct pollfd pfd = {.fd = ep0, .events = POLLIN};
            int pr = poll(&pfd, 1, rebind_ms);
            if (pr == 0) { /* grace elapsed with no ep0 event */
                if (!rebound && host_configured()) {
                    soft_reconnect(gadget_path);
                    rebound = true; /* one per episode; the next ep0 event clears it */
                }
                continue;
            }
            if (pr < 0) {
                if (errno == EINTR) continue;
                harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: ep0 poll failed: %s\n", strerror(errno));
                return 1;
            }
        }
        ssize_t r = read(ep0, &ev, sizeof ev);
        if (r < (ssize_t)sizeof ev) {
            if (r < 0 && errno == EINTR) continue;
            harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: ep0 read failed: %s\n", strerror(errno));
            return 1;
        }
        rebound = false; /* an ep0 event arrived — this stuck-claim episode is over */
        switch (ev.type) {
            case FUNCTIONFS_BIND:
                harp_devlog(HARP_LOG_INFO, "ffs", "harp-ffs: bound\n");
                break;
            case FUNCTIONFS_ENABLE: {
                ever_enabled = true;
                harp_devlog(HARP_LOG_INFO, "ffs", "harp-ffs: host enabled interface; session up\n");
                ffs_io fio;
                snprintf(path, sizeof path, "%s/ep1", ffs_dir);
                int ep_in = open(path, O_RDWR);
                snprintf(path, sizeof path, "%s/ep2", ffs_dir);
                int ep_out = open(path, O_RDWR);
                ffs_io_init(&fio, ep_in, ep_out); /* wires the framing onto ep1/ep2 */
                snprintf(path, sizeof path, "%s/ep3", ffs_dir);
                g_audio_in_fd = open(path, O_RDWR);
                snprintf(path, sizeof path, "%s/ep4", ffs_dir);
                g_audio_out_fd = open(path, O_RDWR);
                if (fio.ep_in < 0 || fio.ep_out < 0) {
                    harp_devlog(HARP_LOG_ERROR, "ffs", "harp-ffs: endpoint open failed: %s\n",
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
                harp_devlog(HARP_LOG_WARN, "ffs", "harp-ffs: endpoints closed; waiting for enable\n");
                break;
            }
            case FUNCTIONFS_DISABLE:
                harp_devlog(HARP_LOG_WARN, "ffs", "harp-ffs: host disabled interface\n");
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
