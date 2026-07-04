/* libusb transport for HARP hosts (probe + VST3 shell runtime).
 *
 * Discovery follows §6.1's class-triple probe (0xFF/0x48/0x01) — the BOS
 * platform-capability path of §4.3 isn't available from a libcomposite
 * gadget, and scanning interface descriptors finds the device regardless.
 *
 * ASYNC ARCHITECTURE (second generation). The first implementation used
 * sync libusb: every blocking call did its own event handling, and under
 * a multithreaded host (reader + feeder + pump) completion reaping
 * bounced between threads — measured 20-25 ms completion tails at user
 * QoS, which forced a 5-block (26.7 ms) ring cushion in the shell. Now
 * one dedicated event thread owns libusb_handle_events at elevated QoS:
 *
 *   - both IN endpoints keep transfers ALWAYS PENDING (link x2, audio x4,
 *     16 KiB each); completions land in lock-protected byte FIFOs and the
 *     transfer resubmits immediately. The §4.2.1 deadlock precondition
 *     (no posted IN read while writing) is gone BY CONSTRUCTION, so the
 *     old drain-on-stall write loop is too.
 *   - audio OUT (pacing) writes are fire-and-forget submissions from a
 *     small slot pool: the feeder never blocks on the wire again (the
 *     8 ms drain-on-stall write timeout was the head-of-line stall that
 *     motivated the event pump). Pipeline depth control stays where it
 *     belongs, in the shell's in-flight frame counting.
 *   - link OUT writes submit and wait for completion (control plane
 *     keeps write_all semantics).
 *
 * Reads stay buffered in wMaxPacketSize multiples: requesting fewer bytes
 * than an arriving packet holds errors with LIBUSB_ERROR_OVERFLOW, so all
 * posted reads are large multiples of 512 and callers are served from the
 * FIFOs.
 */
#ifdef HAVE_LIBUSB

#include "usb_io.h"
#include "usb_select.h" /* pure §15.2 device-match predicate (host-unit-tested) */
#include "rt_sched.h"   /* harp_thread_set_realtime + HARP_RT_PERIOD_US_DEFAULT (extracted; no libusb) */

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sync_compat.h"
#include "harp/plat.h" /* harp_now_ns (monotonic) for the rtstats timing capture */
#ifdef __APPLE__
#include <pthread/qos.h> /* QOS_CLASS_USER_INTERACTIVE: the event thread's fallback band */
#endif

#define USB_READ_CHUNK 16384
#define USB_AUDIO_IN_XFERS 4
#define USB_LINK_IN_XFERS 2
#define USB_AOUT_SLOTS 8
#define USB_LINK_WRITE_GIVE_UP_MS 30000

/* The default real-time period (one §8.3 pacing block @ 48 k) lives in rt_sched.h
 * as HARP_RT_PERIOD_US_DEFAULT; a device with a smaller render block overrides it
 * per session via HARP_USB_RT_PERIOD_US (read in harp_usb_open below). */

/* rtstats capture cap: one µs sample per audio transfer. 262144 ≈ 23 min of
 * 256-sample frames, so an ordinary render never truncates; ~1 MiB per array,
 * allocated only when HARP_USB_RTSTATS is set. */
#define USB_RT_STAT_CAP (1u << 18)

/* FIFO sizes (bytes, power of two). Audio: stereo f32 at 48 k is 384 KiB/s;
 * half a second absorbs any scheduling excursion that matters. */
#define AUDIO_FIFO_SZ (1u << 18) /* 256 KiB */
#define LINK_FIFO_SZ (1u << 16)  /* 64 KiB */

typedef struct {
    uint8_t *buf;
    size_t cap, head, tail; /* tail = write side, head = read side */
} byte_fifo;

static size_t fifo_used(const byte_fifo *f) { return f->tail - f->head; }

static void fifo_push(byte_fifo *f, const uint8_t *p, size_t n) {
    /* caller holds the lock; overflow drops the OLDEST bytes (the reader
     * is wedged anyway if this fires; counted by the caller) */
    if (n > f->cap) {
        p += n - f->cap;
        n = f->cap;
    }
    if (fifo_used(f) + n > f->cap) f->head = f->tail + n - f->cap;
    for (size_t i = 0; i < n; i++) f->buf[(f->tail + i) & (f->cap - 1)] = p[i];
    f->tail += n;
}

static size_t fifo_pop(byte_fifo *f, uint8_t *out, size_t n) {
    size_t avail = fifo_used(f);
    if (n > avail) n = avail;
    for (size_t i = 0; i < n; i++) out[i] = f->buf[(f->head + i) & (f->cap - 1)];
    f->head += n;
    return n;
}

typedef struct usb_io usb_io;

/* one pending IN transfer: fixed buffer, resubmits from its completion */
typedef struct {
    usb_io *u;
    struct libusb_transfer *xfer;
    uint8_t buf[USB_READ_CHUNK];
    int is_audio;
} in_xfer;

/* one audio OUT slot. Sized for a FULL H->D frame, not just a pacing header: §8.2 H->D audio
 * carries nsamples*slots floats, and the §14.3 digital loopback emits an impulse of
 * HARP_AUDIO_HDR_LEN(20)+HARP_AUDIO_FENCE_LEN(4)+kBlock(256)*4 = 1048 B. The old 256 B slot
 * rejected it at harp_usb_audio_write's len guard, so the loopback worked over TCP/eth but broke
 * over USB. USB_READ_CHUNK (16384) is symmetric with the IN xfer buffer + holds it with headroom. */
