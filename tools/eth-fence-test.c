/* tools/eth-fence-test — §8.3.1 event-fence INTEGRATION check (offline path).
 *
 *   eth-fence-test HOST:PORT
 *
 * Drives the device's deterministic OFFLINE host-paced bounce over §8.7 TCP and proves
 * the §8.3.1 event fence is REACHED and COUNTED end-to-end — the integration value the
 * fence-predicate unit test (tests/harp_engine_logic_tests.c) cannot give, because today
 * harp-probe's render_host_paced never sets the fence flag nor sends any event, so the
 * device's fence branch (engine.c:873) is unreachable via the normal host.
 *
 * What it does:
 *   1. hello, then audio.start in HOST-PACED mode (clock-mode 1) with a host-paced TCP
 *      audio port (key 7) — d->audio.offline becomes true (the UNBOUNDED barrier path).
 *   2. accept the device's connect-back.
 *   3. read g_fence_waits / g_fence_timeouts BEFORE.
 *   4. send N plain pacing frames in SSI order (drain each output), then ONE FENCED
 *      pacing frame: HARP_AUDIO_FENCE set + the 4-byte LE `want` count (=1) appended
 *      after the header. CRITICAL ORDERING (the verified gotcha): the fenced pacing
 *      frame is sent FIRST so the render BLOCKS on the fence; only THEN is the single
 *      NOTE event sent on the EVT stream (the device bumps g_evt_consumed to 1 as it
 *      processes it, which releases the fence). If the event arrived first the fence
 *      would never wait and the setup would silently pass without proving anything.
 *   5. drain the fenced frame's output + a trailing in-order frame.
 *   6. read the counters AFTER; assert g_fence_waits moved (>0 delta) and g_fence_timeouts
 *      stayed 0 (the OFFLINE bounce is an unbounded barrier — it never times out).
 *   7. FNV-1a hash every rendered output byte and print "fence-hash: <hex>". The driving
 *      script runs this tool twice against a fresh daemon and asserts the two hashes
 *      MATCH — a deterministic, fence-held bounce — closing the loop the fence exists for.
 *
 * Portable: the raw connect-back server socket (bind/listen/accept) is wrapped in winsock2
 * on Windows (the device's host-paced connect-back is already Winsock-native), so this builds
 * under MinGW too and the Windows eth lane runs the REAL-TIME bounded-fence assertion. It is
 * modeled on tools/eth-latefr-test.c, which stays POSIX-only.
 */
#include "client.h"
#include "sock_io.h"

#include "harp/audio.h"
#include "harp/cbor.h"
#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/plat.h" /* harp_sleep_ns — portable (nanosleep is POSIX-only) */

#include <stdio.h>
#include <stdlib.h> /* strtol — the `load[:N]` frame-count arg */
#include <string.h>

/* §8.3.1 connect-back uses a raw server socket (bind/listen/accept). sock_io.h already
 * pulls winsock2/ws2tcpip on Windows; add the POSIX socket headers otherwise, plus a
 * portable handle type + close, so this tool builds under MinGW for the Windows eth lane
 * (the device's host-paced connect-back is already Winsock-native — audio_loop.c). */
#ifdef _WIN32
typedef SOCKET sockhandle;
#  define SOCK_INVALID INVALID_SOCKET
#  define harp_closesock closesocket
#else
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sockhandle;
#  define SOCK_INVALID (-1)
#  define harp_closesock close
#endif

#define NS 256        /* nsamples per pacing frame */
#define N_PRE 3       /* plain in-order frames before the fenced one */

/* FNV-1a over the rendered output, so two runs can be compared for determinism. */
static uint64_t g_hash = 1469598103934665603ull;
static void hash_bytes(const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        g_hash ^= b[i];
        g_hash *= 1099511628211ull;
    }
}

/* recv/send over a raw socket: cast to (char *) + (int) so the Winsock prototypes are
 * satisfied (POSIX accepts the same). Buffers here are tiny (≤ one 34-ch frame), so the
 * (int) length never truncates. */
