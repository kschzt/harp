/* harp-probe — HARP host-side probe CLI (spec draft 0.3).
 *
 * Speaks the framed link over the TCP dev transport to harp-deviced (or, on
 * the Pi, to the daemon behind the USB gadget once that lands). Implements
 * the host side of `harp-core` + `harp-recall`: identity, refs, pull (save),
 * and the canonical archive-before-push restore of §11.4/§12.2.
 *
 *   harp-probe [-d HOST:PORT] [-s STOREDIR] CMD
 *     identify | refs | counters | params
 *     knob ID VALUE          simulate a front-panel edit
 *     save                   pull live/project into the local store ("save project")
 *     restore                push saved state back (archive-before-push, CAS)
 *     demo                   narrated end-to-end recall walkthrough
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sockhandle;
#  define SOCK_INVALID INVALID_SOCKET
#  define harp_closesock closesocket
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sockhandle;
#  define SOCK_INVALID (-1)
#  define harp_closesock close
#endif

#include "harp/audio.h"
#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/object.h"
#include "harp/plat.h"
#include "harp/store.h"
#include "client.h"
#include "usb_io.h"

#define EXPECT_REF "expected/live-project"
#define LIVE_REF "live/project"

/* harp_io over a TCP socket (the dev transport). Unlike core's fd-backed io,
 * this uses recv/send so it works on Winsock SOCKETs as well as POSIX fds —
 * read()/write() do not operate on Windows sockets. */
typedef struct {
    harp_io io;
    sockhandle s;
} sock_io;

static bool sock_read_exact(harp_io *io, void *buf, size_t n) {
    sock_io *t = (sock_io *)io;
    uint8_t *p = buf;
    while (n) {
        int r = recv(t->s, (char *)p, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
        if (r < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        if (r == 0) return false; /* peer closed */
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool sock_write_all(harp_io *io, const void *buf, size_t n) {
    sock_io *t = (sock_io *)io;
    const uint8_t *p = buf;
    while (n) {
        int r = send(t->s, (const char *)p, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
        if (r < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static void sock_io_init(sock_io *t, sockhandle s) {
    t->io.read_exact = sock_read_exact;
    t->io.write_all = sock_write_all;
    t->io.corrupt_pct = 0; /* §8.7 fault injection is DEVICE-only (uninit guard) */
    t->s = s;
}

typedef struct {
    harp_io *io;
    sock_io tcp; /* backing storage when the transport is a socket */
    harp_link link;
    harp_client client; /* shared protocol client (host/client.h) */
    harp_store store;
    bool verbose_ntf;
    /* §7.1 epoch-test: last time.epoch notification captured by probe_ntf */
    bool epoch_seen;
    uint32_t epoch_new, epoch_old, epoch_rate;
    uint64_t epoch_old_msc;
} probe;

static void die(const char *msg) {
    fprintf(stderr, "harp-probe: %s\n", msg);
    exit(1);
}

/* ---------------- transport ---------------- */

static sockhandle dial(const char *hostport) {
    char host[256];
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport) die("device address must be HOST:PORT");
    size_t hl = (size_t)(colon - hostport);
    if (hl >= sizeof host) die("host too long");
    memcpy(host, hostport, hl);
    host[hl] = 0;
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0) die("cannot resolve device host");
    sockhandle fd = SOCK_INVALID;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == SOCK_INVALID) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        harp_closesock(fd);
        fd = SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (fd == SOCK_INVALID) die("cannot connect to device");
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return fd;
}

/* ---------------- protocol (host/client.h does the work) ---------------- */

/* verbose demo narration: surface state.changed notifications */
static void probe_ntf(void *ud, const harp_env *e) {
    probe *p = ud;
    if (strcmp(e->method, "time.epoch") == 0 && e->has_body) {
        /* §7.1: {0 new-epoch, 1 new-rate-hz, 2 old-epoch, 3 old-msc-final} */
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t k, v;
                if (!harp_cdec_uint(&b, &k) || !harp_cdec_uint(&b, &v)) break;
                if (k == 0) p->epoch_new = (uint32_t)v;
                else if (k == 1) p->epoch_rate = (uint32_t)v;
                else if (k == 2) p->epoch_old = (uint32_t)v;
                else if (k == 3) p->epoch_old_msc = v;
            }
            p->epoch_seen = true;
        }
        return;
    }
    if (strcmp(e->method, "state.changed") != 0 || !p->verbose_ntf || !e->has_body)
        return;
    harp_cdec b;
    harp_cdec_init(&b, e->body, e->body_len);
    uint64_t n;
    if (!harp_cdec_map(&b, &n)) return;
    char name[HARP_REF_NAME_MAX] = "?";
    uint64_t gen = 0;
    bool dirty = false;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&b, &key)) return;
        if (key == 0) {
            const char *s;
            size_t sl;
            if (!harp_cdec_text(&b, &s, &sl) || sl >= sizeof name) return;
            memcpy(name, s, sl);
            name[sl] = 0;
        } else if (key == 2) {
            if (!harp_cdec_uint(&b, &gen)) return;
        } else if (key == 3) {
            if (!harp_cdec_bool(&b, &dirty)) return;
        } else {
            if (!harp_cdec_skip(&b)) return;
        }
    }
    printf("      ntf state.changed: %s gen=%llu dirty=%s\n", name,
           (unsigned long long)gen, dirty ? "true" : "false");
}

/* the probe's error policy: any failure is fatal with a readable line */
static void ck(probe *p, int rc) {
    if (rc == 0) return;
    if (rc == HARP_CLIENT_EDEV)
        fprintf(stderr, "harp-probe: device error '%s' on %s\n", p->client.err_code,
                p->client.err_method);
    else
        fprintf(stderr, "harp-probe: link failed (device gone?)\n");
    exit(1);
}

static void req_head(probe *p, harp_cbuf *out, const char *method, bool has_body) {
    harp_client_req_head(&p->client, out, method, has_body);
}

static harp_env request(probe *p, harp_cbuf *out, harp_cbuf *rsp_buf) {
    harp_env e;
    ck(p, harp_client_request(&p->client, out, rsp_buf, &e));
    return e;
}

/* ---------------- protocol ops ---------------- */

static harp_client_identity do_hello(probe *p) {
    harp_client_identity id;
    ck(p, harp_client_hello(&p->client, "harp-probe 0.1 (dev)", &id));
    return id;
}

#define MAX_REFS 64

static size_t get_refs(probe *p, harp_ref out[MAX_REFS]) {
    size_t count = 0;
    ck(p, harp_client_refs(&p->client, out, MAX_REFS, &count));
    return count;
}

static bool find_ref(harp_ref *refs, size_t n, const char *name, harp_ref *out) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(refs[i].name, name) == 0) {
            *out = refs[i];
            return true;
        }
    return false;
}

static void print_ref(const harp_ref *r) {
    char hex[2 * HARP_HASH_LEN + 1] = "(unborn)";
    if (!r->unborn) {
        harp_hash_hex(&r->hash, hex);
        hex[12] = 0; /* short display */
    }
    printf("  %-28s %-12s gen=%-6llu %s\n", r->name, hex, (unsigned long long)r->generation,
           r->dirty ? "DIRTY" : "clean");
}

/* device snapshot of live/project; returns new head */
static harp_hash remote_snapshot(probe *p, const char *msg) {
    harp_hash h;
    ck(p, harp_client_snapshot(&p->client, LIVE_REF, msg, &h));
    return h;
}

static void fetch_closure(probe *p, const harp_hash *root) {
    size_t fetched = 0;
    ck(p, harp_client_fetch_closure(&p->client, root, &fetched));
    if (fetched) printf("      fetched %zu object(s)\n", fetched);
}

static uint64_t refset(probe *p, const char *name, const harp_hash *expect /* NULL = unborn */,
                       const harp_hash *newh, bool create) {
    uint64_t gen = 0;
    ck(p, harp_client_refset(&p->client, name, expect, newh, create, false, &gen));
    return gen;
}

/* ---------------- audio recording (§8) ---------------- */

#ifdef HAVE_LIBUSB
/* Writes a 16-bit PCM WAV. Header fields and samples are emitted in host byte
 * order, which WAV requires to be little-endian — correct on every current
 * target (x86/ARM64 are LE); would need byte-swapping on a big-endian host. */