typedef struct {
    usb_io *u;
    struct libusb_transfer *xfer;
    uint8_t buf[USB_READ_CHUNK];
    int busy; /* under u->mu */
    uint64_t submit_ns; /* rtstats: harp_now_ns at submit, read at completion */
} aout_slot;

/* ---- HARP_USB_RTSTATS capture (allocated only when enabled) ----
 * Per-audio-transfer timing so a streaming campaign can characterize the
 * scheduling-jitter tail and pin starvation as thread-late vs queue-empty:
 *   out_lat : audio bulk-OUT submit -> completion-callback latency (the wire +
 *             reaping turnaround for one pacing frame).
 *   out_gap : interval between consecutive OUT submissions = the PRODUCER's
 *             cadence; a fat tail here is the feed thread being preempted
 *             (thread-late) rather than the wire stalling.
 *   in_gap  : interval between consecutive audio bulk-IN completions = the rate
 *             the device's render frames actually LAND on the host; a fat tail
 *             here is the dropout window (reaping-late or device-late).
 *   depth   : OUT transfers already in flight at each submit (queue occupancy) —
 *             0/1 means the endpoint is producer-gated, not ring-limited. */
typedef struct {
    uint32_t *out_lat, *out_gap, *in_gap; /* µs samples */
    size_t out_n, outgap_n, in_n;
    uint64_t last_out_submit_ns, last_in_ns;
    uint64_t depth_hist[USB_AOUT_SLOTS + 1];
    uint64_t truncated; /* samples dropped past the cap (should stay 0) */
} rt_stats;
/* §14.3: the slot MUST hold a full H->D loopback frame (the bug this fixes). hdr(20)+fence(4)+kBlock(256)*4 = 1048 B. */
_Static_assert(sizeof(((aout_slot *)0)->buf) >= 1048, "USB audio-OUT slot too small for a §14.3 H->D loopback frame");

/* one link OUT write in flight (stack-allocated by the writer) */
typedef struct {
    usb_io *u;
    int done, ok;
} lout_ctx;

struct usb_io {
    harp_io io;
    libusb_context *ctx;
    bool owns_ctx;     /* true: we libusb_exit it on close; false: borrowed */
    libusb_device_handle *h;
    int iface;
    uint8_t ep_in, ep_out;             /* framed link */
    uint8_t ep_audio_in, ep_audio_out; /* HARP stream (0 = absent) */
    uint16_t vendor_id, product_id;    /* bound device's USB descriptor ids */
    char dev_serial[64];               /* bound device's USB serial */

    harp_thread evthread;
    harp_flag ev_running;

    harp_mutex mu;
    harp_cond cv; /* broadcast on: fifo growth, slot free, write done, death */
    int dead;          /* transport failed (unplug); all ops fail fast */
    int closing;       /* orderly teardown; cancelled completions are normal */
    int inflight;      /* transfers not yet reaped (close waits for zero) */
    unsigned ctl_timeout_ms; /* >0 only during the hello window: bounds the link read/write so a
                                wedged daemon (enumerated, not draining the endpoint) can't hang
                                the dial; 0 = block (the live framed link). Set via harp_usb_set_ctl_timeout. */

    int debug;        /* HARP_USB_DEBUG: per-completion tracing */
    int sync_audio;    /* HARP_USB_SYNC_AUDIO: diagnostic fallback — audio
                          reads bypass the async machinery (sync bulk),
                          e.g. to bisect transport issues on a new OS */
    int rt_enable;     /* HARP_USB_RT (default on): promote the event thread to
                          hard real-time (THREAD_TIME_CONSTRAINT / SCHED_FIFO).
                          Set HARP_USB_RT=0 to keep the old USER_INTERACTIVE QoS
                          (the A/B "before" arm + a safety valve). */
    double rt_period_ns; /* RT period = audio block cadence (HARP_USB_RT_PERIOD_US) */
    rt_stats *st;        /* HARP_USB_RTSTATS capture, or NULL when disabled */
    byte_fifo link_fifo, audio_fifo;
    uint64_t fifo_drops; /* overflow bytes dropped — never silent */

    in_xfer link_in[USB_LINK_IN_XFERS];
    in_xfer audio_in[USB_AUDIO_IN_XFERS];
    aout_slot aout[USB_AOUT_SLOTS];
};

static void mark_dead_locked(usb_io *u, const char *what, int rc) {
    if (!u->dead && !u->closing)
        fprintf(stderr, "harp-usb: %s failed: %s\n", what, libusb_error_name(rc));
    u->dead = 1;
    harp_cond_broadcast(&u->cv);
}

/* harp_thread_set_realtime() — the THREAD_TIME_CONSTRAINT / SCHED_FIFO promotion
 * that pins this completion-reaping thread (and the shell's feed/pump/reader
 * threads) to the host real-time class — now lives in host/rt_sched.c so it is
 * reachable WITHOUT libusb (and gateable with no hardware). Declared in
 * rt_sched.h, included above. */

/* ---------------- HARP_USB_RTSTATS capture ---------------- */

