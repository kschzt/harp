/* win_loopback_repro.c — isolate the §8.7 host-paced Windows reset (faithful).
 *
 * The bare two-connection dial-back pattern does NOT reset on the runner, so the
 * cause is app-specific. This version mirrors the REAL threading/directionality:
 *
 *   DEVICE  listens for control (host dials in); on the control thread it reads an
 *           "audio.start" byte, then spawns an AUDIO thread (winpthreads) that
 *           DIALS BACK to the host's audio listener and blocking-recv's pacing.
 *           A second "meter" thread is also spawned (like meter_pump). Both the
 *           control and audio reads are BLOCKING recv (with SO_RCVTIMEO).
 *   HOST    dials the device's control port, sends "audio.start", listens for the
 *           audio dial-back, accepts it, sends 3 pacing frames, blocking-recv's.
 *
 * If a connection RSTs (recv -> WSA=10054) we've reproduced it; then bisect.
 *
 * Build (MinGW): x86_64-w64-mingw32-gcc win_loopback_repro.c -lws2_32 -lpthread -o repro.exe
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

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
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

static void set_rcvtimeo(SOCKET s, int ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof ms);
}

/* blocking-recv until data/reset/timeout; logs a reset (10054) loudly */
static void recv_watch(const char *who, SOCKET s) {
    for (int i = 0; i < 5; i++) {
        char buf[256];
        int n = recv(s, buf, sizeof buf, 0);
        if (n > 0) { fprintf(stderr, "REPRO: %s recv %d bytes (ok)\n", who, n); continue; }
        int e = (n < 0) ? WSAGetLastError() : 0;
        if (n < 0 && (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK)) continue; /* idle */
        fprintf(stderr, "REPRO: %s recv=%d WSA=%d  <<< %s\n", who, n, e,
                n == 0 ? "PEER CLOSED" : "RESET");
        return;
    }
    fprintf(stderr, "REPRO: %s — no reset (idle)\n", who);
}

static int g_audio_port;          /* host's audio listener port (device dials back) */
static SOCKET g_dev_audio = INVALID_SOCKET;

static void *dev_audio_thread(void *arg) {
    (void)arg;
    SOCKET a = dial(g_audio_port); /* the §8.7 dial-back */
    g_dev_audio = a;
    if (a == INVALID_SOCKET) return NULL;
    set_rcvtimeo(a, 120);
    recv_watch("dev.audio", a);
    return NULL;
}
static void *dev_meter_thread(void *arg) { (void)arg; /* mimic meter_pump (no I/O yet) */ return NULL; }

static void *device_main(void *arg) {
    int ctl_port = *(int *)arg;
    SOCKET cl = listen_on(&ctl_port);              /* device listens for control... */
    *(int *)arg = ctl_port;                         /* publish the port to the host */
    SOCKET cfd = accept(cl, NULL, NULL);            /* ...host dials in */
    int one = 1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    char b;
    recv(cfd, &b, 1, 0);                            /* read the "audio.start" byte */
    /* spawn audio + meter threads, exactly like handle_audio_start */
    pthread_t at, mt;
    pthread_create(&at, NULL, dev_audio_thread, NULL);
    pthread_create(&mt, NULL, dev_meter_thread, NULL);
    set_rcvtimeo(cfd, 120);
    recv_watch("dev.ctl", cfd);                    /* session loop blocking recv */
    pthread_join(at, NULL);
    pthread_join(mt, NULL);
    return NULL;
}

int main(void) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);

    int ctl_port = 0;
    pthread_t dev;
    pthread_create(&dev, NULL, device_main, &ctl_port);
    /* wait for the device to publish its control port */
    while (ctl_port == 0) Sleep(2);
    Sleep(20);

    int audio_port = 0;
    SOCKET audio_listen = listen_on(&audio_port);
    g_audio_port = audio_port;

    SOCKET chost = dial(ctl_port);                 /* host dials device control */
    if (chost == INVALID_SOCKET) return 1;
    char start = 'S';
    send(chost, &start, 1, 0);                      /* "audio.start" */

    SOCKET ahost = accept(audio_listen, NULL, NULL); /* device dials back */
    int one = 1; setsockopt(ahost, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    fprintf(stderr, "REPRO: host accepted audio dial-back; sending pacing\n");
    char pace[24]; memset(pace, 0xab, sizeof pace);
    for (int i = 0; i < 3; i++) send(ahost, pace, sizeof pace, 0);

    set_rcvtimeo(chost, 120);
    set_rcvtimeo(ahost, 120);
    recv_watch("host.ctl", chost);
    recv_watch("host.audio", ahost);

    pthread_join(dev, NULL);
    fprintf(stderr, "REPRO: done\n");
    return 0;
}
