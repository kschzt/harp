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
 * POSIX-only (raw server socket for the device's connect-back); not built on Windows,
 * exactly like tools/eth-latefr-test.c which this models.
 */
#include "client.h"
#include "sock_io.h"

#include "harp/audio.h"
#include "harp/cbor.h"
#include "harp/envelope.h"
#include "harp/link.h"

#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

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

static bool read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, 0);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

/* a plain host-paced pacing frame: header only (slots=0 => no input payload) at SSI `ts` */
static bool send_pacing(int fd, uint64_t ts) {
    harp_audio_hdr h = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D, 0, 1, ts, NS, HARP_AUDIO_FMT_F32};
    uint8_t hdr[HARP_AUDIO_HDR_LEN];
    harp_audio_hdr_encode(&h, hdr);
    return send_all(fd, hdr, HARP_AUDIO_HDR_LEN);
}

/* a FENCED pacing frame: HARP_AUDIO_FENCE set + the 4-byte LE `want` count appended
 * after the header (engine.c:874-891 reads exactly HARP_AUDIO_FENCE_LEN bytes there). */
static bool send_pacing_fenced(int fd, uint64_t ts, uint32_t want) {
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
static bool drain_output(int fd) {
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
        fprintf(stderr, "usage: eth-fence-test HOST:PORT [realtime]\n");
        return 2;
    }
    /* realtime = exercise the §8.3.1 REAL-TIME bounded fence (deadline + count) instead of the
     * offline unbounded barrier. The daemon must run with HARP_FENCE_FORCE_RT=1 so its host-paced
     * stream is offline=false. We fence beyond the feed (never release it with an event) and assert
     * the device renders the range anyway (bounded — no wedge) and counts the timeout. */
    bool realtime = (argc > 2 && strcmp(argv[2], "realtime") == 0);

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
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
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

    int dev = accept(srv, NULL, NULL);
    if (dev < 0) {
        fprintf(stderr, "fence: accept failed\n");
        return 1;
    }

    uint64_t w_before = counter(&client, "x.harp-refdev.fence_waits");
    uint64_t t_before = counter(&client, "x.harp-refdev.fence_timeouts");

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
        nanosleep(&(struct timespec){0, 50 * 1000 * 1000}, NULL); /* 50 ms: audio thread parks on the fence */
        if (!send_note_evt(&client, &tio.io, &link)) {
            fprintf(stderr, "fence: note evt send failed\n");
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

    close(dev);
    close(srv);
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