static rt_stats *stats_alloc(void) {
    rt_stats *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->out_lat = malloc(USB_RT_STAT_CAP * sizeof *s->out_lat);
    s->out_gap = malloc(USB_RT_STAT_CAP * sizeof *s->out_gap);
    s->in_gap  = malloc(USB_RT_STAT_CAP * sizeof *s->in_gap);
    if (!s->out_lat || !s->out_gap || !s->in_gap) {
        free(s->out_lat); free(s->out_gap); free(s->in_gap); free(s);
        return NULL;
    }
    return s;
}

static void stats_free(rt_stats *s) {
    if (!s) return;
    free(s->out_lat); free(s->out_gap); free(s->in_gap);
    free(s);
}

/* caller holds u->mu (the callbacks already do) */
static void st_push(rt_stats *s, uint32_t *arr, size_t *n, uint32_t v) {
    if (*n < USB_RT_STAT_CAP) arr[(*n)++] = v;
    else s->truncated++;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void st_report(const char *name, uint32_t *a, size_t n) {
    if (!n) { fprintf(stderr, "  %-8s (no samples)\n", name); return; }
    qsort(a, n, sizeof *a, cmp_u32);
    double mean = 0;
    for (size_t i = 0; i < n; i++) mean += a[i];
    mean /= (double)n;
    size_t i50 = (size_t)(n * 0.50), i99 = (size_t)(n * 0.99), i999 = (size_t)(n * 0.999);
    if (i50 >= n) i50 = n - 1;
    if (i99 >= n) i99 = n - 1;
    if (i999 >= n) i999 = n - 1;
    fprintf(stderr, "  %-8s n=%-6zu mean=%7.1f  P50=%-6u P99=%-6u P99.9=%-6u max=%-6u  (µs)\n",
            name, n, mean, a[i50], a[i99], a[i999], a[n - 1]);
}

static void stats_dump(usb_io *u) {
    rt_stats *s = u->st;
    if (!s) return;
    fprintf(stderr, "harp-usb: RTSTATS for %s (rt=%s)\n", u->dev_serial,
            u->rt_enable ? "on" : "off");
    st_report("out_lat", s->out_lat, s->out_n);   /* OUT submit -> completion */
    st_report("out_gap", s->out_gap, s->outgap_n); /* producer cadence (thread-late probe) */
    st_report("in_gap", s->in_gap, s->in_n);       /* D->H landing cadence (dropout-window probe) */
    fprintf(stderr, "  OUT in-flight depth at submit:");
    for (int d = 0; d <= USB_AOUT_SLOTS; d++)
        if (s->depth_hist[d]) fprintf(stderr, " [%d]=%llu", d, (unsigned long long)s->depth_hist[d]);
    fprintf(stderr, "\n");
    if (s->truncated)
        fprintf(stderr, "  (rtstats truncated %llu samples past cap)\n",
                (unsigned long long)s->truncated);
}

/* ---------------- completions (run on the event thread) ---------------- */

static void LIBUSB_CALL in_cb(struct libusb_transfer *x) {
    in_xfer *t = x->user_data;
    usb_io *u = t->u;
    if (u->debug)
        fprintf(stderr, "harp-usb-dbg: in_cb %s status=%d len=%d\n",
                t->is_audio ? "audio" : "link", x->status, x->actual_length);
    harp_mutex_lock(&u->mu);
    u->inflight--;
    if (x->status == LIBUSB_TRANSFER_COMPLETED ||
        x->status == LIBUSB_TRANSFER_TIMED_OUT) {
        if (x->actual_length > 0) {
            byte_fifo *f = t->is_audio ? &u->audio_fifo : &u->link_fifo;
            size_t before = fifo_used(f);
            fifo_push(f, t->buf, (size_t)x->actual_length);
            if (fifo_used(f) - before < (size_t)x->actual_length)
                u->fifo_drops += x->actual_length - (fifo_used(f) - before);
            if (u->st && t->is_audio) { /* D->H landing cadence (the dropout window) */
                uint64_t now = harp_now_ns();
                if (u->st->last_in_ns)
                    st_push(u->st, u->st->in_gap, &u->st->in_n,
                            (uint32_t)((now - u->st->last_in_ns) / 1000));
                u->st->last_in_ns = now;
            }
            harp_cond_broadcast(&u->cv);
        }
        if (!u->closing && !u->dead) { /* keep the read pending, always */
            t->xfer = libusb_alloc_transfer(0); /* fresh per submission */
            if (t->xfer) {
                libusb_fill_bulk_transfer(t->xfer, u->h,
                                          x->endpoint, t->buf, sizeof t->buf, in_cb,
                                          t, 1000);
                if (libusb_submit_transfer(t->xfer) == 0)
                    u->inflight++;
                else
                    mark_dead_locked(u, "in resubmit", LIBUSB_ERROR_IO);
            } else
                mark_dead_locked(u, "in alloc", LIBUSB_ERROR_NO_MEM);
            libusb_free_transfer(x); /* the completed one */
            harp_cond_broadcast(&u->cv);
            harp_mutex_unlock(&u->mu);
            return;
        }
    } else if (x->status != LIBUSB_TRANSFER_CANCELLED) {
        mark_dead_locked(u, t->is_audio ? "audio bulk in" : "bulk in",
                         x->status == LIBUSB_TRANSFER_NO_DEVICE ? LIBUSB_ERROR_NO_DEVICE
                                                                : LIBUSB_ERROR_IO);
    }
    harp_cond_broadcast(&u->cv); /* close() may be waiting on inflight */
    harp_mutex_unlock(&u->mu);
}

static void LIBUSB_CALL aout_cb(struct libusb_transfer *x) {
    aout_slot *s = x->user_data;
    usb_io *u = s->u;
    harp_mutex_lock(&u->mu);
    u->inflight--;
    s->busy = 0;
    if (u->st && s->submit_ns) { /* OUT submit -> completion turnaround */
        st_push(u->st, u->st->out_lat, &u->st->out_n,
                (uint32_t)((harp_now_ns() - s->submit_ns) / 1000));
        s->submit_ns = 0;
    }
    if (x->status != LIBUSB_TRANSFER_COMPLETED &&
        x->status != LIBUSB_TRANSFER_CANCELLED)
        mark_dead_locked(u, "audio bulk out",
                         x->status == LIBUSB_TRANSFER_NO_DEVICE ? LIBUSB_ERROR_NO_DEVICE
                                                                : LIBUSB_ERROR_IO);
    harp_cond_broadcast(&u->cv);
    harp_mutex_unlock(&u->mu);
}

static void LIBUSB_CALL lout_cb(struct libusb_transfer *x) {
    lout_ctx *c = x->user_data;
    usb_io *u = c->u;
    harp_mutex_lock(&u->mu);
    u->inflight--;
    c->done = 1;
    c->ok = x->status == LIBUSB_TRANSFER_COMPLETED;
    if (!c->ok && x->status != LIBUSB_TRANSFER_CANCELLED)
        mark_dead_locked(u, "bulk out",
                         x->status == LIBUSB_TRANSFER_NO_DEVICE ? LIBUSB_ERROR_NO_DEVICE
                                                                : LIBUSB_ERROR_IO);
    harp_cond_broadcast(&u->cv);
    harp_mutex_unlock(&u->mu);
    libusb_free_transfer(x);
}

/* ---------------- the event thread ---------------- */

static HARP_THREAD_RET ev_main(void *arg) {
    usb_io *u = arg;
    /* completion reaping is on the realtime path: every audio byte and every
     * event acknowledgment passes through this thread. Set the USER_INTERACTIVE
     * QoS first — it is both the macOS fallback when RT is denied and what
     * HARP_USB_RT=0 keeps for the A/B "before" arm — then promote to hard
     * real-time (THREAD_TIME_CONSTRAINT / SCHED_FIFO) so host CPU contention
     * can't deschedule us and empty the endpoint (the cliff-onset dropout). */
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    if (u->rt_enable) harp_thread_set_realtime(u->rt_period_ns / 1000.0);
    while (harp_flag_load_acq(&u->ev_running)) {
        struct timeval tv = {0, 20000}; /* 20 ms tick to observe shutdown */
        libusb_handle_events_timeout_completed(u->ctx, &tv, NULL);
    }
    return 0;
}

/* ---------------- harp_io: framed link ---------------- */

static bool usb_read_exact(harp_io *io, void *buf, size_t n) {
    usb_io *u = (usb_io *)io;
    uint8_t *p = buf;
    harp_mutex_lock(&u->mu);
    /* During the hello window (ctl_timeout_ms>0) bound the total wait so a wedged daemon that
     * never feeds the link FIFO can't hang the dial; 0 = block forever (the live framed link,
     * read only after linkPoll). The deadline is absolute so a partial-then-stalled hello is
     * bounded too. */
    uint64_t dl = u->ctl_timeout_ms ? harp_deadline_ms(u->ctl_timeout_ms) : 0;
    while (n) {
        size_t got = fifo_pop(&u->link_fifo, p, n);
        p += got;
        n -= got;
        if (!n) break;
        if (u->dead || u->closing) {
            harp_mutex_unlock(&u->mu);
            return false;
        }
        if (dl) {
            bool timedout = harp_cond_timedwait(&u->cv, &u->mu, dl);
            if (timedout && fifo_used(&u->link_fifo) == 0) {
                harp_mutex_unlock(&u->mu);
                return false; /* hello-window timeout: device not responding */
            }
        } else {
            harp_cond_wait(&u->cv, &u->mu);
        }
    }
    harp_mutex_unlock(&u->mu);
    return true;
}

static bool usb_write_all(harp_io *io, const void *buf, size_t n) {
    usb_io *u = (usb_io *)io;
    if (!n) return true;
    lout_ctx c = {u, 0, 1};
    struct libusb_transfer *x = libusb_alloc_transfer(0);
    if (!x) return false;
    /* the transfer must own a stable copy: the caller's buffer may die
     * while the submission is in flight during an error unwind */
    uint8_t *copy = malloc(n);
    if (!copy) {
        libusb_free_transfer(x);
        return false;
    }
    memcpy(copy, buf, n);
    libusb_fill_bulk_transfer(x, u->h, u->ep_out, copy, (int)n, lout_cb, &c, 0);
    x->flags = LIBUSB_TRANSFER_FREE_BUFFER; /* libusb frees `copy` with x */
    harp_mutex_lock(&u->mu);
    if (u->dead || u->closing) {
        harp_mutex_unlock(&u->mu);
        libusb_free_transfer(x); /* frees copy via flag */
        return false;
    }
    if (libusb_submit_transfer(x) != 0) {
        mark_dead_locked(u, "bulk out submit", LIBUSB_ERROR_IO);
        harp_mutex_unlock(&u->mu);
        libusb_free_transfer(x);
        return false;
    }
    u->inflight++;
    /* During the hello window use the short ctl bound (a daemon that isn't draining the link
     * endpoint stalls the OUT transfer); otherwise the normal 30s give-up for a live link. */
    uint64_t dl = harp_deadline_ms(u->ctl_timeout_ms ? u->ctl_timeout_ms : USB_LINK_WRITE_GIVE_UP_MS);
    while (!c.done) {
        if (harp_cond_timedwait(&u->cv, &u->mu, dl) && !c.done) {
            /* still in flight after the give-up window: the device is
             * wedged. Cancel and wait for the (now certain) completion —
             * the ctx must stay valid until the callback runs. */
            libusb_cancel_transfer(x);
            while (!c.done) harp_cond_wait(&u->cv, &u->mu);
            c.ok = 0;
            break;
        }
    }
    harp_mutex_unlock(&u->mu);
    return c.ok != 0;
}

/* ---------------- audio endpoint pair ---------------- */

bool harp_usb_has_audio(harp_io *io) {
    return io && ((usb_io *)io)->ep_audio_in != 0;
}

int harp_usb_audio_read(harp_io *io, void *buf, int len, unsigned timeout_ms) {
    usb_io *u = (usb_io *)io;
    if (u->sync_audio) {
        int got = 0;
        int rc = libusb_bulk_transfer(u->h, u->ep_audio_in, buf, len, &got,
                                      timeout_ms);
        if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) return got;
        fprintf(stderr, "harp-usb: sync audio in failed: %s\n", libusb_error_name(rc));
        return -1;
    }
    harp_mutex_lock(&u->mu);
    if (!fifo_used(&u->audio_fifo) && !u->dead && !u->closing && timeout_ms) {
        uint64_t dl = harp_deadline_ms(timeout_ms);
        while (!fifo_used(&u->audio_fifo) && !u->dead && !u->closing)
            if (harp_cond_timedwait(&u->cv, &u->mu, dl)) break;
    }
    size_t got = fifo_pop(&u->audio_fifo, buf, (size_t)len);
    int rc = (int)got;
    if (!got && (u->dead || u->closing)) rc = -1;
    harp_mutex_unlock(&u->mu);
    return rc;
}

