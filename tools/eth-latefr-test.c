/* tools/eth-latefr-test — §8.2 host-paced late-frame discard check.
 *
 *   eth-latefr-test HOST:PORT
 *
 * Dials the device's TCP framed link, says hello, then audio.start in HOST-PACED
 * mode (clock-mode 1) with a host-paced TCP audio port (key 7). The device connects
 * back; we send pacing frames in SSI order, then ONE frame whose ts is BEHIND the
 * render cursor (a rewound SSI — a host bug), then one more in-order frame. Per §8.2
 * the device MUST discard the rewound frame and count it (audio_late_frames). Draining
 * the trailing in-order frame's output proves the device read past the late one; we
 * then assert the counter moved by exactly 1.
 *
 * POSIX-only (raw server socket for the device's connect-back); not built on Windows.
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
#include <unistd.h>

#define NS 256 /* nsamples per pacing frame */

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

/* a host-paced pacing frame: header only (slots=0 => no input payload) at SSI `ts` */
static bool send_pacing(int fd, uint64_t ts) {
    harp_audio_hdr h = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D, 0, 1, ts, NS, HARP_AUDIO_FMT_F32};
    uint8_t hdr[HARP_AUDIO_HDR_LEN];
    harp_audio_hdr_encode(&h, hdr);
    size_t off = 0;
    while (off < HARP_AUDIO_HDR_LEN) {
        ssize_t w = send(fd, hdr + off, HARP_AUDIO_HDR_LEN - off, 0);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}

/* drain one D->H output frame (header + payload) the device renders per in-order frame */
static bool drain_output(int fd) {
    uint8_t hdr[HARP_AUDIO_HDR_LEN];
    if (!read_exact(fd, hdr, HARP_AUDIO_HDR_LEN)) return false;
    harp_audio_hdr h;
    if (!harp_audio_hdr_decode(hdr, &h)) return false;
    size_t pl = harp_audio_payload_len(&h);
    uint8_t buf[34 * NS * 4];
    if (pl > sizeof buf) return false;
    return pl == 0 || read_exact(fd, buf, pl);
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
        fprintf(stderr, "usage: eth-latefr-test HOST:PORT\n");
        return 2;
    }

    harp_sockhandle s = harp_sock_dial(argv[1]);
    if (s == HARP_SOCK_INVALID) {
        fprintf(stderr, "latefr: dial failed\n");
        return 1;
    }
    harp_sock_io tio;
    harp_sock_io_init(&tio, s);
    harp_link link;
    harp_link_init(&link);
    harp_client client;
    harp_client_init(&client, &tio.io, &link, NULL, NULL, NULL);
    harp_client_identity id;
    if (harp_client_hello(&client, "eth-latefr-test", &id) != 0) {
        fprintf(stderr, "latefr: hello failed (%s)\n", client.err_code);
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
        fprintf(stderr, "latefr: listen failed\n");
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
        fprintf(stderr, "latefr: audio.start failed (rc=%d %s)\n", rc, client.err_code);
        return 1;
    }

    int dev = accept(srv, NULL, NULL);
    if (dev < 0) {
        fprintf(stderr, "latefr: accept failed\n");
        return 1;
    }

    uint64_t before = counter(&client, "audio_late_frames");

    /* 0, NS, 2*NS in order (drain each output) -> render cursor = 3*NS. Then a REWOUND
     * frame (ts=NS < 3*NS) that MUST be discarded, then 3*NS in order whose output
     * proves the device read past the rewound one (it reads the link in order). */
    bool ok = send_pacing(dev, 0) && drain_output(dev) &&
              send_pacing(dev, NS) && drain_output(dev) &&
              send_pacing(dev, 2 * NS) && drain_output(dev) &&
              send_pacing(dev, NS) &&               /* LATE: behind the cursor */
              send_pacing(dev, 3 * NS) && drain_output(dev);
    if (!ok) {
        fprintf(stderr, "latefr: pacing I/O failed\n");
        return 1;
    }

    uint64_t after = counter(&client, "audio_late_frames");

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

    if (after != before + 1) {
        fprintf(stderr, "LATEFR FAIL: audio_late_frames %llu -> %llu (want +1)\n",
                (unsigned long long)before, (unsigned long long)after);
        return 1;
    }
    printf("LATEFR PASS: host-paced rewound frame discarded + counted "
           "(audio_late_frames %llu -> %llu)\n",
           (unsigned long long)before, (unsigned long long)after);
    return 0;
}