static void write_wav16(const char *path, const float *interleaved, size_t nframes,
                        uint32_t rate) {
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write wav");
    uint32_t data_len = (uint32_t)(nframes * 2 * 2);
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t riff = 36 + data_len;
    memcpy(hdr + 4, &riff, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16;
    memcpy(hdr + 16, &fmtlen, 4);
    uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
    uint32_t byterate = rate * 4;
    memcpy(hdr + 20, &pcm, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byterate, 4);
    memcpy(hdr + 32, &align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_len, 4);
    fwrite(hdr, 1, 44, f);
    for (size_t i = 0; i < nframes * 2; i++) {
        float v = interleaved[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)(v * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static void cmd_record(probe *p, double seconds, const char *path) {
    if (!harp_usb_has_audio(p->io))
        die("device exposes no HARP stream endpoints (record needs -d usb)");
    do_hello(p);

    /* discard stale stream bytes from a previous run — must happen BEFORE
     * audio.start: a free-running device streams immediately, so a
     * drain-until-quiet afterwards would never go quiet */
    {
        uint8_t junk[16384];
        while (harp_usb_audio_read(p->io, junk, sizeof junk, 50) > 0) {}
    }

    uint32_t rate = 48000, nsamples = 256;
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "audio.start", true);
    harp_cbor_map(&req, 5);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, rate);
    harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, nsamples);
    harp_cbor_uint(&req, 2);
    harp_cbor_uint(&req, 8); /* target depth, frames */
    harp_cbor_uint(&req, 3);
    harp_cbor_array(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, 4);
    harp_cbor_array(&req, 0);
    request(p, &req, &rsp);
    printf("      audio.start: 48 kHz float32, stereo, %u-sample blocks (free-running)\n",
           nsamples);

    size_t want_frames = (size_t)(seconds * rate);
    float *samples = malloc(want_frames * 2 * sizeof(float));
    if (!samples) die("oom");
    size_t got_frames = 0;

    /* stream reassembly: bulk transfers ≈ one audio frame each, but never
     * assume framing survives transport boundaries — accumulate and parse */
    uint8_t acc[65536];
    size_t acc_len = 0;
    uint64_t expect_ts = 0;
    bool have_ts = false;
    uint64_t lost_samples = 0, disconts = 0, frames = 0;
    uint64_t t0 = harp_now_ns(), t1;

    while (got_frames < want_frames) {
        int r = harp_usb_audio_read(p->io, acc + acc_len, (int)(sizeof acc - acc_len), 2000);
        if (r < 0) die("audio stream read failed");
        if (r == 0) die("audio stream went silent (2 s timeout)");
        acc_len += (size_t)r;
        size_t off = 0;
        while (acc_len - off >= HARP_AUDIO_HDR_LEN) {
            harp_audio_hdr h;
            if (!harp_audio_hdr_decode(acc + off, &h)) die("malformed audio frame header");
            size_t need = HARP_AUDIO_HDR_LEN + harp_audio_payload_len(&h);
            if (acc_len - off < need) break; /* partial frame: wait for more */
            if (have_ts && h.ts != expect_ts) {
                lost_samples += h.ts - expect_ts;
            }
            if (h.dirflags & HARP_AUDIO_DISCONT) disconts++;
            expect_ts = h.ts + h.nsamples;
            have_ts = true;
            frames++;
            size_t take = h.nsamples;
            if (got_frames + take > want_frames) take = want_frames - got_frames;
            memcpy(samples + got_frames * 2, acc + off + HARP_AUDIO_HDR_LEN,
                   take * 2 * sizeof(float));
            got_frames += take;
            off += need;
        }
        memmove(acc, acc + off, acc_len - off);
        acc_len -= off;
    }
    t1 = harp_now_ns();

    /* stop: send the request, then keep draining the stream until the device
     * thread parks — its writes must complete before it can see the stop */
    req_head(p, &req, "audio.stop", false);
    uint64_t stop_rid = p->client.next_rid;
    if (harp_link_send(p->io, HARP_STREAM_CTL, req.buf, req.len) != 0)
        die("link send failed");
    int quiet = 0;
    while (quiet < 3) {
        int r = harp_usb_audio_read(p->io, acc, sizeof acc, 100);
        if (r < 0) break;
        quiet = (r == 0) ? quiet + 1 : 0;
    }
    {
        harp_env stop_e;
        ck(p, harp_client_wait(&p->client, stop_rid, &rsp, &stop_e));
    }

    double wall = (double)(t1 - t0) / 1e9;
    double stream_secs = (double)got_frames / rate;
    double drift_ppm = (stream_secs / wall - 1.0) * 1e6;
    write_wav16(path, samples, got_frames, rate);
    printf("      captured %zu samples in %zu frames -> %s\n", got_frames, (size_t)frames,
           path);
    printf("      timestamp continuity: %llu lost samples, %llu discontinuity flags\n",
           (unsigned long long)lost_samples, (unsigned long long)disconts);
    printf("      device clock vs host clock: %+.1f ppm (two crystals, as §7.3 promises)\n",
           drift_ppm);
    free(samples);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}
/* Host-paced render (§8.3 mode 1): we own the SSI timeline. Pacing frames
 * (header-only, slots=0) name the exact sample ranges; the device renders
 * them with no clock of its own. Pipelined a few blocks deep — the mode
 * constrains WHAT is rendered, pipelining covers transport scheduling. */
static double render_host_paced(probe *p, size_t want_frames, uint32_t nsamples,
                                float *out) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "audio.start", true);
    harp_cbor_map(&req, 6);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 48000);
    harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, nsamples);
    harp_cbor_uint(&req, 2);
    harp_cbor_uint(&req, 8);
    harp_cbor_uint(&req, 3);
    harp_cbor_array(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, 4);
    harp_cbor_array(&req, 0);
    harp_cbor_uint(&req, 5);
    harp_cbor_uint(&req, 1); /* clock-mode: host-paced */
    request(p, &req, &rsp);

    /* discard any stale stream bytes from a previous run */
    uint8_t acc[65536];
    while (harp_usb_audio_read(p->io, acc, sizeof acc, 50) > 0) {}

    const int AHEAD = 4;
    uint64_t ssi_sent = 0, frames_sent = 0, frames_recv = 0;
    size_t total_blocks = (want_frames + nsamples - 1) / nsamples;
    size_t got_frames = 0, acc_len = 0;
    uint64_t expect_ts = 0;
    uint64_t t0 = harp_now_ns(), t1;

    int dry_spins = 0;
    /* Effective pipeline depth: start optimistic, learn the device's real
     * limit. A pacing-write timeout means the device is blocked mid-response
     * waiting for US to read (the audio-pair flavor of the link's IN/OUT
     * deadlock) — cap in-flight there and never burn the timeout again. */
    uint64_t ahead = AHEAD;
    while (got_frames < want_frames) {
        while (frames_sent < total_blocks && frames_sent - frames_recv < ahead) {
            harp_audio_hdr pace = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D, 0, 0,
                                   ssi_sent, (uint16_t)nsamples, HARP_AUDIO_FMT_F32};
            uint8_t ph[HARP_AUDIO_HDR_LEN];
            harp_audio_hdr_encode(&pace, ph);
            if (!harp_usb_audio_write(p->io, ph, sizeof ph, 100)) {
                uint64_t in_flight = frames_sent - frames_recv;
                if (in_flight >= 1 && in_flight < ahead) ahead = in_flight;
                break;
            }
            ssi_sent += nsamples;
            frames_sent++;
            dry_spins = 0;
        }
        int r = harp_usb_audio_read(p->io, acc + acc_len, (int)(sizeof acc - acc_len), 1000);
        if (r < 0) die("render stream read failed");
        if (r == 0 && ++dry_spins > 10) die("render stream stalled (10 s)");
        acc_len += (size_t)r;
        size_t off = 0;
        while (acc_len - off >= HARP_AUDIO_HDR_LEN) {
            harp_audio_hdr h;
            if (!harp_audio_hdr_decode(acc + off, &h)) die("malformed render frame");
            size_t need = HARP_AUDIO_HDR_LEN + harp_audio_payload_len(&h);
            if (acc_len - off < need) break;
            if (h.ts != expect_ts) die("host-paced SSI mismatch — determinism broken");
            expect_ts = h.ts + h.nsamples;
            frames_recv++;
            size_t take = h.nsamples;
            if (got_frames + take > want_frames) take = want_frames - got_frames;
            memcpy(out + got_frames * 2, acc + off + HARP_AUDIO_HDR_LEN,
                   take * 2 * sizeof(float));
            got_frames += take;
            off += need;
        }
        memmove(acc, acc + off, acc_len - off);
        acc_len -= off;
    }
    t1 = harp_now_ns();

    req_head(p, &req, "audio.stop", false);
    uint64_t stop_rid = p->client.next_rid;
    if (harp_link_send(p->io, HARP_STREAM_CTL, req.buf, req.len) != 0)
        die("link send failed");
    int quiet = 0;
    while (quiet < 2) {
        int r = harp_usb_audio_read(p->io, acc, sizeof acc, 100);
        if (r < 0) break;
        quiet = (r == 0) ? quiet + 1 : 0;
    }
    {
        harp_env stop_e;
        ck(p, harp_client_wait(&p->client, stop_rid, &rsp, &stop_e));
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return (double)(t1 - t0) / 1e9;
}

static void cmd_render(probe *p, double seconds, const char *path) {
    if (!harp_usb_has_audio(p->io)) die("render needs -d usb");
    do_hello(p);
    size_t want = (size_t)(seconds * 48000);
    float *buf = malloc(want * 2 * sizeof(float));
    if (!buf) die("oom");
    double wall = render_host_paced(p, want, 256, buf);
    write_wav16(path, buf, want, 48000);
    printf("      host-paced render: %.2f s of audio in %.2f s wall — %.1fx real time\n",
           seconds, wall, seconds / wall);
    printf("      offline bounce through hardware: an ordinary feature, not a stunt (§8.3)\n");
    printf("      -> %s\n", path);
    free(buf);
}

static void cmd_t15(probe *p, double seconds) {
    if (!harp_usb_has_audio(p->io)) die("t15 needs -d usb");
    do_hello(p);
    size_t want = (size_t)(seconds * 48000);
    float *a = malloc(want * 2 * sizeof(float));
    float *b = malloc(want * 2 * sizeof(float));
    if (!a || !b) die("oom");
    printf("      T15: two host-paced renders of identical state and SSI range [0, %zu)\n",
           want);
    double w1 = render_host_paced(p, want, 256, a);
    double w2 = render_host_paced(p, want, 256, b);
    if (memcmp(a, b, want * 2 * sizeof(float)) == 0) {
        printf("      BYTE-IDENTICAL: %zu samples x 2ch x 4B compare equal "
               "(renders took %.2fs / %.2fs)\n",
               want, w1, w2);
        printf("      audio.deterministic holds — hardware behaving like a plugin\n");
    } else {
        size_t i = 0;
        while (i < want * 2 && a[i] == b[i]) i++;
        printf("      FAILED: first divergence at float index %zu\n", i);
    }
    free(a);
    free(b);
}
#endif

/* ---------------- commands ---------------- */

static void cmd_identify(probe *p) {
    harp_client_identity id = do_hello(p);
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&id.param_map_hash, hex);
    hex[16] = 0;
    printf("device:    %s %s\n", id.vendor, id.product);
    printf("serial:    %s\n", id.serial);
    printf("firmware:  %s   engine: %s %s\n", id.fw, id.engine_id, id.engine_ver);
    printf("param-map: %s…   boots: %llu\n", hex, (unsigned long long)id.boot_count);
    printf("caps:     ");
    for (size_t i = 0; i < id.ncaps; i++) printf(" %s", id.caps[i]);
    printf("\n");
}

/* §7.2 time correlation: bracket a time.ping with host monotonic stamps,
 * solve NTP-style for host<->device offset and its uncertainty. One-shot
 * here (the runtime MUST refine continuously — that loop is still TODO). */
static void cmd_ping(probe *p, int rounds) {
    do_hello(p);
    if (rounds < 1) rounds = 5;
    double best_unc = 1e18;
    long long best_off = 0, best_rtt = 0;
    for (int k = 0; k < rounds; k++) {
        harp_cbuf req, rsp;
        harp_cbuf_init(&req);
        harp_cbuf_init(&rsp);
        req_head(p, &req, "time.ping", false);
        uint64_t s = harp_now_ns();
        harp_env e = request(p, &req, &rsp);
        uint64_t r = harp_now_ns();
        long long t_send = (long long)(s / 1000);
        long long t_recv = (long long)(r / 1000);
        /* parse {0:[ep,recv_us], 1:[ep,xmit_us]} */
        long long d_recv = 0, d_xmit = 0;
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key, al, ep, us;
        if (e.has_body && harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                if (!harp_cdec_uint(&b, &key) || !harp_cdec_array(&b, &al) ||
                    !harp_cdec_uint(&b, &ep) || !harp_cdec_uint(&b, &us))
                    break;
                if (key == 0) d_recv = (long long)us;
                else if (key == 1) d_xmit = (long long)us;
            }
        }
        long long rtt = t_recv - t_send;
        long long turnaround = d_xmit - d_recv;
        /* offset = device_mid - host_mid; uncertainty = (rtt - turnaround)/2 */
        long long host_mid = t_send + rtt / 2;
        long long dev_mid = d_recv + turnaround / 2;
        double unc = (double)(rtt - turnaround) / 2.0;
        if (unc < best_unc) {
            best_unc = unc;
            best_off = dev_mid - host_mid;
            best_rtt = rtt;
        }
        harp_cbuf_free(&req);
        harp_cbuf_free(&rsp);
    }
    /* the absolute offset is between two unrelated monotonic origins
     * (host-boot vs device-boot) and is meaningless until a shared epoch
     * exists — RTT and uncertainty are the spec-relevant figures (§7.2) */
    (void)best_off;
    printf("time.ping x%d: best RTT %lld µs, correlation uncertainty ±%.1f µs (%s)\n",
           rounds, best_rtt, best_unc,
           best_unc < 250 ? "SHOULD-grade <250µs" : best_unc < 1000 ? "MUST-grade <1ms"
                                                                    : "OUT OF SPEC");
}