bool harp_usb_audio_write(harp_io *io, const void *buf, int len, unsigned timeout_ms) {
    usb_io *u = (usb_io *)io;
    if (len > (int)sizeof u->aout[0].buf) {
        fprintf(stderr, "harp-usb: audio write too large (%d)\n", len);
        return false;
    }
    harp_mutex_lock(&u->mu);
    aout_slot *s = NULL;
    for (;;) {
        if (u->dead || u->closing) {
            harp_mutex_unlock(&u->mu);
            return false;
        }
        for (int i = 0; i < USB_AOUT_SLOTS; i++)
            if (!u->aout[i].busy) {
                s = &u->aout[i];
                break;
            }
        if (s) break;
        /* all slots in flight = genuine device backpressure; same contract
         * as the old sync write timeout */
        uint64_t dl = harp_deadline_ms(timeout_ms ? timeout_ms : 1);
        if (harp_cond_timedwait(&u->cv, &u->mu, dl)) {
            harp_mutex_unlock(&u->mu);
            return false;
        }
    }
    if (u->st) { /* producer cadence + queue occupancy, sampled at submit */
        uint64_t now = harp_now_ns();
        int depth = 0;
        for (int i = 0; i < USB_AOUT_SLOTS; i++)
            if (u->aout[i].busy) depth++; /* OUT transfers already in flight */
        u->st->depth_hist[depth]++;
        if (u->st->last_out_submit_ns)
            st_push(u->st, u->st->out_gap, &u->st->outgap_n,
                    (uint32_t)((now - u->st->last_out_submit_ns) / 1000));
        u->st->last_out_submit_ns = now;
        s->submit_ns = now;
    }
    memcpy(s->buf, buf, (size_t)len);
    libusb_fill_bulk_transfer(s->xfer, u->h, u->ep_audio_out, s->buf, len, aout_cb, s,
                              0);
    if (libusb_submit_transfer(s->xfer) != 0) {
        mark_dead_locked(u, "audio bulk out submit", LIBUSB_ERROR_IO);
        harp_mutex_unlock(&u->mu);
        return false;
    }
    s->busy = 1;
    u->inflight++;
    harp_mutex_unlock(&u->mu);
    return true;
}