static bool read_exact(sockhandle fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        int r = recv(fd, (char *)p, (int)n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool send_all(sockhandle fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < n) {
        int w = send(fd, (const char *)(p + off), (int)(n - off), 0);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

/* a plain host-paced pacing frame: header only (slots=0 => no input payload) at SSI `ts` */
static bool send_pacing(sockhandle fd, uint64_t ts) {
    harp_audio_hdr h = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D, 0, 1, ts, NS, HARP_AUDIO_FMT_F32};
    uint8_t hdr[HARP_AUDIO_HDR_LEN];
    harp_audio_hdr_encode(&h, hdr);
    return send_all(fd, hdr, HARP_AUDIO_HDR_LEN);
}

/* a FENCED pacing frame: HARP_AUDIO_FENCE set + the 4-byte LE `want` count appended
 * after the header (engine.c:874-891 reads exactly HARP_AUDIO_FENCE_LEN bytes there). */
static bool send_pacing_fenced(sockhandle fd, uint64_t ts, uint32_t want) {
    harp_audio_hdr h = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D | HARP_AUDIO_FENCE, 0, 1, ts, NS,
                        HARP_AUDIO_FMT_F32};
    uint8_t hdr[HARP_AUDIO_HDR_LEN];
    harp_audio_hdr_encode(&h, hdr);
    if (!send_all(fd, hdr, HARP_AUDIO_HDR_LEN)) return false;
    uint8_t fb[HARP_AUDIO_FENCE_LEN] = {(uint8_t)want, (uint8_t)(want >> 8),
                                        (uint8_t)(want >> 16), (uint8_t)(want >> 24)};
    return send_all(fd, fb, sizeof fb);
}

/* drain one D->H output frame (header + payload) and fold it into the determinism hash */
static bool drain_output(sockhandle fd) {
    uint8_t hdr[HARP_AUDIO_HDR_LEN];
    if (!read_exact(fd, hdr, HARP_AUDIO_HDR_LEN)) return false;
    harp_audio_hdr h;
    if (!harp_audio_hdr_decode(hdr, &h)) return false;
    size_t pl = harp_audio_payload_len(&h);
    uint8_t buf[34 * NS * 4];
    if (pl > sizeof buf) return false;
    if (pl && !read_exact(fd, buf, pl)) return false;
    hash_bytes(buf, pl);
    return true;
}

/* send ONE MIDI-1.0-in-UMP note-on at ts 0 (asap, §9.2) on the EVT stream — the device
 * routes it and bumps g_evt_consumed once it is fully processed (session.c:2206), which
 * is what releases the fence the device is blocked on. */
static bool send_note_evt(harp_client *c, harp_io *io, harp_link *link) {
    (void)c;
    (void)link;
    uint8_t b[4] = {0x20, 0x90, 60, 80}; /* note-on, ch0, pitch 60, vel 80 */
    harp_cbuf ev;
    harp_cbuf_init(&ev);
    harp_cbor_array(&ev, 3);
    harp_cbor_array(&ev, 2);
    harp_cbor_uint(&ev, 0); /* epoch 0 = now */
    harp_cbor_uint(&ev, 0); /* msc 0 = asap */
    harp_cbor_uint(&ev, 0); /* etype 0 = UMP carriage (§9.10) */
    harp_cbor_bytes(&ev, b, 4);
    int rc = harp_link_send(io, HARP_STREAM_EVT, ev.buf, ev.len);
    harp_cbuf_free(&ev);
    return rc == 0;
}

/* Send ONE untagged §9.4/§9.2 param-set (etype 1) at SSI sample `msc` on the EVT stream, with the
 * given param id/value. Two callers:
 *   - the L10-density flood (send_frame_batch): a BATCH of K of these releases a fenced frame whose
 *     want counts them all; unlike the note (ts 0 = asap, applied in one segment) a distinct msc
 *     lands the event at that exact sample, so a spread batch makes the render's evq_apply_due SPLIT
 *     into many segments (the §8.3.1 lock-coupling hammer). param id/value are kept range-safe (a
 *     valid refdev timbre id, value in [0,1]) so the flood surfaces only real faults.
 *   - the `param` release mode: msc 0 (asap). UNLIKE the note (which the device pushes to the evq
 *     immediately), an untagged param-set takes the consume-side BATCHING path (#160): the device
 *     STAGES it and bumps g_evt_consumed only when it flushes the staged run — which, after a single
 *     message, happens the instant the recv would block (harp_io.readable == false). So releasing a
 *     fence with a param proves the staging deferral + readable-triggered flush + deferred
 *     consume-accounting end-to-end (broken accounting would never reach want -> the barrier wedges). */
static bool send_param_evt(harp_io *io, uint64_t msc, uint32_t pid, float val) {
    harp_cbuf ev;
    harp_cbuf_init(&ev);
    harp_cbor_array(&ev, 3);
    harp_cbor_array(&ev, 2);
    harp_cbor_uint(&ev, 0);   /* epoch 0 = now */
    harp_cbor_uint(&ev, msc); /* §9.2 event instant = this SSI sample (spread => segment split) */
    harp_cbor_uint(&ev, 1);   /* etype 1 = param set */
    harp_cbor_map(&ev, 2);
    harp_cbor_uint(&ev, 0);
    harp_cbor_uint(&ev, pid); /* key 0: param id */
    harp_cbor_uint(&ev, 1);
    harp_cbor_float(&ev, val); /* key 1: value (clamped [0,1] device-side) */
    int rc = harp_link_send(io, HARP_STREAM_EVT, ev.buf, ev.len);
    harp_cbuf_free(&ev);
    return rc == 0;
}

/* One fenced frame k of the L10-DENSITY driver: the fenced pacing frame (ssi=k*NS, want=(k+1)*K —
 * the running EVT-message total this frame's batch completes), then its K param-set events spread
 * across `spread` distinct SSI samples inside [ssi, ssi+NS). The valid refdev timbre-param pool
 * (driver.py REF_POOL) is cycled so no two adjacent events collide on one id. With K near
 * DEV_EVQ_CAP and a pipeline depth >= 2, the render is still draining an earlier frame's deep queue
 * (holding g_evq_mu, O(S*N)) while the consume thread pushes THIS batch — the coupling under test. */
static bool send_frame_batch(sockhandle dev, harp_io *io, long k, long K, long spread) {
    static const uint32_t REF_POOL[6] = {5, 6, 12, 2, 3, 4}; /* attack release glide shape cutoff reso */
    uint64_t ssi_k = (uint64_t)k * NS;
    uint32_t want = (uint32_t)(((uint64_t)k + 1) * (uint64_t)K); /* cumulative EVT-message count */
    if (!send_pacing_fenced(dev, ssi_k, want)) return false;
    for (long j = 0; j < K; j++) {
        uint64_t off = (uint64_t)(((j % spread) * (long)NS) / spread); /* distinct sample in the frame */
        uint32_t pid = REF_POOL[(size_t)(j % 6)];
        float val = 0.2f + 0.6f * (float)((j * 7) % 16) / 16.0f; /* range-safe [0.2,0.8) */
        if (!send_param_evt(io, ssi_k + off, pid, val)) return false;
    }
    return true;
}

static uint64_t counter(harp_client *c, const char *name) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(c, &req, "diag.counters", false);
    harp_env e;
    uint64_t out = 0;
    if (harp_client_request(c, &req, &rsp, &e) == 0 && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n))
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: eth-fence-test HOST:PORT [realtime|load[:N]|param]\n");
        return 2;
    }
    /* realtime = exercise the §8.3.1 REAL-TIME bounded fence (deadline + count) instead of the
     * offline unbounded barrier. The daemon must run with HARP_FENCE_FORCE_RT=1 so its host-paced
     * stream is offline=false. We fence beyond the feed (never release it with an event) and assert
     * the device renders the range anyway (bounded — no wedge) and counts the timeout. */
    bool realtime = (argc > 2 && strcmp(argv[2], "realtime") == 0);
    /* load[:N] = the SCHED_FIFO regression driver (device/session.c harp_device_thread_set_realtime).
     * Like `realtime` the daemon runs HARP_FENCE_FORCE_RT=1 (bounded real-time fence over the TCP
     * carrier, faithful to a USB host-paced stream — the fence reads a->offline, not the transport).
     * But UNLIKE `realtime` every fenced frame IS released by an in-order event: for frame k we send
     * a fenced frame want=k+1 then the k+1'th note event, N times. The fence should therefore never
     * time out — the event is fed on time. It DOES time out only when the device's event-consume
     * thread is descheduled past the few-ms bound under CPU contention (the benign fence_timeout this
     * fix targets). Run the daemon under load with HARP_DEVICE_RT=0 (time-share, the "before" arm) vs
     * default (SCHED_FIFO, the fix) and compare the printed fence_timeouts delta: the fix drives it to
     * literal zero while audio_underruns stay 0. This mode MEASURES + prints; it never asserts on the
     * timeout count (that is the A/B the driving script/operator compares). N defaults to 4000.
     *
     * L10-DENSITY (the off-hardware fence_timeout repro, scripts/fence-l10-repro.sh): the defaults
     * above are ONE note/frame in strict ping-pong — the queue stays shallow, the render never splits,
     * and render/consume never overlap, so it reads 0 (which is why the stock gate does). Three env
     * knobs turn the stock loop into the §8.3.1 lock-coupling flood (see driver.py L10):
     *   HARP_FENCE_K       events (timestamped param-sets) per fenced frame  -> deep queue N (->512)
     *   HARP_FENCE_SPREAD  distinct SSI samples/frame the batch spans         -> render segment count S
     *   HARP_FENCE_PIPE    frames in flight (>=2)                             -> render-drain / consume-push OVERLAP
     * With HARP_FENCE_INSTRUMENT=1 on the daemon the acquire-wait vs lock-hold readout decides the ROOT
     * (lock-dominant vs floor). Defaults (K=1,PIPE=1) keep the legacy path byte-identical to the gate. */
    bool load = (argc > 2 && strncmp(argv[2], "load", 4) == 0);
    /* param = the OFFLINE single-fence flow, but release the fence with an untagged param-set
     * (which takes the consume-side batching/staging path) instead of a note (pushed directly).
     * Proves the staged event's deferred g_evt_consumed reaches the fence want via readable-flush. */
    bool param = (argc > 2 && strcmp(argv[2], "param") == 0);
    long load_n = 4000;
    if (load && argv[2][4] == ':') {
        long v = strtol(argv[2] + 5, NULL, 10);
        if (v > 0) load_n = v;
    }