#ifdef HAVE_LIBUSB
/* enumerate every HARP device on the bus (no claim) — the multi-device
 * "what's plugged in" view */
static void cmd_list(void) {
    harp_usb_devinfo devs[16];
    size_t n = harp_usb_enumerate(devs, 16);
    printf("%zu HARP device(s) on the bus:\n", n);
    for (size_t i = 0; i < n && i < 16; i++)
        printf("  %04x:%04x  serial %s\n", devs[i].vendor_id, devs[i].product_id,
               devs[i].serial);
}
#endif

static void cmd_refs(probe *p) {
    do_hello(p);
    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    printf("%zu ref(s):\n", n);
    for (size_t i = 0; i < n; i++) print_ref(&refs[i]);
}

static void cmd_counters(probe *p) {
    do_hello(p);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "diag.counters", false);
    harp_env e = request(p, &req, &rsp);
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n;
    if (e.has_body && harp_cdec_map(&b, &n)) {
        for (uint64_t i = 0; i < n; i++) {
            const char *s;
            size_t sl;
            if (!harp_cdec_text(&b, &s, &sl)) break;
            printf("  %.*s = ", (int)sl, s);
            if (harp_cdec_peek(&b) == 0) {
                uint64_t v;
                harp_cdec_uint(&b, &v);
                printf("%llu\n", (unsigned long long)v);
            } else {
                int64_t v;
                harp_cdec_int(&b, &v);
                printf("%lld\n", (long long)v);
            }
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

/* §16 SEAM EXCEPTION (the host re-encodes device-section ONLY when anonymizing).
 * DECODE the device's device-section map {0 => identity, 1 => counters} and
 * RE-ENCODE it into `out`, clearing the identity PII leaves to "" IN PLACE while
 * preserving everything else byte-for-meaning. Cleared (per docs/diag-bundle-
 * design.md §16, lines 313-316/422 — the authoritative leaf list): identity key 2
 * (serial); vendor (key 0) key 1 (vendor name); product (key 1) key 1 (product
 * name); identity key 9 (build-id; may embed host/date); identity channel-map
 * (key 7) — for EACH entry map, keys 2/3/4 (name/group/path). PRESERVED verbatim:
 * vid/pid, firmware, engine (incl. engine-id + param-map-hash), protocol,
 * latency-profile, boot count, ump-group-map, part count, caps, and per
 * channel-map entry the slot index (key 0) / direction (key 1) / host-paced flag
 * (key 5) plus the array length and order — and the entire counters map
 * (device-section key 1: no PII). Subtrees that need no editing are copied via
 * harp_cdec_span so they survive byte-for-byte; only the listed leaf texts are
 * rewritten to "". Returns false on a malformed section (caller falls back). */
static bool anonymize_device_section(harp_cbuf *out, const uint8_t *sec, size_t len) {
    harp_cdec d;
    harp_cdec_init(&d, sec, len);
    uint64_t nsec;
    if (!harp_cdec_map(&d, &nsec)) return false;
    harp_cbor_map(out, nsec);
    for (uint64_t i = 0; i < nsec; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        harp_cbor_uint(out, key);
        if (key != 0) {
            /* counters (key 1) + any future device-section member: no PII,
             * copy the value verbatim. */
            const uint8_t *span;
            size_t sl;
            if (!harp_cdec_span(&d, &span, &sl)) return false;
            harp_cbuf_put(out, span, sl);
            continue;
        }
        /* key 0 => identity: re-encode, clearing serial + vendor/product names. */
        uint64_t nid;
        if (!harp_cdec_map(&d, &nid)) return false;
        harp_cbor_map(out, nid);
        for (uint64_t j = 0; j < nid; j++) {
            uint64_t ik;
            if (!harp_cdec_uint(&d, &ik)) return false;
            harp_cbor_uint(out, ik);
            if (ik == 0 || ik == 1) {
                /* vendor (key 0) / product (key 1) sub-map { 0 => id, 1 => name };
                 * clear key 1 (name) to "", preserve key 0 (vid/pid) and any
                 * other member verbatim. */
                uint64_t nsub;
                if (!harp_cdec_map(&d, &nsub)) return false;
                harp_cbor_map(out, nsub);
                for (uint64_t s = 0; s < nsub; s++) {
                    uint64_t sk;
                    if (!harp_cdec_uint(&d, &sk)) return false;
                    harp_cbor_uint(out, sk);
                    if (sk == 1) {
                        if (!harp_cdec_skip(&d)) return false; /* drop the name */
                        harp_cbor_text(out, "");               /* "" in place (§16) */
                    } else {
                        const uint8_t *span;
                        size_t sl;
                        if (!harp_cdec_span(&d, &span, &sl)) return false;
                        harp_cbuf_put(out, span, sl);
                    }
                }
            } else if (ik == 2 || ik == 9) {
                /* serial (key 2) and build-id (key 9, may embed host/date):
                 * cleared to "" in place (§16). */
                if (!harp_cdec_skip(&d)) return false;
                harp_cbor_text(out, "");
            } else if (ik == 7) {
                /* channel-map (key 7): an array of entry maps. For EACH entry,
                 * clear keys 2 (name) / 3 (group) / 4 (path) to "" in place,
                 * preserving the slot index (key 0), direction (key 1), host-
                 * paced flag (key 5), and the array length + order (§16). */
                uint64_t nent;
                if (!harp_cdec_array(&d, &nent)) return false;
                harp_cbor_array(out, nent);
                for (uint64_t en = 0; en < nent; en++) {
                    uint64_t nek;
                    if (!harp_cdec_map(&d, &nek)) return false;
                    harp_cbor_map(out, nek);
                    for (uint64_t ek = 0; ek < nek; ek++) {
                        uint64_t ekey;
                        if (!harp_cdec_uint(&d, &ekey)) return false;
                        harp_cbor_uint(out, ekey);
                        if (ekey == 2 || ekey == 3 || ekey == 4) {
                            if (!harp_cdec_skip(&d)) return false; /* drop name/group/path */
                            harp_cbor_text(out, "");               /* "" in place (§16) */
                        } else {
                            const uint8_t *span;
                            size_t sl;
                            if (!harp_cdec_span(&d, &span, &sl)) return false;
                            harp_cbuf_put(out, span, sl);
                        }
                    }
                }
            } else {
                /* fw/engine/protocol/latency-profile/boot/ump-map/part-count:
                 * preserved byte-for-meaning. */
                const uint8_t *span;
                size_t sl;
                if (!harp_cdec_span(&d, &span, &sl)) return false;
                harp_cbuf_put(out, span, sl);
            }
        }
    }
    return !d.err;
}

/* §14.4 diag-bundle — CAP-GATED. After do_hello the host scans the identity
 * capabilities for "diag.bundle". IF PRESENT (the eth-agent device-assembled
 * path): the host issues `req diag.bundle` and embeds the device's response body
 * — the FULL device-section (identity keys 0-12 incl channel-map + latency-
 * profile, plus counters) — VERBATIM under top key 4 (the byte-identical embed
 * is the device conformance gate). IF ABSENT: the v0 host-synth path below
 * re-encodes a SUBSET identity (keys 0-4,6) from harp_client_identity and marks
 * it host-synthesized in bundle-meta key 1. --anonymize runs the §16 host pass:
 * it clears identity serial / vendor-name / product-name to the empty string IN
 * PLACE (not omission, not "[redacted]") and sets key 3=true, retaining
 * vid/pid/param-map-hash/engine-id/caps (reveal whether, not what). On the
 * device-assembled path anonymize is the seam exception: the host DECODES and
 * re-encodes the device-section (anonymize_device_section) instead of embedding
 * verbatim — the byte-identical gate holds only on the non-anonymized bundle. */
static void cmd_diag_bundle(probe *p, const char *outfile, bool anon) {
    harp_client_identity id = do_hello(p);
    bool device_assembled = harp_client_has_cap(&id, "diag.bundle");

    /* The device-section. On the device-assembled path it is the device's verbatim
     * `rsp diag.bundle` body (full identity + counters). On the v0 host-synth path
     * we instead round-trip diag.counters and keep its body as device-section key 1
     * counters, re-encoding a subset identity ourselves below. */
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, device_assembled ? "diag.bundle" : "diag.counters", false);
    harp_env e = request(p, &req, &rsp);

    harp_cbuf out;
    harp_cbuf_init(&out);

    /* --- top map: 0 magic, 1 version, 2 bundle-meta, 3 anonymized?, 4 device-section --- */
    harp_cbor_map(&out, 5);

    harp_cbor_uint(&out, 0); /* magic (parallels recall-bundle "harpb") */
    harp_cbor_text(&out, "harpd");

    harp_cbor_uint(&out, 1); /* bundle schema version */
    harp_cbor_uint(&out, 1);

    harp_cbor_uint(&out, 2); /* bundle-meta: capture provenance */
    harp_cbor_map(&out, 2);
    harp_cbor_uint(&out, 0); /* tstamp: [epoch, msc-or-0] */
    harp_cbor_array(&out, 2);
    harp_cbor_uint(&out, (uint64_t)time(NULL)); /* wall-clock epoch */
    harp_cbor_uint(&out, 0);                    /* host MSC: 0 in v0 (no stream) */
    harp_cbor_uint(&out, 1); /* tool + provenance. The marker distinguishes the two paths:
                              * the DEVICE-ASSEMBLED section is the device's verbatim rsp
                              * diag.bundle (full identity, keys 0-12); the HOST-SYNTHESIZED
                              * one is a host stand-in carrying only the hello subset (keys
                              * 0-4,6 — no channel-map/latency-profile/protocol). Consumers
                              * key off this string. */
    harp_cbor_text(&out, device_assembled
                             ? "harp-probe (device-assembled device-section)"
                             : "harp-probe v0 (host-synthesized device-section)");

    harp_cbor_uint(&out, 3); /* anonymized? — first-class privacy-state flag */
    harp_cbor_bool(&out, anon);

    harp_cbor_uint(&out, 4); /* device-section */
    if (device_assembled) {
        /* The device assembled the full device-section. Non-anonymized: embed the
         * rsp body VERBATIM (byte-identical — the conformance gate; do NOT
         * re-encode). Anonymized: the §16 seam exception — decode + re-encode,
         * clearing the identity PII leaves while preserving structure + counters. */
        if (!(e.has_body && e.body_len))
            die("device advertised diag.bundle but returned no device-section");
        if (anon) {
            if (!anonymize_device_section(&out, e.body, e.body_len))
                die("malformed device-section (anonymize decode failed)");
        } else {
            harp_cbuf_put(&out, e.body, e.body_len);
        }
    } else {
        /* v0 HOST-SYNTH path (unchanged): identity (subset) + counters. */
        harp_cbor_map(&out, 2);

        /* device-section key 0 => identity, re-encoded mirroring encode_identity
         * (device/session.c:176) key shape. Under --anonymize the serial / vendor
         * name / product name become "" IN PLACE (§16); vid/pid/hash/engine-id/caps
         * are retained. */
        harp_cbor_uint(&out, 0);
        harp_cbor_map(&out, 6);
        harp_cbor_uint(&out, 0); /* vendor { 0 => vid, 1 => name } */
        harp_cbor_map(&out, 2);
        harp_cbor_uint(&out, 0);
        harp_cbor_uint(&out, id.vendor_id);
        harp_cbor_uint(&out, 1);
        harp_cbor_text(&out, anon ? "" : id.vendor);
        harp_cbor_uint(&out, 1); /* product { 0 => pid, 1 => name } */
        harp_cbor_map(&out, 2);
        harp_cbor_uint(&out, 0);
        harp_cbor_uint(&out, id.product_id);
        harp_cbor_uint(&out, 1);
        harp_cbor_text(&out, anon ? "" : id.product);
        harp_cbor_uint(&out, 2); /* serial (§16: cleared to "" under anon) */
        harp_cbor_text(&out, anon ? "" : id.serial);
        harp_cbor_uint(&out, 3); /* firmware */
        harp_cbor_text(&out, id.fw);
        harp_cbor_uint(&out, 4); /* engine { 0 => id, 1 => ver, 2 => param-map-hash } */
        harp_cbor_map(&out, 3);
        harp_cbor_uint(&out, 0);
        harp_cbor_text(&out, id.engine_id); /* engine-id retained (class id, not a name) */
        harp_cbor_uint(&out, 1);
        harp_cbor_text(&out, id.engine_ver);
        harp_cbor_uint(&out, 2);
        harp_cbor_bytes(&out, id.param_map_hash.b, HARP_HASH_LEN); /* hash always retained */
        harp_cbor_uint(&out, 6); /* capabilities array (retained) */
        harp_cbor_array(&out, id.ncaps);
        for (size_t k = 0; k < id.ncaps; k++) harp_cbor_text(&out, id.caps[k]);

        /* device-section key 1 => counters, embedded VERBATIM (the byte-identical
         * device conformance gate). Counters carry no PII, so they stay verbatim
         * under --anonymize too — only identity leaves change. */
        harp_cbor_uint(&out, 1);
        if (e.has_body && e.body_len)
            harp_cbuf_put(&out, e.body, e.body_len);
        else
            harp_cbor_map(&out, 0); /* device returned no counters body */
    }

    if (out.oom) die("oom assembling diag bundle");

    /* --- write the bundle (write_wav16's fopen/fwrite/fclose pattern) --- */
    FILE *f = fopen(outfile, "wb");
    if (!f) die("cannot write diag bundle");
    fwrite(out.buf, 1, out.len, f);
    fclose(f);
    printf("wrote %zu bytes to %s%s (%s device-section)\n", out.len, outfile,
           anon ? " (anonymized)" : "",
           device_assembled ? "device-assembled" : "host-synthesized");

    harp_cbuf_free(&out);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static void cmd_params(probe *p) {
    do_hello(p);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.params", false);
    harp_env e = request(p, &req, &rsp);
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n, key, alen;
    if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
        key == 0 && harp_cdec_array(&b, &alen)) {
        for (uint64_t i = 0; i < alen; i++) {
            uint64_t three, id;
            const char *s;
            size_t sl;
            double v;
            if (!harp_cdec_array(&b, &three) || three != 3 || !harp_cdec_uint(&b, &id) ||
                !harp_cdec_text(&b, &s, &sl) || !harp_cdec_float(&b, &v))
                break;
            printf("  [%llu] %-16.*s %.3f\n", (unsigned long long)id, (int)sl, s, v);
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

/* §9.9 live output meters: x.harp-refdev.meters -> {0: [ [slot, peak, rms], ...]}
 * slot 0..15 = parts, 16 = main mix. */
static void cmd_meters(probe *p) {
    do_hello(p);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.meters", false);
    harp_env e = request(p, &req, &rsp);
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n, key, alen;
    if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) &&
        key == 0 && harp_cdec_array(&b, &alen)) {
        for (uint64_t i = 0; i < alen; i++) {
            uint64_t three, slot;
            double peak, rms;
            if (!harp_cdec_array(&b, &three) || three != 3 || !harp_cdec_uint(&b, &slot) ||
                !harp_cdec_float(&b, &peak) || !harp_cdec_float(&b, &rms))
                break;
            if (slot == 16)
                printf("  main     peak %.4f  rms %.4f\n", peak, rms);
            else
                printf("  part %-2llu  peak %.4f  rms %.4f\n", (unsigned long long)slot, peak, rms);
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static void cmd_knob(probe *p, uint64_t id, double v) {
    do_hello(p);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.knob", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, id);
    harp_cbor_uint(&req, 1);
    harp_cbor_float(&req, v);
    request(p, &req, &rsp);
    printf("knob %llu -> %.3f (device live state is now dirty)\n", (unsigned long long)id, v);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

/* save = Pull (§11.4 action 2) */
static void do_save(probe *p) {
    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    harp_ref live;
    if (!find_ref(refs, n, LIVE_REF, &live) || live.unborn)
        die("device has no live/project state");
    harp_hash head = live.hash;
    if (live.dirty) {
        printf("      live state is dirty -> snapshot-on-demand before pull (§10.4)\n");
        head = remote_snapshot(p, "host pull");
    }
    fetch_closure(p, &head);
    harp_ref expected = {0};
    snprintf(expected.name, sizeof expected.name, "%s", EXPECT_REF);
    expected.unborn = false;
    expected.hash = head;
    expected.generation = live.generation;
    expected.dirty = false;
    if (harp_store_ref_write(&p->store, &expected) != 0) die("cannot write local bundle ref");
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&head, hex);
    hex[12] = 0;
    printf("      project saved: live/project @ %s…\n", hex);
}

/* restore = the §12.2 reopen flow; Push with archive-before-push (§11.4 action 1) */
static void do_restore(probe *p) {
    harp_ref expected;
    if (harp_store_ref_read(&p->store, EXPECT_REF, &expected) != 0 || expected.unborn)
        die("no saved project in local store (run `save` first)");

    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    harp_ref live;
    if (!find_ref(refs, n, LIVE_REF, &live)) die("device has no live/project ref");

    if (!live.unborn && !live.dirty && harp_hash_eq(&live.hash, &expected.hash)) {
        printf("      hash match, not dirty -> SYNCED silently (zero dialogs, §12.2)\n");
        return;
    }
    printf("      mismatch%s -> resolving by Push with archive (§11.4)\n",
           live.dirty ? " + dirty live edits" : "");

    /* 1. archive the device's current state — never overwrite without it */
    harp_hash device_head = live.hash;
    if (live.dirty) device_head = remote_snapshot(p, "pre-push archive");
    char archive_name[HARP_REF_NAME_MAX];
    time_t now = time(NULL);
    struct tm tm;
    harp_gmtime(now, &tm);
    snprintf(archive_name, sizeof archive_name, "archive/%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             tm.tm_sec);
    refset(p, archive_name, NULL, &device_head, true);
    printf("      archived device state as %s (O(1): a pointer, §11.4)\n", archive_name);

    /* 2.+3. negotiate (have) and transfer (send) — re-sync stays
     * proportional to the diff */
    size_t sent = 0, already = 0;
    ck(p, harp_client_push_closure(&p->client, &expected.hash, &sent, &already));
    if (sent)
        printf("      transferred %zu missing object(s), %zu already on device\n", sent,
               already);
    else
        printf("      device already holds all %zu object(s)\n", already);

    /* 4. CAS the live ref: expect = post-archive head */
    uint64_t gen = refset(p, LIVE_REF, &device_head, &expected.hash, false);
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&expected.hash, hex);
    hex[12] = 0;
    printf("      live/project -> %s… (gen %llu) — recall complete\n", hex,
           (unsigned long long)gen);
}

/* cas-test (§11.3): exercises the recall negotiation's REJECTION + override paths
 * that demo/restore (the happy path) never hit — a CAS conflict, the force
 * override, and an incomplete-closure target. Needs an existing live/project ref
 * (run after `demo`). Exits 0 on all-pass, 1 on any mismatch. */
static void cmd_cas_test(probe *p) {
    do_hello(p);
    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    harp_ref live;
    if (!find_ref(refs, n, LIVE_REF, &live) || live.unborn)
        die("cas-test needs an existing live/project ref (run `demo` first)");
    harp_hash head = live.hash;                       /* the real, pushed current head */
    if (live.dirty) head = remote_snapshot(p, "cas-test baseline"); /* clean it first */
    printf("── cas-test: recall CAS conflict / force / not-found (§11.3)\n");

    int fails = 0;
    uint64_t gen = 0;

    /* (a) CONFLICT: a wrong `expect` (head with a flipped bit) against a valid,
     * already-pushed target must be REJECTED with code "conflict". */
    harp_hash bogus = head;
    bogus.b[HARP_HASH_LEN - 1] ^= 0xff; /* wrong digest, valid algorithm byte (§10.2) */
    int rc = harp_client_refset(&p->client, LIVE_REF, &bogus, &head, false, false, &gen);
    if (rc == 0 || strcmp(p->client.err_code, "conflict") != 0) {
        fprintf(stderr, "   FAIL (a): expected 'conflict', got rc=%d code='%s'\n", rc,
                rc ? p->client.err_code : "ok (accepted!)");
        fails++;
    } else
        printf("   (a) wrong expect -> conflict: OK\n");

    /* (b) FORCE overrides the mismatch: the SAME wrong expect with force succeeds
     * (the §11.4 "DAW wins, archive-before-push" override). */
    rc = harp_client_refset(&p->client, LIVE_REF, &bogus, &head, false, true, &gen);
    if (rc != 0) {
        fprintf(stderr, "   FAIL (b): force refset rejected rc=%d code='%s'\n", rc,
                p->client.err_code);
        fails++;
    } else
        printf("   (b) force overrides the mismatch: OK (gen %llu)\n",
               (unsigned long long)gen);

    /* (c) NOT-FOUND: a refset (force, to bypass the CAS) to a hash whose object
     * closure was never pushed must be REJECTED with code "not-found". */
    harp_hash absent;
    memset(absent.b, 0xab, sizeof absent.b);
    absent.b[0] = HARP_HASH_ALG_SHA256; /* valid algorithm byte; the digest is just never pushed */
    rc = harp_client_refset(&p->client, LIVE_REF, &head, &absent, false, true, &gen);
    if (rc == 0 || strcmp(p->client.err_code, "not-found") != 0) {
        fprintf(stderr, "   FAIL (c): expected 'not-found', got rc=%d code='%s'\n", rc,
                rc ? p->client.err_code : "ok (accepted!)");
        fails++;
    } else
        printf("   (c) unpushed closure -> not-found: OK\n");

    /* (d) MALFORMED (§10.2): an expect hash carrying an unknown algorithm byte
     * must be rejected as "malformed" — before any conflict/closure check. */
    harp_hash badalg = head;
    badalg.b[0] = 0x02; /* not 0x01 = SHA-256 */
    rc = harp_client_refset(&p->client, LIVE_REF, &badalg, &head, false, false, &gen);
    if (rc == 0 || strcmp(p->client.err_code, "malformed") != 0) {
        fprintf(stderr, "   FAIL (d): expected 'malformed', got rc=%d code='%s'\n", rc,
                rc ? p->client.err_code : "ok (accepted!)");
        fails++;
    } else
        printf("   (d) unknown hash-algorithm byte -> malformed: OK\n");

    if (fails) {
        fprintf(stderr, "CAS-TEST FAIL (%d of 4)\n", fails);
        exit(1);
    }
    printf("CAS-TEST PASS: conflict rejected, force overrides, unpushed -> not-found, "
           "bad-alg -> malformed\n");
}

/* version-test (§5.4): force a protocol-major mismatch and assert the device
 * replies 'incompatible' WITH its supported range, so a host can prompt for a
 * firmware/host update instead of failing opaquely. Driven by HARP_FORCE_PROTO_MAJOR
 * (set => expect rejection; unset => negative control: a current-major hello succeeds). */
static void cmd_version_test(probe *p) {
    const char *fm = getenv("HARP_FORCE_PROTO_MAJOR");
    printf("── version-test: protocol-major negotiation (§5.4)\n");
    harp_client_identity id;
    int rc = harp_client_hello(&p->client, "version-test 0.1", &id);
    if (fm && fm[0]) {
        if (rc != HARP_CLIENT_EINCOMPAT || strcmp(p->client.err_code, "incompatible") != 0) {
            fprintf(stderr, "VERSION-TEST FAIL: forced major %s -> expected incompatible, got rc=%d code='%s'\n",
                    fm, rc, rc ? p->client.err_code : "ok (accepted!)");
            exit(1);
        }
        if (p->client.incompat_major_min == 0 && p->client.incompat_major_max == 0) {
            fprintf(stderr, "VERSION-TEST FAIL: 'incompatible' carried no machine-readable supported range\n");
            exit(1);
        }
        printf("VERSION-TEST PASS: major-%s hello -> incompatible, device supports major [%u..%u] "
               "(host can prompt for a firmware/host update)\n",
               fm, p->client.incompat_major_min, p->client.incompat_major_max);
    } else {
        if (rc != 0) {
            fprintf(stderr, "VERSION-TEST FAIL (control): current-major hello rc=%d code='%s'\n",
                    rc, rc ? p->client.err_code : "?");
            exit(1);
        }
        printf("VERSION-TEST PASS (control): current-major hello accepted\n");
    }
}

/* --- §7.1/§8.6 time.epoch / stale-epoch test helpers --- */
static void audio_start_rtp(probe *p, uint64_t rate) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "audio.start", true);
    harp_cbor_map(&req, 4);
    harp_cbor_uint(&req, 0); harp_cbor_uint(&req, rate);  /* rate */
    harp_cbor_uint(&req, 1); harp_cbor_uint(&req, 256);   /* nsamples */
    harp_cbor_uint(&req, 5); harp_cbor_uint(&req, 0);     /* free-running */
    harp_cbor_uint(&req, 6); harp_cbor_uint(&req, 47900); /* RTP dest (dummy: nothing consumes it) */
    request(p, &req, &rsp);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static void audio_stop_probe(probe *p) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "audio.stop", false);
    request(p, &req, &rsp);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static uint64_t counter_u64(probe *p, const char *name) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "diag.counters", false);
    harp_env e = request(p, &req, &rsp);
    uint64_t out = UINT64_MAX, n;
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    if (e.has_body && harp_cdec_map(&b, &n)) {
        for (uint64_t i = 0; i < n; i++) {
            const char *s;
            size_t sl;
            if (!harp_cdec_text(&b, &s, &sl)) break;
            uint64_t v = 0;
            if (harp_cdec_peek(&b) == 0) {
                harp_cdec_uint(&b, &v);
            } else {
                int64_t iv = 0;
                harp_cdec_int(&b, &iv);
                v = (uint64_t)iv;
            }
            if (sl == strlen(name) && memcmp(s, name, sl) == 0) out = v;
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return out;
}

/* epoch-test (§7.1/§8.6): a sample-rate change opens a new clock epoch — the device
 * MUST bump it + emit ntf time.epoch {new-epoch,new-rate,old-epoch,old-msc-final}, and
 * MUST discard + count (evt_stale_epoch) events timestamped in a stale epoch. */
static void cmd_epoch_test(probe *p) {
    do_hello(p);
    printf("── epoch-test: time.epoch on rate change + stale-epoch discard (§7.1/§8.6)\n");

    audio_start_rtp(p, 48000);
    audio_stop_probe(p);
    p->epoch_seen = false;
    audio_start_rtp(p, 44100); /* rate change -> time.epoch (captured by probe_ntf during the request) */
    if (!p->epoch_seen) {
        fprintf(stderr, "EPOCH-TEST FAIL: no time.epoch notification on rate change\n");
        exit(1);
    }
    if (p->epoch_new != p->epoch_old + 1 || p->epoch_rate != 44100) {
        fprintf(stderr, "EPOCH-TEST FAIL: time.epoch new=%u old=%u rate=%u (want new=old+1, rate=44100)\n",
                p->epoch_new, p->epoch_old, p->epoch_rate);
        exit(1);
    }
    printf("   (a) rate 48000->44100 -> time.epoch new-epoch=%u old-epoch=%u new-rate=%u: OK\n",
           p->epoch_new, p->epoch_old, p->epoch_rate);
    /* old-msc-final MUST be the old stream's published final MSC — a positive, block-aligned
     * count (the 48000 stream rendered >=1 block of 256). 0 would mean msc_final was lost. */
    if (p->epoch_old_msc == 0 || p->epoch_old_msc % 256 != 0) {
        fprintf(stderr, "EPOCH-TEST FAIL: old-msc-final %llu not a positive multiple of nsamples (256)\n",
                (unsigned long long)p->epoch_old_msc);
        exit(1);
    }
    printf("       old-msc-final=%llu (positive, block-aligned): OK\n", (unsigned long long)p->epoch_old_msc);

    /* (b) an event timestamped in the OLD (now stale) epoch must be discarded + counted */
    uint64_t before = counter_u64(p, "evt_stale_epoch");
    harp_cbuf ev;
    harp_cbuf_init(&ev);
    harp_cbor_array(&ev, 3);
    harp_cbor_array(&ev, 2);
    harp_cbor_uint(&ev, p->epoch_old); /* stale epoch */
    harp_cbor_uint(&ev, 0);            /* msc */
    harp_cbor_uint(&ev, 1);            /* etype 1 = param set */
    harp_cbor_uint(&ev, 0);            /* param id (never applied: discarded on the stale epoch) */
    harp_link_send(p->io, HARP_STREAM_EVT, ev.buf, ev.len);
    harp_cbuf_free(&ev);
    uint64_t after = counter_u64(p, "evt_stale_epoch"); /* the counters request is ordered after the evt */
    if (before == UINT64_MAX || after != before + 1) {
        fprintf(stderr, "EPOCH-TEST FAIL: stale event not discarded+counted (evt_stale_epoch %llu -> %llu)\n",
                (unsigned long long)before, (unsigned long long)after);
        exit(1);
    }
    printf("   (b) event in stale epoch %u -> discarded + counted (evt_stale_epoch %llu -> %llu): OK\n",
           p->epoch_old, (unsigned long long)before, (unsigned long long)after);

    audio_stop_probe(p);
    printf("EPOCH-TEST PASS: time.epoch emitted on rate change; stale-epoch events discarded + counted\n");
}

/* ---- §9.6 event transactions ---- */

static bool txn_approx(double a, double b) {
    double d = a - b;
    return d < 0.02 && d > -0.02;
}

/* read one param value via x.harp-refdev.params; -1 if absent */
static double probe_param(probe *p, uint64_t want) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.params", false);
    harp_env e = request(p, &req, &rsp);
    double out = -1;
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n, key, alen;
    if (e.has_body && harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
        harp_cdec_array(&b, &alen)) {
        for (uint64_t i = 0; i < alen; i++) {
            uint64_t three, id;
            const char *s;
            size_t sl;
            double v;
            if (!harp_cdec_array(&b, &three) || three != 3 || !harp_cdec_uint(&b, &id) ||
                !harp_cdec_text(&b, &s, &sl) || !harp_cdec_float(&b, &v))
                break;
            if (id == want) out = v;
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return out;
}

/* raw param event { 0:id, 1:value, ?4:txn } at [epoch 0=now, msc] on the EVT stream */
static void send_param_evt(probe *p, uint64_t id, double value, uint64_t txn, uint64_t msc) {
    harp_cbuf ev;
    harp_cbuf_init(&ev);
    harp_cbor_array(&ev, 3);
    harp_cbor_array(&ev, 2);
    harp_cbor_uint(&ev, 0);
    harp_cbor_uint(&ev, msc);
    harp_cbor_uint(&ev, 1); /* etype 1 = param */
    harp_cbor_map(&ev, txn ? 3 : 2);
    harp_cbor_uint(&ev, 0);
    harp_cbor_uint(&ev, id);
    harp_cbor_uint(&ev, 1);
    harp_cbor_float(&ev, (float)value);
    if (txn) {
        harp_cbor_uint(&ev, 4);
        harp_cbor_uint(&ev, txn);
    }
    harp_link_send(p->io, HARP_STREAM_EVT, ev.buf, ev.len);
    harp_cbuf_free(&ev);
}

/* txn control event: etype 2 begin / 3 commit / 4 abort, body { 0: txn-id, ?1: [epoch,msc] } */
static void send_txn_evt(probe *p, uint64_t etype, uint64_t txn) {
    harp_cbuf ev;
    harp_cbuf_init(&ev);
    harp_cbor_array(&ev, 3);
    harp_cbor_array(&ev, 2);
    harp_cbor_uint(&ev, 0);
    harp_cbor_uint(&ev, 0); /* outer [epoch 0, msc 0] -> commit applies "now" (asap) */
    harp_cbor_uint(&ev, etype);
    harp_cbor_map(&ev, 1);
    harp_cbor_uint(&ev, 0);
    harp_cbor_uint(&ev, txn);
    harp_link_send(p->io, HARP_STREAM_EVT, ev.buf, ev.len);
    harp_cbuf_free(&ev);
}

/* read the §9.6 txn reject meters via x.harp-refdev.txn -> {0:rejected, 1:overflow, 2:unknown}.
 * These live OUTSIDE the §14.2 16-pair counters golden, so the reject paths are observable to a
 * test (a begin/submit/commit/abort that is supposed to be refused leaves a counted, readable mark
 * rather than a silent no-op). The CTL request is ordered after prior EVT sends on the one link. */
static void probe_txn_ctrs(probe *p, uint64_t *rej, uint64_t *ovf, uint64_t *unk) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.txn", false);
    harp_env e = request(p, &req, &rsp);
    *rej = *ovf = *unk = 0;
    harp_cdec b;
    harp_cdec_init(&b, e.body, e.body_len);
    uint64_t n;
    if (e.has_body && harp_cdec_map(&b, &n)) {
        for (uint64_t i = 0; i < n; i++) {
            uint64_t k, v;
            if (!harp_cdec_uint(&b, &k) || !harp_cdec_uint(&b, &v)) break;
            if (k == 0) *rej = v;
            else if (k == 1) *ovf = v;
            else if (k == 2) *unk = v;
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

#define TXN_SETTLE_NS 80000000ull /* ~15 render blocks @48k/256: the evq drains + applies */

/* §9.6: events tagged with a txn-id buffer until commit applies them atomically; abort discards.
 * Observable via the param readback — a buffered event NEVER reaches the evq, so it cannot apply
 * (timing-independent); after commit the whole batch is on the evq and the running stream applies
 * it. Needs a live stream (audio.start) so the render thread drains the evq. */
static void cmd_txn_test(probe *p) {
    harp_client_identity id = do_hello(p);
    printf("── txn-test: §9.6 event transactions (capability / atomic apply / abort / unknown)\n");

    if (!harp_client_has_cap(&id, "evt.txn")) {
        fprintf(stderr, "TXN-TEST FAIL: device does not advertise the evt.txn capability\n");
        exit(1);
    }
    printf("   capability evt.txn advertised: OK\n");
    if (id.txn_max < 1 || id.txn_events < 256) { /* §9.6 MUST: >= 1 concurrent, >= 256 events */
        fprintf(stderr, "TXN-TEST FAIL: txn limits (key 13) not reported / below spec min "
                "(concurrent=%llu events=%llu, MUST >= 1 / >= 256)\n",
                (unsigned long long)id.txn_max, (unsigned long long)id.txn_events);
        exit(1);
    }
    printf("   txn limits reported (identity key 13): concurrent=%llu, events=%llu (>= spec min 1/256): OK\n",
           (unsigned long long)id.txn_max, (unsigned long long)id.txn_events);

    audio_start_rtp(p, 48000);
    const uint64_t PID = 3;

    /* sanity: an UNTAGGED param set applies under our timing (the evq path works) */
    send_param_evt(p, PID, 0.90, 0, 0);
    harp_sleep_ns(TXN_SETTLE_NS);
    double base = probe_param(p, PID);
    if (!txn_approx(base, 0.90)) {
        fprintf(stderr, "TXN-TEST FAIL: untagged param set did not apply (%.3f != 0.90)\n", base);
        exit(1);
    }
    printf("   (sanity) untagged param set applied: %.3f\n", base);

    /* (1) ATOMIC: begin -> tagged set MUST NOT move the param (buffered) -> commit moves it */
    send_txn_evt(p, 2, 7);              /* txn-begin 7 */
    send_param_evt(p, PID, 0.20, 7, 0); /* tagged: buffered, must not apply */
    harp_sleep_ns(TXN_SETTLE_NS);
    double buffered = probe_param(p, PID);
    if (!txn_approx(buffered, 0.90)) {
        fprintf(stderr, "TXN-TEST FAIL: a buffered (uncommitted) event applied early (%.3f, want 0.90)\n",
                buffered);
        exit(1);
    }
    printf("   (1a) buffered event did NOT apply pre-commit: %.3f (still 0.90): OK\n", buffered);
    send_txn_evt(p, 3, 7); /* txn-commit 7 */
    harp_sleep_ns(TXN_SETTLE_NS);
    double committed = probe_param(p, PID);
    if (!txn_approx(committed, 0.20)) {
        fprintf(stderr, "TXN-TEST FAIL: commit did not apply the buffered event (%.3f, want 0.20)\n",
                committed);
        exit(1);
    }
    printf("   (1b) commit applied the buffered event atomically: %.3f: OK\n", committed);

    /* (2) ABORT: begin -> tagged set -> abort -> the param MUST stay (discarded) */
    send_txn_evt(p, 2, 8);
    send_param_evt(p, PID, 0.70, 8, 0);
    send_txn_evt(p, 4, 8); /* txn-abort 8 */
    harp_sleep_ns(TXN_SETTLE_NS);
    double aborted = probe_param(p, PID);
    if (!txn_approx(aborted, 0.20)) {
        fprintf(stderr, "TXN-TEST FAIL: an ABORTED txn applied (%.3f, want 0.20)\n", aborted);
        exit(1);
    }
    printf("   (2) abort discarded the buffered event: %.3f (still 0.20): OK\n", aborted);

    /* (3) UNKNOWN COMMIT: a never-begun id is a no-op + the device keeps serving */
    send_txn_evt(p, 3, 99);
    harp_sleep_ns(TXN_SETTLE_NS);
    double after_unknown = probe_param(p, PID);
    if (!txn_approx(after_unknown, 0.20)) {
        fprintf(stderr, "TXN-TEST FAIL: commit of an unknown txn changed state (%.3f)\n", after_unknown);
        exit(1);
    }
    printf("   (3) commit of an unknown txn-id: no-op, device still serving (%.3f): OK\n", after_unknown);

    /* (4) MULTI-EVENT ATOMICITY — the actual point of evq_push_batch: a txn buffering SEVERAL
     * distinct params applies them ALL on commit and NONE before. The single-event cases above
     * never exercise count>1, where a per-event push could partial-apply at the queue edge. */
    const uint64_t A = 1, B = 2, C = 4; /* three distinct params (NPARAMS=13); avoid PID=3 */
    send_param_evt(p, A, 0.10, 0, 0);
    send_param_evt(p, B, 0.10, 0, 0);
    send_param_evt(p, C, 0.10, 0, 0);
    harp_sleep_ns(TXN_SETTLE_NS);
    send_txn_evt(p, 2, 11); /* begin 11 */
    send_param_evt(p, A, 0.55, 11, 0);
    send_param_evt(p, B, 0.65, 11, 0);
    send_param_evt(p, C, 0.75, 11, 0);
    harp_sleep_ns(TXN_SETTLE_NS);
    double pa = probe_param(p, A), pb = probe_param(p, B), pc = probe_param(p, C);
    if (!txn_approx(pa, 0.10) || !txn_approx(pb, 0.10) || !txn_approx(pc, 0.10)) {
        fprintf(stderr, "TXN-TEST FAIL: a multi-event txn applied a param before commit "
                "(A=%.3f B=%.3f C=%.3f, want all 0.10)\n", pa, pb, pc);
        exit(1);
    }
    send_txn_evt(p, 3, 11); /* commit 11 */
    harp_sleep_ns(TXN_SETTLE_NS);
    pa = probe_param(p, A);
    pb = probe_param(p, B);
    pc = probe_param(p, C);
    if (!txn_approx(pa, 0.55) || !txn_approx(pb, 0.65) || !txn_approx(pc, 0.75)) {
        fprintf(stderr, "TXN-TEST FAIL: a multi-event commit did not apply the whole batch "
                "(A=%.3f want 0.55, B=%.3f want 0.65, C=%.3f want 0.75)\n", pa, pb, pc);
        exit(1);
    }
    printf("   (4) multi-event txn: 3 params buffered together, applied atomically on commit "
           "(%.2f/%.2f/%.2f): OK\n", pa, pb, pc);

    /* (5) BEGIN REJECTS — observed via the x.harp-refdev.txn meter (golden-free): opening one
     * past the advertised concurrent limit, a duplicate of an open id, and the reserved txn-id 0
     * are EACH refused + counted, without clobbering the open ones. */
    uint64_t rej0, ovf0, unk0;
    probe_txn_ctrs(p, &rej0, &ovf0, &unk0);
    for (uint64_t i = 0; i < id.txn_max; i++) send_txn_evt(p, 2, 100 + i); /* fill every slot */
    send_txn_evt(p, 2, 100 + id.txn_max);                                  /* one too many */
    send_txn_evt(p, 2, 100);                                               /* duplicate of an open id */
    send_txn_evt(p, 2, 0);                                                 /* reserved sentinel id 0 */
    harp_sleep_ns(TXN_SETTLE_NS);
    uint64_t rej1, ovf1, unk1;
    probe_txn_ctrs(p, &rej1, &ovf1, &unk1);
    if (rej1 - rej0 != 3) {
        fprintf(stderr, "TXN-TEST FAIL: begin rejects miscounted (delta=%llu, want 3: over-limit + "
                "duplicate + reserved-id-0)\n", (unsigned long long)(rej1 - rej0));
        exit(1);
    }
    printf("   (5) begin rejects: over-limit + duplicate + reserved-id-0 each counted (delta=3): OK\n");
    for (uint64_t i = 0; i < id.txn_max; i++) send_txn_evt(p, 4, 100 + i); /* abort: free the slots */
    harp_sleep_ns(TXN_SETTLE_NS);

    /* (6) UNKNOWN tag + abort: a submit or abort naming a never-open txn is swallowed + counted
     * (never half-applied), and leaks nothing into state. */
    uint64_t rej2, ovf2, unk2;
    probe_txn_ctrs(p, &rej2, &ovf2, &unk2);
    send_param_evt(p, PID, 0.42, 31337, 0); /* tag for an unknown txn -> swallowed */
    send_txn_evt(p, 4, 31337);              /* abort an unknown txn -> no-op */
    harp_sleep_ns(TXN_SETTLE_NS);
    uint64_t rej3, ovf3, unk3;
    probe_txn_ctrs(p, &rej3, &ovf3, &unk3);
    if (unk3 - unk2 != 2) {
        fprintf(stderr, "TXN-TEST FAIL: unknown-txn submit/abort not counted (delta=%llu, want 2)\n",
                (unsigned long long)(unk3 - unk2));
        exit(1);
    }
    if (!txn_approx(probe_param(p, PID), 0.20)) {
        fprintf(stderr, "TXN-TEST FAIL: an unknown-tagged event leaked into state (%.3f, want 0.20)\n",
                probe_param(p, PID));
        exit(1);
    }
    printf("   (6) unknown-txn submit + abort: swallowed + counted (delta=2), no state change: OK\n");

    /* (7) PER-TXN OVERFLOW: buffering one past the advertised per-txn event limit drops + counts
     * the extra event, and the txn still commits the events that fit. */
    uint64_t rej4, ovf4, unk4;
    probe_txn_ctrs(p, &rej4, &ovf4, &unk4);
    send_txn_evt(p, 2, 200); /* begin */
    for (uint64_t i = 0; i < id.txn_events + 1; i++) /* one past the advertised per-txn cap */
        send_param_evt(p, PID, 0.30, 200, 0);
    harp_sleep_ns(TXN_SETTLE_NS * 2);
    uint64_t rej5, ovf5, unk5;
    probe_txn_ctrs(p, &rej5, &ovf5, &unk5);
    if (ovf5 - ovf4 < 1) {
        fprintf(stderr, "TXN-TEST FAIL: over-limit buffering (%llu events into a %llu-cap txn) not "
                "counted (overflow delta=%llu)\n", (unsigned long long)(id.txn_events + 1),
                (unsigned long long)id.txn_events, (unsigned long long)(ovf5 - ovf4));
        exit(1);
    }
    send_txn_evt(p, 3, 200); /* commit the events that fit */
    harp_sleep_ns(TXN_SETTLE_NS);
    if (!txn_approx(probe_param(p, PID), 0.30)) {
        fprintf(stderr, "TXN-TEST FAIL: an over-limit txn did not commit the events that fit "
                "(%.3f, want 0.30)\n", probe_param(p, PID));
        exit(1);
    }
    printf("   (7) per-txn overflow: the %lluth event dropped + counted, txn still commits: OK\n",
           (unsigned long long)(id.txn_events + 1));

    audio_stop_probe(p);
    printf("TXN-TEST PASS: §9.6 events buffer under a txn, apply atomically on commit, discard on abort; "
           "evt.txn advertised\n");
}

/* notif-test (§5.2): the device MUST ignore unknown notifications. Send a well-formed
 * NOTIFICATION with an unknown method, then prove the device kept serving — a follow-up
 * request must still succeed (request() dies via ck() if the device crashed or errored). */
static void cmd_notif_test(probe *p) {
    do_hello(p);
    printf("── notif-test: device ignores unknown notifications (§5.2)\n");
    harp_cbuf ntf;
    harp_cbuf_init(&ntf);
    harp_env_head(&ntf, HARP_MSG_NOTIFICATION, 0, "x.unknown.notification", true);
    harp_cbor_map(&ntf, 1);
    harp_cbor_uint(&ntf, 0);
    harp_cbor_uint(&ntf, 12345);
    harp_link_send(p->io, HARP_STREAM_CTL, ntf.buf, ntf.len);
    harp_cbuf_free(&ntf);
    /* the device must ignore the notification and keep serving */
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "diag.counters", false);
    request(p, &req, &rsp);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    printf("NOTIF-TEST PASS: unknown notification ignored, device still serving\n");
}

/* --- §9.8 evt.format / evt.parse helpers + round-trip test --- */
static void evt_format(probe *p, uint64_t id, double val, char *out, size_t n) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "evt.format", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0); harp_cbor_uint(&req, id);
    harp_cbor_uint(&req, 1); harp_cbor_float(&req, val);
    harp_env e = request(p, &req, &rsp);
    out[0] = 0;
    if (e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t m, key;
        const char *s;
        size_t sl;
        if (harp_cdec_map(&b, &m) && m >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_text(&b, &s, &sl)) {
            if (sl >= n) sl = n - 1;
            memcpy(out, s, sl);
            out[sl] = 0;
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

static double evt_parse_val(probe *p, uint64_t id, const char *str) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "evt.parse", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0); harp_cbor_uint(&req, id);
    harp_cbor_uint(&req, 1); harp_cbor_text(&req, str);
    harp_env e = request(p, &req, &rsp);
    double val = -1;
    if (e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t m, key;
        if (harp_cdec_map(&b, &m) && m >= 1 && harp_cdec_uint(&b, &key) && key == 0)
            harp_cdec_float(&b, &val);
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return val;
}

/* format-test (§9.8): evt.format/evt.parse round-trip for a continuous param (Filter
 * Cutoff, id 3) and a stepped param (Arp Mode, id 9, enum labels). */
static void cmd_format_test(probe *p) {
    do_hello(p);
    printf("── format-test: evt.format/evt.parse round-trip (§9.8)\n");
    char s1[64];
    evt_format(p, 3, 0.5, s1, sizeof s1);
    double v1 = evt_parse_val(p, 3, s1);
    double diff = v1 - 0.5;
    if (diff < 0) diff = -diff;
    if (diff > 0.01) {
        fprintf(stderr, "FORMAT-TEST FAIL: continuous 0.5 -> '%s' -> %g\n", s1, v1);
        exit(1);
    }
    printf("   continuous (Filter Cutoff): 0.5 -> '%s' -> %g: OK\n", s1, v1);
    char s2[64], s3[64];
    evt_format(p, 9, 0.0, s2, sizeof s2);
    double v2 = evt_parse_val(p, 9, s2);
    evt_format(p, 9, v2, s3, sizeof s3);
    if (s2[0] == 0 || strcmp(s2, s3) != 0) {
        fprintf(stderr, "FORMAT-TEST FAIL: stepped '%s' -> %g -> '%s'\n", s2, v2, s3);
        exit(1);
    }
    printf("   stepped (Arp Mode): 0.0 -> '%s' -> %g -> '%s': OK\n", s2, v2, s3);
    printf("FORMAT-TEST PASS: evt.format/evt.parse round-trip (continuous + stepped)\n");
}

/* §5.5 core methods beyond hello/credit: ping (liveness echo), identify (re-fetch identity
 * with no session reset), core.changed (a D->H "re-query topic" hint — the refdev has no
 * spontaneous identity change, so we trigger it via the x.harp-refdev.notify-changed seam),
 * and an orderly core.bye. core.bye is LAST: the device closes its end after acking it. */
static void cmd_core_test(probe *p) {
    harp_client_identity id = do_hello(p);
    printf("── core-test: §5.5 core.ping / core.identify / core.changed / core.bye\n");

    if (harp_client_ping(&p->client) != 0) {
        fprintf(stderr, "CORE-TEST FAIL: core.ping did not echo the nonce (liveness)\n");
        exit(1);
    }
    printf("   core.ping: nonce echoed verbatim (liveness): OK\n");

    harp_client_identity id2;
    if (harp_client_identify(&p->client, &id2) != 0) {
        fprintf(stderr, "CORE-TEST FAIL: core.identify failed\n");
        exit(1);
    }
    bool differs = strcmp(id.serial, id2.serial) != 0 || strcmp(id.engine_id, id2.engine_id) != 0 ||
                   strcmp(id.engine_ver, id2.engine_ver) != 0 || strcmp(id.fw, id2.fw) != 0 ||
                   memcmp(&id.param_map_hash, &id2.param_map_hash, sizeof id.param_map_hash) != 0 ||
                   id2.ncaps != id.ncaps;
    for (size_t i = 0; !differs && i < id.ncaps; i++)
        if (strcmp(id.caps[i], id2.caps[i]) != 0) differs = true;
    if (differs) {
        fprintf(stderr, "CORE-TEST FAIL: core.identify identity differs from hello "
                "(serial '%s' vs '%s', engine '%s %s' vs '%s %s', caps %zu vs %zu)\n",
                id.serial, id2.serial, id.engine_id, id.engine_ver, id2.engine_id, id2.engine_ver,
                id.ncaps, id2.ncaps);
        exit(1);
    }
    printf("   core.identify: re-fetched identity matches hello (serial %s, %zu caps), no reset: OK\n",
           id2.serial, id2.ncaps);

    /* core.changed: trigger the device to emit core.changed{0:"identity"}. The device sends
     * the ntf BEFORE this seam's own response, so request() routes it to the client en route. */
    p->client.changed_pending = false;
    p->client.last_changed_topic[0] = 0;
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.notify-changed", true);
    harp_cbor_map(&req, 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, "identity");
    request(p, &req, &rsp);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (!p->client.changed_pending || strcmp(p->client.last_changed_topic, "identity") != 0) {
        fprintf(stderr, "CORE-TEST FAIL: core.changed{identity} not received (pending=%d topic='%s')\n",
                p->client.changed_pending, p->client.last_changed_topic);
        exit(1);
    }
    printf("   core.changed: device hint to re-query '%s' received: OK\n", p->client.last_changed_topic);

    if (harp_client_bye(&p->client) != 0) {
        fprintf(stderr, "CORE-TEST FAIL: core.bye was not acknowledged\n");
        exit(1);
    }
    printf("   core.bye: orderly session end acknowledged: OK\n");

    printf("CORE-TEST PASS: §5.5 core.ping/identify/changed/bye all conformant\n");
}

static void cmd_demo(probe *p, const char *addr) {
    (void)addr;
    p->verbose_ntf = true;
    printf("HARP recall walkthrough (spec §12.2 / §11.4) — device: harp-deviced\n\n");

    printf("[1] core.hello — identity, engine, capabilities\n");
    harp_client_identity id = do_hello(p);
    printf("      %s %s, serial %s, fw %s, engine %s %s\n\n", id.vendor, id.product,
           id.serial, id.fw, id.engine_id, id.engine_ver);

    printf("[2] state.refs — what does the device hold?\n");
    harp_ref refs[MAX_REFS];
    size_t n = get_refs(p, refs);
    for (size_t i = 0; i < n; i++) print_ref(&refs[i]);
    printf("\n[3] \"Save the DAW project\" -> Pull (§11.4)\n");
    do_save(p);

    printf("\n[4] The musician turns knobs on the front panel after the save…\n");
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(p, &req, "x.harp-refdev.knob", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 3);
    harp_cbor_uint(&req, 1);
    harp_cbor_float(&req, 0.83);
    request(p, &req, &rsp);
    req_head(p, &req, "x.harp-refdev.knob", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 4);
    harp_cbor_uint(&req, 1);
    harp_cbor_float(&req, 0.61);
    request(p, &req, &rsp);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    printf("      Filter Cutoff -> 0.83, Filter Reso -> 0.61\n");
    n = get_refs(p, refs);
    harp_ref live;
    if (find_ref(refs, n, LIVE_REF, &live)) print_ref(&live);

    printf("\n[5] \"Reopen the project\" -> mismatch detected -> Push with archive\n");
    do_restore(p);

    printf("\n[6] Verify: live/project matches the saved hash, clean;\n");
    printf("    the musician's tweaks survive in archive/:\n");
    n = get_refs(p, refs);
    for (size_t i = 0; i < n; i++) print_ref(&refs[i]);

    printf("\nEvery overwrite was preceded by a free snapshot. No dialogs on match,\n");
    printf("no silent loss on mismatch — the founding asymmetry of §11.4.\n");
}

/* §4.4.3: mDNS/DNS-SD discovery of `_harp._tcp` devices. A production host folds this into
 * its system device list; here it is a `discover` subcommand that browses the segment,
 * resolves each instance to host:port, and reads the TXT `proto` — proving the §4.4.3
 * round-trip end to end (and flagging a device that illegally leaks its serial in TXT, §16).
 * Built only where dns_sd is available (native on macOS, avahi-compat's libdns_sd on Linux);
 * elsewhere it degrades to a clear stub so the rest of harp-probe still builds. */
#ifdef HAVE_DNS_SD
#include <arpa/inet.h>
#include <dns_sd.h>
#include <sys/select.h>

static void disc_resolve_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t iface,
                            DNSServiceErrorType err, const char *fullname, const char *host,
                            uint16_t port, uint16_t txtlen, const unsigned char *txt, void *ctx) {
    (void)ref;
    (void)flags;
    (void)iface;
    (void)fullname;
    if (err != kDNSServiceErr_NoError) return;
    char proto[32] = "?";
    uint8_t plen = 0;
    const void *pv = TXTRecordGetValuePtr(txtlen, txt, "proto", &plen);
    if (pv && plen < sizeof proto) {
        memcpy(proto, pv, plen);
        proto[plen] = 0;
    }
    /* §16: the TXT MUST carry no serial — flag a device that leaks one. */
    uint8_t slen = 0;
    bool leak = TXTRecordGetValuePtr(txtlen, txt, "serial", &slen) != NULL;
    printf("  %s:%u\tproto=%s%s\n", host, ntohs(port), proto,
           leak ? "\t[!! serial leaked in TXT — §16 violation]" : "");
    *(int *)ctx += 1;
}

/* pump a DNSServiceRef's socket for ~deadline_ds tenths-of-a-second */
static void disc_pump(DNSServiceRef ref, int deadline_ds) {
    int fd = DNSServiceRefSockFD(ref);
    for (int i = 0; i < deadline_ds; i++) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval tv = {0, 100000};
        if (select(fd + 1, &rs, NULL, NULL, &tv) > 0) DNSServiceProcessResult(ref);
    }
}

static void disc_browse_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t iface,
                           DNSServiceErrorType err, const char *name, const char *type,
                           const char *domain, void *ctx) {
    (void)ref;
    if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd)) return;
    printf("%s\n", name);
    DNSServiceRef res;
    if (DNSServiceResolve(&res, 0, iface, name, type, domain, disc_resolve_cb, ctx) ==
        kDNSServiceErr_NoError) {
        disc_pump(res, 20); /* up to ~2s to resolve this instance */
        DNSServiceRefDeallocate(res);
    }
}