/* ---------------- ctl hello-window timeout ---------------- */

/* Bound the link read/write during the hello window so a wedged daemon (enumerated but not
 * draining the endpoint) can't hang the dial. ms=0 restores blocking for the live framed link.
 * The shell sets 2000 around hello/identity, 0 after — the USB twin of EthTransport's SO_RCVTIMEO. */
void harp_usb_set_ctl_timeout(harp_io *io, unsigned ms) {
    if (!io) return;
    usb_io *u = (usb_io *)io;
    harp_mutex_lock(&u->mu);
    u->ctl_timeout_ms = ms;
    harp_mutex_unlock(&u->mu);
}

/* ---------------- link polling (event echoes) ---------------- */

bool harp_usb_link_poll(harp_io *io, unsigned timeout_ms) {
    usb_io *u = (usb_io *)io;
    harp_mutex_lock(&u->mu);
    if (!fifo_used(&u->link_fifo) && !u->dead && !u->closing) {
        uint64_t dl = harp_deadline_ms(timeout_ms ? timeout_ms : 1);
        while (!fifo_used(&u->link_fifo) && !u->dead && !u->closing)
            if (harp_cond_timedwait(&u->cv, &u->mu, dl)) break;
    }
    bool have = fifo_used(&u->link_fifo) > 0;
    harp_mutex_unlock(&u->mu);
    return have; /* data already buffered is served even on a dying
                    transport; the recv that follows reports death */
}

