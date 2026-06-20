/* win_loopback_repro.c — isolate the §8.7 host-paced Windows reset (bidirectional).
 *
 * Faithful threading + dial-back did NOT reset. Remaining structural difference:
 * the real host-paced flow has CONCURRENT send+recv on the SAME audio socket from
 * two threads (feeder sends pacing while the reader blocking-recv's audio) and is
 * BIDIRECTIONAL (device recvs pacing -> "renders" -> sends audio back, in a loop).
 * The live path's audio thread never recv's; this is the host-paced-only shape.
 *
 * Build: x86_64-w64-mingw32-gcc win_loopback_repro.c -lws2_32 -lpthread -o repro.exe
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static volatile int g_stop = 0;

static SOCKET listen_on(int *port) {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(ls, (struct sockaddr *)&a, sizeof a);
    listen(ls, 1);
    int al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    *port = ntohs(a.sin_port);
    return ls;
}
static SOCKET dial(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)port);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "REPRO: connect(:%d) WSA=%d\n", port, WSAGetLastError());
        return INVALID_SOCKET;
    }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}
static void rst(const char *who, int n) {
    fprintf(stderr, "REPRO: %s n=%d WSA=%d  <<< %s\n", who, n, n < 0 ? WSAGetLastError() : 0,
            n == 0 ? "PEER CLOSED" : "RESET");
    g_stop = 1;
}

static int g_audio_port, g_ctl_port;

/* DEVICE audio thread: blocking recv pacing -> "render" -> send audio back, loop */
static void *dev_audio(void *arg) {
    (void)arg;
    SOCKET s = dial(g_audio_port);
    if (s == INVALID_SOCKET) return NULL;
    char in[64], out[256];
    memset(out, 0x5a, sizeof out);
    while (!g_stop) {
        int n = recv(s, in, 24, 0);                 /* a pacing frame */
        if (n <= 0) { rst("dev.audio.recv", n); return NULL; }
        int w = send(s, out, sizeof out, 0);        /* the rendered block */
        if (w <= 0) { rst("dev.audio.send", w); return NULL; }
    }
    return NULL;
}
/* DEVICE control session thread: accept host, read audio.start, spawn audio thread,
 * then blocking-recv control (like the real session loop) */
static void *device_main(void *arg) {
    (void)arg;
    SOCKET cl = listen_on(&g_ctl_port);
    SOCKET cfd = accept(cl, NULL, NULL);
    int one = 1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    char b; recv(cfd, &b, 1, 0);
    pthread_t at; pthread_create(&at, NULL, dev_audio, NULL);
    while (!g_stop) {
        char c[64];
        int n = recv(cfd, c, sizeof c, 0);
        if (n < 0) { rst("dev.ctl.recv", n); break; }
        if (n == 0) break;
    }
    pthread_join(at, NULL);
    return NULL;
}

static SOCKET g_ahost;
/* HOST feeder: continuously send pacing on the SAME socket the reader recv's */
static void *host_feeder(void *arg) {
    (void)arg;
    char pace[24]; memset(pace, 0xab, sizeof pace);
    while (!g_stop) {
        int w = send(g_ahost, pace, sizeof pace, 0);
        if (w <= 0) { rst("host.feeder.send", w); return NULL; }
        Sleep(3);
    }
    return NULL;
}
/* HOST reader: continuously blocking-recv audio on the SAME socket */
static void *host_reader(void *arg) {
    (void)arg;
    char buf[512];
    while (!g_stop) {
        int n = recv(g_ahost, buf, sizeof buf, 0);
        if (n <= 0) { rst("host.reader.recv", n); return NULL; }
    }
    return NULL;
}

int main(void) {
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    pthread_t dev; pthread_create(&dev, NULL, device_main, NULL);
    while (g_ctl_port == 0) Sleep(2);
    Sleep(20);

    int ap = 0; SOCKET al = listen_on(&ap); g_audio_port = ap;
    SOCKET chost = dial(g_ctl_port);
    char s = 'S'; send(chost, &s, 1, 0);
    g_ahost = accept(al, NULL, NULL);
    int one = 1; setsockopt(g_ahost, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    fprintf(stderr, "REPRO: connected; concurrent feeder+reader on the audio socket\n");

    pthread_t ft, rt; pthread_create(&ft, NULL, host_feeder, NULL);
    pthread_create(&rt, NULL, host_reader, NULL);
    Sleep(500);                 /* let it run ~0.5s under bidirectional load */
    g_stop = 1;
    pthread_join(ft, NULL); pthread_join(rt, NULL);
    fprintf(stderr, "REPRO: done (stop=%d — if any RESET above, reproduced)\n", g_stop);
    return 0;
}