static int cmd_discover(int secs) {
    DNSServiceRef br;
    int found = 0;
    if (DNSServiceBrowse(&br, 0, 0, "_harp._tcp", NULL, disc_browse_cb, &found) !=
        kDNSServiceErr_NoError) {
        fprintf(stderr, "discover: DNSServiceBrowse failed\n");
        return 1;
    }
    fprintf(stderr, "discover: browsing _harp._tcp for %ds...\n", secs);
    disc_pump(br, secs * 10);
    DNSServiceRefDeallocate(br);
    fprintf(stderr, "discover: %d device address(es) resolved\n", found);
    return found > 0 ? 0 : 1;
}
#else
static int cmd_discover(int secs) {
    (void)secs;
    fprintf(stderr, "discover: built without dns_sd (Bonjour / avahi-compat libdns_sd) — "
                    "mDNS discovery unavailable on this host build\n");
    return 2;
}
#endif

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    harp_plat_init();
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup failed");
#endif
    const char *addr = "127.0.0.1:47800";
    const char *store_dir = "./host-store";
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            addr = argv[i + 1], i += 2;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            store_dir = argv[i + 1], i += 2;
        else
            break;
    }
    if (i >= argc) {
        fprintf(stderr,
                "usage: harp-probe [-d HOST:PORT|usb|usb:SERIAL] [-s STOREDIR] "
                "identify|refs|counters|diag-bundle [--anonymize] [OUT.cbor]|params|meters|knob ID V|save|restore|record SECS WAV|demo\n");
        return 2;
    }
    const char *cmd = argv[i];