size_t harp_usb_link_pending(harp_io *io) {
    usb_io *u = (usb_io *)io;
    harp_mutex_lock(&u->mu);
    size_t n = fifo_used(&u->link_fifo);
    harp_mutex_unlock(&u->mu);
    return n;
}

/* ---------------- discovery / open / close ---------------- */

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

static bool start_in_xfers(usb_io *u, in_xfer *arr, int n, uint8_t ep, int is_audio) {
    for (int i = 0; i < n; i++) {
        arr[i].u = u;
        arr[i].is_audio = is_audio;
        arr[i].xfer = libusb_alloc_transfer(0);
        if (!arr[i].xfer) return false;
        libusb_fill_bulk_transfer(arr[i].xfer, u->h, ep, arr[i].buf,
                                  sizeof arr[i].buf, in_cb, &arr[i], 1000);
        if (libusb_submit_transfer(arr[i].xfer) != 0) return false;
        u->inflight++;
    }
    return true;
}

static void teardown(usb_io *u) {
    /* cancel everything pending, reap on this thread, then stop the loop */
    harp_mutex_lock(&u->mu);
    u->closing = 1;
    harp_cond_broadcast(&u->cv);
    for (int i = 0; i < USB_LINK_IN_XFERS; i++)
        if (u->link_in[i].xfer) libusb_cancel_transfer(u->link_in[i].xfer);
    for (int i = 0; i < USB_AUDIO_IN_XFERS; i++)
        if (u->audio_in[i].xfer) libusb_cancel_transfer(u->audio_in[i].xfer);
    for (int i = 0; i < USB_AOUT_SLOTS; i++)
        if (u->aout[i].busy) libusb_cancel_transfer(u->aout[i].xfer);
    harp_mutex_unlock(&u->mu);

    /* the event thread reaps the cancellations; wait for inflight == 0 */
    harp_mutex_lock(&u->mu);
    uint64_t dl = harp_deadline_ms(2000);
    while (u->inflight > 0)
        if (harp_cond_timedwait(&u->cv, &u->mu, dl)) break;
    harp_mutex_unlock(&u->mu);

    harp_flag_store_rel(&u->ev_running, 0);
    if (u->evthread) harp_thread_join(u->evthread);

    for (int i = 0; i < USB_LINK_IN_XFERS; i++)
        if (u->link_in[i].xfer) libusb_free_transfer(u->link_in[i].xfer);
    for (int i = 0; i < USB_AUDIO_IN_XFERS; i++)
        if (u->audio_in[i].xfer) libusb_free_transfer(u->audio_in[i].xfer);
    for (int i = 0; i < USB_AOUT_SLOTS; i++)
        if (u->aout[i].xfer) libusb_free_transfer(u->aout[i].xfer);
}

harp_io *harp_usb_open(void) { return harp_usb_open_match(NULL, false, 0, 0); }
harp_io *harp_usb_open_serial(const char *want) {
    return harp_usb_open_match(want, false, 0, 0);
}

/* Open+claim the first HARP device matching the multi-device selection
 * policy (see usb_io.h). Claiming is the mutual-exclusion primitive: a
 * device already owned by another instance fails libusb_claim_interface
 * and the loop advances, so racing fresh instances get distinct devices.
 *   want_serial != NULL : bind exactly that serial.
 *   want_vp == true     : require USB vid:pid == (want_vid, want_pid).
 *   both unset          : first unclaimed HARP device of any model. */
/* Persistent libusb context for a long-lived caller (the plugin supervisor):
 * create once, reuse across many open/close cycles, destroy once at teardown.
 * Per-attempt libusb_init/exit churn is fragile on the Windows backend (it
 * spins the whole backend up and down each time) and was the prime suspect
 * for unclean process shutdown on a device-less host. Opaque void* so callers
 * need not include libusb.h. NULL on failure. */
void *harp_usb_ctx_create(void) {
    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) != 0) {
        fprintf(stderr, "harp-usb: libusb init failed\n");
        return NULL;
    }
    return ctx;
}
void harp_usb_ctx_destroy(void *ctx) {
    if (ctx) libusb_exit((libusb_context *)ctx);
}

/* Core open: u->ctx and u->owns_ctx are preset by the caller. Enumerate the
 * bus, claim a matching device, return a transport. On failure free u, exiting
 * the context only if we own it. */