#ifdef _WIN32
    /* sock_io.h: the caller owns WSAStartup before dialing (and before bind/listen below). */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "fence: WSAStartup failed\n");
        return 1;
    }
#endif

    harp_sockhandle s = harp_sock_dial(argv[1]);
    if (s == HARP_SOCK_INVALID) {
        fprintf(stderr, "fence: dial failed\n");
        return 1;
    }
    harp_sock_io tio;
    harp_sock_io_init(&tio, s);
    harp_link link;
    harp_link_init(&link);
    harp_client client;
    harp_client_init(&client, &tio.io, &link, NULL, NULL, NULL);
    harp_client_identity id;
    if (harp_client_hello(&client, "eth-fence-test", &id) != 0) {
        fprintf(stderr, "fence: hello failed (%s)\n", client.err_code);
        return 1;
    }

    /* listen for the device's host-paced connect-back on an ephemeral loopback port */
    sockhandle srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one); /* (char*) for Winsock */
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(srv, (struct sockaddr *)&a, sizeof a) != 0 || listen(srv, 1) != 0) {
        fprintf(stderr, "fence: listen failed\n");
        return 1;
    }
    socklen_t al = sizeof a;
    getsockname(srv, (struct sockaddr *)&a, &al);
    int hp_port = ntohs(a.sin_port);

    /* audio.start, host-paced (clock-mode 1), host-paced TCP audio port (key 7) */
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client, &req, "audio.start", true);
    harp_cbor_map(&req, 4);
    harp_cbor_uint(&req, 0); harp_cbor_uint(&req, 48000);
    harp_cbor_uint(&req, 1); harp_cbor_uint(&req, NS);
    harp_cbor_uint(&req, 5); harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, 7); harp_cbor_uint(&req, (uint64_t)hp_port);
    harp_env e;
    int rc = harp_client_request(&client, &req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (rc != 0) {
        fprintf(stderr, "fence: audio.start failed (rc=%d %s)\n", rc, client.err_code);
        return 1;
    }

    sockhandle dev = accept(srv, NULL, NULL);
    if (dev == SOCK_INVALID) {
        fprintf(stderr, "fence: accept failed\n");
        return 1;
    }

    uint64_t w_before = counter(&client, "x.harp-refdev.fence_waits");
    uint64_t t_before = counter(&client, "x.harp-refdev.fence_timeouts");

    if (load) {
        /* SCHED_FIFO regression driver: stream `load_n` fenced frames, each RELEASED by an
         * in-order event, so a correctly-scheduled device never times out. For frame k: send
         * the fenced frame want=k+1 FIRST (so the audio thread can park on the fence), then the
         * (k+1)'th note event (bumping g_evt_consumed toward want). drain_output blocks until
         * the device renders the range — released by the event within the bound, or, if the
         * device's event-consume thread was descheduled past it, by the bound itself (counted).
         * The daemon's g_evt_consumed starts at 0 on a fresh stream, so want == the running
         * event total. Back-to-back is a harsher, faster proxy for the realtime cadence; it
         * paces to the device's own render rate (drain_output gates each iteration). */
        uint64_t au_before = counter(&client, "audio_underruns");
        uint64_t ao_before = counter(&client, "audio_overruns");
        uint64_t al_before = counter(&client, "audio_late_frames");
        uint64_t fe_before = counter(&client, "frame_errors");
        uint64_t el_before = counter(&client, "evt_late");
        uint64_t dr_before = counter(&client, "x.harp-refdev.evq_drops");
        /* L10-DENSITY knobs. Defaults K=1,PIPE=1 => the legacy one-event ping-pong, byte-identical to
         * the fence-load-rt.sh gate. Set HARP_FENCE_K (events/frame, sized toward DEV_EVQ_CAP=512),
         * HARP_FENCE_PIPE (>=2: frames in flight so the render's deep-queue drain OVERLAPS the
         * consume-push — the coupling), HARP_FENCE_SPREAD (distinct SSI samples/frame => the render's
         * segment count S) to reproduce the §8.3.1 fence flood off-hardware (see driver.py L10). */
        long K = 1, PIPE = 1, SPREAD = NS;
        { const char *e; long v;
          if ((e = getenv("HARP_FENCE_K")))      { v = strtol(e, NULL, 10); if (v >= 1) K = v; }
          if ((e = getenv("HARP_FENCE_PIPE")))   { v = strtol(e, NULL, 10); if (v >= 1) PIPE = v; }
          if ((e = getenv("HARP_FENCE_SPREAD"))) { v = strtol(e, NULL, 10); if (v >= 1) SPREAD = v; } }
        if (SPREAD > NS) SPREAD = NS;
        bool dense = (K > 1 || PIPE > 1);
        if (!dense) {
            /* legacy: one note event per fenced frame, strict ping-pong (the unchanged gate path) */
            uint64_t ssi = 0;
            for (long k = 0; k < load_n; k++) {
                if (!send_pacing_fenced(dev, ssi, (uint32_t)(k + 1))) {
                    fprintf(stderr, "fence: load fenced send failed at k=%ld\n", k);
                    return 1;
                }
                if (!send_note_evt(&client, &tio.io, &link)) {
                    fprintf(stderr, "fence: load note evt send failed at k=%ld\n", k);
                    return 1;
                }
                if (!drain_output(dev)) {
                    fprintf(stderr, "fence: load fenced output never arrived at k=%ld (wedge?)\n", k);
                    return 1;
                }
                ssi += NS;
            }
        } else {
            /* L10-density PIPELINED driver: keep PIPE frames+batches in flight so the consume thread
             * pushes a later frame's K-event batch WHILE the render still holds g_evq_mu draining an
             * earlier frame's deep queue (O(S*N)) — the coupling that stalls the g_evt_consumed bump
             * past the 5 ms fence bound. drain_output paces to the render so the queue stays bounded
             * near DEV_EVQ_CAP (overflow => evq_drops, still counted; the fence still releases). */
            long sent = 0;
            for (; sent < PIPE && sent < load_n; sent++)
                if (!send_frame_batch(dev, &tio.io, sent, K, SPREAD)) {
                    fprintf(stderr, "fence: load batch send failed at k=%ld\n", sent);
                    return 1;
                }
            for (long dn = 0; dn < load_n; dn++) {
                if (!drain_output(dev)) {
                    fprintf(stderr, "fence: load fenced output never arrived at k=%ld (wedge?)\n", dn);
                    return 1;
                }
                if (sent < load_n) {
                    if (!send_frame_batch(dev, &tio.io, sent, K, SPREAD)) {
                        fprintf(stderr, "fence: load batch send failed at k=%ld\n", sent);
                        return 1;
                    }
                    sent++;
                }
            }
        }
        uint64_t w_after = counter(&client, "x.harp-refdev.fence_waits");
        uint64_t t_after = counter(&client, "x.harp-refdev.fence_timeouts");
        uint64_t au_after = counter(&client, "audio_underruns");
        uint64_t ao_after = counter(&client, "audio_overruns");
        uint64_t al_after = counter(&client, "audio_late_frames");
        uint64_t fe_after = counter(&client, "frame_errors");
        uint64_t el_after = counter(&client, "evt_late");
        uint64_t dr_after = counter(&client, "x.harp-refdev.evq_drops");

        harp_cbuf sreq, srsp;
        harp_cbuf_init(&sreq);
        harp_cbuf_init(&srsp);
        harp_client_req_head(&client, &sreq, "audio.stop", false);
        harp_env se;
        harp_client_request(&client, &sreq, &srsp, &se);
        harp_cbuf_free(&sreq);
        harp_cbuf_free(&srsp);
        harp_closesock(dev);
        harp_closesock(srv);
        harp_sock_close(s);

        /* machine-parseable A/B line: the driving script/operator compares two arms (device
         * HARP_DEVICE_RT=0 "before" vs default SCHED_FIFO "after"). fence_timeouts is the target.
         * k/pipe/spread echo the L10-density config so a sweep row is self-describing. */
        printf("fence-load: frames=%ld k=%ld pipe=%ld spread=%ld fence_waits=+%llu fence_timeouts=+%llu "
               "audio_underruns=+%llu audio_overruns=+%llu audio_late_frames=+%llu "
               "frame_errors=+%llu evt_late=+%llu evq_drops=+%llu\n",
               load_n, K, PIPE, SPREAD,
               (unsigned long long)(w_after - w_before),
               (unsigned long long)(t_after - t_before),
               (unsigned long long)(au_after - au_before),
               (unsigned long long)(ao_after - ao_before),
               (unsigned long long)(al_after - al_before),
               (unsigned long long)(fe_after - fe_before),
               (unsigned long long)(el_after - el_before),
               (unsigned long long)(dr_after - dr_before));
        return 0;
    }

    /* N_PRE plain in-order frames, draining each output -> render cursor = N_PRE*NS. */
    uint64_t ssi = 0;
    bool ok = true;
    for (int k = 0; ok && k < N_PRE; k++) {
        ok = send_pacing(dev, ssi) && drain_output(dev);
        ssi += NS;
    }
    if (!ok) {
        fprintf(stderr, "fence: pre-fence pacing I/O failed\n");
        return 1;
    }

    /* THE FENCE: send the fenced frame (want=1) FIRST so the device BLOCKS on the fence
     * (g_evt_consumed is 0 < 1). Only then send the single note event; the device bumps
     * g_evt_consumed to 1 as it processes it, releasing the fence. Ordering is the whole
     * point (the verified gotcha) — two sockets have no mutual ordering otherwise.
     *
     * The two streams ride two sockets with NO mutual ordering, so a bare back-to-back
     * send can let the device's session thread consume the event BEFORE the audio thread
     * reads the fenced frame — then g_evt_consumed is already 1, the fence never waits, and
     * fence_waits stays 0 (exactly the flake seen run-to-run). Bridge the gap explicitly:
     * after sending the fenced frame, sleep long enough that the audio thread has surely
     * read it and parked on the fence (it polls every 50 µs), THEN release it with the note.
     * This makes "the fence is reached" deterministic without weakening the assertion. */
    if (!send_pacing_fenced(dev, ssi, 1)) {
        fprintf(stderr, "fence: fenced pacing send failed\n");
        return 1;
    }
    if (realtime) {
        /* REAL-TIME: do NOT release the fence with an event. The device MUST bound the wait
         * (a few ms), render the range with the late (here, absent) event, and count the
         * timeout — never wedge. drain_output blocks only until that bounded render lands. */
    } else {
        harp_sleep_ns(50ull * 1000 * 1000); /* 50 ms: audio thread parks on the fence (portable) */
        /* param mode releases with a STAGED param-set (consume-side batching path); default with a
         * note (immediate evq push). Both must bump g_evt_consumed to 1 and release the fence. */
        bool sent = param ? send_param_evt(&tio.io, 0, 0, 0.5f) : send_note_evt(&client, &tio.io, &link);
        if (!sent) {
            fprintf(stderr, "fence: release evt send failed (%s)\n", param ? "param" : "note");
            return 1;
        }
    }
    if (!drain_output(dev)) { /* offline: blocks until the event releases; realtime: until the bound */
        fprintf(stderr, "fence: fenced-frame output never arrived (fence wedged?)\n");
        return 1;
    }
    ssi += NS;

    /* a trailing in-order frame: its output proves the device read past the fence cleanly */
    if (!send_pacing(dev, ssi) || !drain_output(dev)) {
        fprintf(stderr, "fence: trailing pacing I/O failed\n");
        return 1;
    }

    uint64_t w_after = counter(&client, "x.harp-refdev.fence_waits");
    uint64_t t_after = counter(&client, "x.harp-refdev.fence_timeouts");

    harp_cbuf sreq, srsp;
    harp_cbuf_init(&sreq);
    harp_cbuf_init(&srsp);
    harp_client_req_head(&client, &sreq, "audio.stop", false);
    harp_env se;
    harp_client_request(&client, &sreq, &srsp, &se);
    harp_cbuf_free(&sreq);
    harp_cbuf_free(&srsp);

    harp_closesock(dev);
    harp_closesock(srv);
    harp_sock_close(s);

    /* the determinism hash the driving script compares across two runs */
    printf("fence-hash: %016llx\n", (unsigned long long)g_hash);

    if (w_after <= w_before) {
        fprintf(stderr, "FENCE FAIL: fence_waits did not move (%llu -> %llu) — "
                "the fence branch was never reached\n",
                (unsigned long long)w_before, (unsigned long long)w_after);
        return 1;
    }
    if (realtime) {
        /* the range DID render (drain_output above succeeded — no wedge) AND the bound fired */
        if (t_after <= t_before) {
            fprintf(stderr, "FENCE FAIL: real-time fence_timeouts did not move (%llu -> %llu) — "
                    "the bounded path did not fire (unbounded fence would WEDGE a real-time stream)\n",
                    (unsigned long long)t_before, (unsigned long long)t_after);
            return 1;
        }
        printf("FENCE PASS: real-time event fence bounded + counted, no wedge "
               "(fence_waits %llu -> %llu, fence_timeouts %llu -> %llu)\n",
               (unsigned long long)w_before, (unsigned long long)w_after,
               (unsigned long long)t_before, (unsigned long long)t_after);
        return 0;
    }
    if (t_after != t_before) {
        fprintf(stderr, "FENCE FAIL: fence_timeouts moved (%llu -> %llu) — the OFFLINE "
                "bounce must be an UNBOUNDED barrier, never a timeout\n",
                (unsigned long long)t_before, (unsigned long long)t_after);
        return 1;
    }
    printf("FENCE PASS: offline event fence reached + counted "
           "(fence_waits %llu -> %llu, fence_timeouts %llu == %llu)\n",
           (unsigned long long)w_before, (unsigned long long)w_after,
           (unsigned long long)t_before, (unsigned long long)t_after);
    return 0;
}