#ifdef HAVE_LIBUSB
    /* `list` is a bus scan: no transport, no claim, no hello */
    if (strcmp(cmd, "list") == 0) {
        cmd_list();
        return 0;
    }
#endif

    /* §4.4.3 `discover` is an mDNS browse of the segment: no device connection at all */
    if (strcmp(cmd, "discover") == 0)
        return cmd_discover(3);

    probe p = {0};
    sockhandle tcp_fd = SOCK_INVALID;
    if (strcmp(addr, "usb") == 0 || strncmp(addr, "usb:", 4) == 0) {
#ifdef HAVE_LIBUSB
        /* "usb:SERIAL" picks a board; bare "usb" honors HARP_DEVICE_SERIAL
         * if set (lets the hw-test suite pin one board on a multi-dev bus),
         * else first match. */
        const char *want = addr[3] == ':' ? addr + 4 : getenv("HARP_DEVICE_SERIAL");
        if (want && !want[0]) want = NULL;
        p.io = harp_usb_open_serial(want);
        if (!p.io) return 1;
#else
        die("built without libusb; -d usb unavailable");
#endif
    } else {
        tcp_fd = dial(addr);
        sock_io_init(&p.tcp, tcp_fd);
        p.io = &p.tcp.io;
    }
    harp_link_init(&p.link);
    if (harp_store_open(&p.store, store_dir) != 0) die("cannot open local store");
    harp_client_init(&p.client, p.io, &p.link, &p.store, probe_ntf, &p);

    if (strcmp(cmd, "identify") == 0)
        cmd_identify(&p);
    else if (strcmp(cmd, "ping") == 0)
        cmd_ping(&p, i + 1 < argc ? atoi(argv[i + 1]) : 5);
    else if (strcmp(cmd, "refs") == 0)
        cmd_refs(&p);
    else if (strcmp(cmd, "counters") == 0)
        cmd_counters(&p);
    else if (strcmp(cmd, "diag-bundle") == 0) {
        /* diag-bundle [--anonymize] [outfile.cbor] — §14.4 v0 export */
        bool anon = false;
        const char *outfile = "harp-diag.cbor";
        int j = i + 1;
        if (j < argc && strcmp(argv[j], "--anonymize") == 0) {
            anon = true;
            j++;
        }
        if (j < argc) outfile = argv[j];
        cmd_diag_bundle(&p, outfile, anon);
    } else if (strcmp(cmd, "params") == 0)
        cmd_params(&p);
    else if (strcmp(cmd, "meters") == 0)
        cmd_meters(&p);
    else if (strcmp(cmd, "knob") == 0 && i + 2 < argc) {
        cmd_knob(&p, strtoull(argv[i + 1], NULL, 10), strtod(argv[i + 2], NULL));
    } else if (strcmp(cmd, "save") == 0) {
        do_hello(&p);
        do_save(&p);
    } else if (strcmp(cmd, "restore") == 0) {
        do_hello(&p);
        do_restore(&p);
    } else if (strcmp(cmd, "dev-restart") == 0) {
        do_hello(&p);
        harp_cbuf req, rsp;
        harp_cbuf_init(&req);
        harp_cbuf_init(&rsp);
        req_head(&p, &req, "x.harp-refdev.restart", false);
        request(&p, &req, &rsp);
        printf("device daemon restarting (systemd respawn)\n");
        harp_cbuf_free(&req);
        harp_cbuf_free(&rsp);
    } else if (strcmp(cmd, "reconcile-offer") == 0 && i + 3 < argc) {
        /* §11.4: post a conflict for the front panel (short-hex display strings) */
        do_hello(&p);
        int rc = harp_client_reconcile_offer(&p.client, argv[i + 1], argv[i + 2],
                                             atoi(argv[i + 3]));
        printf("reconcile-offer: %s (expect %s live %s dirty %s)\n",
               rc == 0 ? "posted" : "FAILED", argv[i + 1], argv[i + 2], argv[i + 3]);
    } else if (strcmp(cmd, "reconcile-poll") == 0) {
        /* read the front-panel pick back */
        do_hello(&p);
        bool pending = false;
        int choice = -1;
        int rc = harp_client_reconcile_poll(&p.client, &pending, &choice);
        static const char *names[] = {"push", "pull", "read-only", "duplicate"};
        printf("reconcile-poll: %s pending=%s choice=%d (%s)\n", rc == 0 ? "ok" : "FAILED",
               pending ? "true" : "false", choice,
               (choice >= 0 && choice <= 3) ? names[choice] : "none");
    } else if (strcmp(cmd, "record") == 0 && i + 2 < argc) {
#ifdef HAVE_LIBUSB
        cmd_record(&p, strtod(argv[i + 1], NULL), argv[i + 2]);
#else
        die("built without libusb");
#endif
    } else if (strcmp(cmd, "render") == 0 && i + 2 < argc) {
#ifdef HAVE_LIBUSB
        cmd_render(&p, strtod(argv[i + 1], NULL), argv[i + 2]);
#else
        die("built without libusb");
#endif
    } else if (strcmp(cmd, "t15") == 0 && i + 1 < argc) {
#ifdef HAVE_LIBUSB
        cmd_t15(&p, strtod(argv[i + 1], NULL));
#else
        die("built without libusb");
#endif
    } else if (strcmp(cmd, "version-test") == 0)
        cmd_version_test(&p);
    else if (strcmp(cmd, "epoch-test") == 0)
        cmd_epoch_test(&p);
    else if (strcmp(cmd, "txn-test") == 0)
        cmd_txn_test(&p);
    else if (strcmp(cmd, "notif-test") == 0)
        cmd_notif_test(&p);
    else if (strcmp(cmd, "format-test") == 0)
        cmd_format_test(&p);
    else if (strcmp(cmd, "core-test") == 0)
        cmd_core_test(&p);
    else if (strcmp(cmd, "cas-test") == 0)
        cmd_cas_test(&p);
    else if (strcmp(cmd, "demo") == 0)
        cmd_demo(&p, addr);
    else {
        fprintf(stderr, "harp-probe: unknown command '%s'\n", cmd);
        return 2;
    }
    if (tcp_fd != SOCK_INVALID) harp_closesock(tcp_fd);
#ifdef HAVE_LIBUSB
    else
        harp_usb_close(p.io);
#endif
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