static harp_io *usb_open_core(usb_io *u, const char *want, bool want_vp,
                             uint16_t want_vid, uint16_t want_pid) {
    harp_mutex_init(&u->mu);
    harp_cond_init(&u->cv);
    u->debug = getenv("HARP_USB_DEBUG") != NULL;
    u->sync_audio = getenv("HARP_USB_SYNC_AUDIO") != NULL;
    /* RT on by default; HARP_USB_RT=0 keeps the old USER_INTERACTIVE QoS. */
    {
        const char *rt = getenv("HARP_USB_RT");
        u->rt_enable = !(rt && rt[0] == '0');
        const char *pe = getenv("HARP_USB_RT_PERIOD_US");
        double us = pe ? atof(pe) : 0.0;
        u->rt_period_ns = (us > 0 ? us : HARP_RT_PERIOD_US_DEFAULT) * 1000.0;
    }
    if (getenv("HARP_USB_RTSTATS")) u->st = stats_alloc(); /* NULL = capture off */
    u->link_fifo.buf = malloc(LINK_FIFO_SZ);
    u->link_fifo.cap = LINK_FIFO_SZ;
    u->audio_fifo.buf = malloc(AUDIO_FIFO_SZ);
    u->audio_fifo.cap = AUDIO_FIFO_SZ;
    if (!u->link_fifo.buf || !u->audio_fifo.buf) goto fail;

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
        char serial[64] = "?";
        if (dd.iSerialNumber)
            libusb_get_string_descriptor_ascii(u->h, dd.iSerialNumber,
                                               (unsigned char *)serial, sizeof serial);
        if (!harp_usb_dev_matches(serial, dd.idVendor, dd.idProduct,
                                  want, want_vp, want_vid, want_pid)) {
            libusb_close(u->h); /* wrong serial or wrong model — never bind a different synth */
            u->h = NULL;
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
        u->vendor_id = dd.idVendor;
        u->product_id = dd.idProduct;
        snprintf(u->dev_serial, sizeof u->dev_serial, "%s", serial);
        u->io.read_exact = usb_read_exact;
        u->io.write_all = usb_write_all;
        u->io.corrupt_pct = 0; /* §8.7 fault injection is DEVICE-only (uninit guard) */
        u->io.readable = NULL; /* host side never runs the device consume loop (uninit guard) */

        /* OUT slots pre-allocated; IN transfers posted before anything
         * else can write — "inbound read pending" from the first byte */
        for (int s = 0; s < USB_AOUT_SLOTS; s++) {
            u->aout[s].u = u;
            u->aout[s].xfer = libusb_alloc_transfer(0);
            if (!u->aout[s].xfer) goto fail_open;
        }
        harp_flag_store_rel(&u->ev_running, 1);
        if (harp_thread_create(&u->evthread, ev_main, u) != 0) goto fail_open;
        if (!start_in_xfers(u, u->link_in, USB_LINK_IN_XFERS, ep_in, 0)) goto fail_started;
        if (ep_ain && !u->sync_audio &&
            !start_in_xfers(u, u->audio_in, USB_AUDIO_IN_XFERS, ep_ain, 1))
            goto fail_started;

        fprintf(stderr,
                "harp-usb: claimed %04x:%04x serial %s (iface %d, link %02x/%02x, "
                "audio %02x/%02x, async)\n",
                dd.idVendor, dd.idProduct, serial, iface, ep_in, ep_out, ep_ain,
                ep_aout);
        libusb_free_device_list(list, 1);
        return &u->io;

    fail_started:
        libusb_free_device_list(list, 1);
        teardown(u);
        goto fail_open_after_list;
    fail_open:
        libusb_free_device_list(list, 1);
    fail_open_after_list:
        if (u->h) {
            libusb_release_interface(u->h, u->iface);
            libusb_close(u->h);
        }
        goto fail_no_list;
    }
    libusb_free_device_list(list, 1);
    if (want)
        fprintf(stderr, "harp-usb: no HARP device with serial %s on the bus\n", want);
    else if (want_vp)
        fprintf(stderr, "harp-usb: no unclaimed HARP device of model %04x:%04x\n",
                want_vid, want_pid);
    else
        fprintf(stderr, "harp-usb: no HARP device on the bus (class FF/48/01 scan)\n");
fail:
fail_no_list:
    stats_free(u->st);
    free(u->link_fifo.buf);
    free(u->audio_fifo.buf);
    if (u->owns_ctx) libusb_exit(u->ctx);
    free(u);
    return NULL;
}

harp_io *harp_usb_open_match(const char *want, bool want_vp, uint16_t want_vid,
                             uint16_t want_pid) {
    usb_io *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    if (libusb_init(&u->ctx) != 0) {
        fprintf(stderr, "harp-usb: libusb init failed\n");
        free(u);
        return NULL;
    }
    u->owns_ctx = true; /* one-shot caller (harp-probe): own and free the ctx */
    return usb_open_core(u, want, want_vp, want_vid, want_pid);
}

harp_io *harp_usb_open_match_ctx(void *ctx, const char *want, bool want_vp,
                                 uint16_t want_vid, uint16_t want_pid) {
    if (!ctx) return NULL;
    usb_io *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->ctx = (libusb_context *)ctx;
    u->owns_ctx = false; /* borrowed from harp_usb_ctx_create; leave it intact */
    return usb_open_core(u, want, want_vp, want_vid, want_pid);
}

void harp_usb_close(harp_io *io) {
    if (!io) return;
    usb_io *u = (usb_io *)io;
    teardown(u);
    if (u->fifo_drops)
        fprintf(stderr, "harp-usb: %llu FIFO bytes dropped this session\n",
                (unsigned long long)u->fifo_drops);
    if (u->st) {
        stats_dump(u);
        stats_free(u->st);
        u->st = NULL;
    }
    if (u->h) {
        libusb_release_interface(u->h, u->iface);
        libusb_close(u->h);
    }
    free(u->link_fifo.buf);
    free(u->audio_fifo.buf);
    harp_mutex_destroy(&u->mu);
    harp_cond_destroy(&u->cv);
    if (u->owns_ctx) libusb_exit(u->ctx);
    free(u);
}

bool harp_usb_devident(harp_io *io, harp_usb_devinfo *out) {
    if (!io || !out) return false;
    usb_io *u = (usb_io *)io;
    out->vendor_id = u->vendor_id;
    out->product_id = u->product_id;
    snprintf(out->serial, sizeof out->serial, "%s", u->dev_serial);
    return true;
}

/* Map a LIBUSB_SPEED_* constant to the design CDDL usb-speed enum (so callers
 * need not include libusb.h to interpret harp_usb_topology.speed):
 *   0 unknown, 1 low, 2 full, 3 high, 4 super, 5 super-plus. */
static int map_usb_speed(int libusb_speed) {
    switch (libusb_speed) {
    case LIBUSB_SPEED_LOW:       return 1;
    case LIBUSB_SPEED_FULL:      return 2;
    case LIBUSB_SPEED_HIGH:      return 3;
    case LIBUSB_SPEED_SUPER:     return 4;
#ifdef LIBUSB_SPEED_SUPER_PLUS
    case LIBUSB_SPEED_SUPER_PLUS: return 5;
#endif
    default:                     return 0; /* LIBUSB_SPEED_UNKNOWN */
    }
}

/* §14.4 host-context-C: USB topology of the bound device (diag-bundle key 10).
 * READ-ONLY — only descriptor/topology queries against the already-open handle,
 * no transfer or FIFO state is touched, so the diag-bundle assembler can call it
 * off the control path while the audio threads stream. */
bool harp_usb_get_topology(harp_io *io, harp_usb_topology *out) {
    if (!io || !out) return false;
    usb_io *u = (usb_io *)io;
    memset(out, 0, sizeof *out);
    /* identity (VID/PID/serial) is always available from the bound descriptor. */
    out->vendor_id = u->vendor_id;
    out->product_id = u->product_id;
    snprintf(out->serial, sizeof out->serial, "%s", u->dev_serial);
    if (!u->h) return false; /* no live handle (should not happen post-open) */
    libusb_device *dev = libusb_get_device(u->h);
    if (!dev) return false;
    out->bus = libusb_get_bus_number(dev);
    out->addr = libusb_get_device_address(dev);
    out->speed = map_usb_speed(libusb_get_device_speed(dev));
    uint8_t ports[HARP_USB_MAX_PORTS];
    int n = libusb_get_port_numbers(dev, ports, (int)sizeof ports);
    if (n > 0) {
        out->nports = n;
        for (int i = 0; i < n && i < HARP_USB_MAX_PORTS; i++) out->ports[i] = ports[i];
    }
    /* Controller / root id: libusb has no portable controller-name API, so we
     * synthesize a stable per-host root identifier from the bus the device sits
     * on ("usb-bus-N"). It is the §16-anonymizable controller field (cleared to
     * "" in the anon pass); bus/addr/ports stay as the retained topology. */
    snprintf(out->controller, sizeof out->controller, "usb-bus-%u", (unsigned)out->bus);
    out->ok = true;
    return true;
}

/* Read-only scan of every HARP device on the bus. Opens each briefly to
 * read its serial string, then closes WITHOUT claiming — so it sees
 * devices already owned by other instances too (a claimed interface does
 * not prevent libusb_open). Its own short-lived context; enumeration is a
 * session-start rarity. */
size_t harp_usb_enumerate(harp_usb_devinfo *out, size_t cap) {
    libusb_context *ctx;
    if (libusb_init(&ctx) != 0) return 0;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    size_t found = 0;
    for (ssize_t i = 0; i < n; i++) {
        int iface;
        uint8_t ep[4] = {0}; /* endpoints unused here — we only want identity */
        if (!find_harp_interface(list[i], &iface, &ep[0], &ep[1], &ep[2], &ep[3]))
            continue;
        struct libusb_device_descriptor dd;
        libusb_get_device_descriptor(list[i], &dd);
        harp_usb_devinfo info = {dd.idVendor, dd.idProduct, "?"};
        libusb_device_handle *h;
        if (libusb_open(list[i], &h) == 0) {
            if (dd.iSerialNumber)
                libusb_get_string_descriptor_ascii(
                    h, dd.iSerialNumber, (unsigned char *)info.serial, sizeof info.serial);
            libusb_close(h);
        }
        if (found < cap) out[found] = info;
        found++;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found;
}

#endif /* HAVE_LIBUSB */
