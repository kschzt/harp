/* win_loopback_repro.c — isolate the §8.7 host-paced Windows reset.
 *
 * The offline bounce establishes TWO localhost TCP connections between the host
 * and the device: a CONTROL link (host dials device) and a host-paced AUDIO link
 * (device DIALS BACK to an ephemeral listener the host opened). Shortly after the
 * dial-back both connections reset with WSAECONNRESET (10054) while idle. The live
 * path (RTP/UDP audio, no dial-back) does NOT reset — so the dial-back is the
 * suspect. This reproduces the bare pattern with two threads on one process and
 * reports any reset, so the fix (socket option / ordering) can be found fast.
 *
 * Build (MinGW): x86_64-w64-mingw32-gcc win_loopback_repro.c -lws2_32 -o repro.exe
 * Run: repro.exe         (mode 0: faithful — accept audio LATE, like the drain)
 *      repro.exe 1       (mode 1: accept audio IMMEDIATELY after connect)
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <stdio.h>
#include <string.h>

static int g_accept_late = 1; /* mode 0/1 toggles this */

static SOCKET listen_ephemeral(int *port) {
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
        fprintf(stderr, "REPRO: connect(:%d) failed WSA=%d\n", port, WSAGetLastError());
        return INVALID_SOCKET;
    }
    return s;
}

/* a blocking recv with a poll timeout; returns 1 if reset/closed, 0 if just idle */
static int probe(const char *who, SOCKET s) {
    WSAPOLLFD p;
    p.fd = s;
    p.events = POLLRDNORM;
    p.revents = 0;
    int r = WSAPoll(&p, 1, 50);
    if (r > 0 && (p.revents & (POLLRDNORM | POLLHUP | POLLERR))) {
        char buf[64];
        int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) {
            fprintf(stderr, "REPRO: %s recv=%d WSA=%d revents=0x%x  <<< RESET/CLOSE\n",
                    who, n, n < 0 ? WSAGetLastError() : 0, p.revents);
            return 1;
        }
    }
    return 0;
}

struct ctx {
    int ctl_port;
    int audio_port;
};

/* the "device": dial control, then DIAL BACK the audio listener, then idle */
static unsigned __stdcall device_thread(void *arg) {
    struct ctx *c = arg;
    SOCKET ctl = dial(c->ctl_port);   /* control: device->host (here, just a 2nd peer) */
    Sleep(5);
    SOCKET aud = dial(c->audio_port); /* the §8.7 host-paced DIAL-BACK */
    if (ctl == INVALID_SOCKET || aud == INVALID_SOCKET) return 1;
    fprintf(stderr, "REPRO: device connected ctl+audio; idling, watching for reset\n");
    for (int i = 0; i < 6; i++) { /* ~300ms idle, like the device's blocked recv */
        if (probe("device.ctl", ctl)) return 0;
        if (probe("device.audio", aud)) return 0;
        Sleep(50);
    }
    fprintf(stderr, "REPRO: device side — NO reset after idle\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) g_accept_late = atoi(argv[1]) ? 0 : 1;
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);

    int ctl_port = 0, audio_port = 0;
    SOCKET ctl_listen = listen_ephemeral(&ctl_port);
    SOCKET audio_listen = listen_ephemeral(&audio_port);
    struct ctx c = {ctl_port, audio_port};

    uintptr_t th = _beginthreadex(NULL, 0, device_thread, &c, 0, NULL);

    /* host: accept control immediately; accept audio LATE (mode 0) to mimic the
     * sessionUp drain window, or immediately (mode 1). */
    SOCKET sctl = accept(ctl_listen, NULL, NULL);
    if (g_accept_late) Sleep(30); /* the drain/audio.start round-trip window */
    SOCKET saud = accept(audio_listen, NULL, NULL);
    fprintf(stderr, "REPRO: host accepted ctl+audio (accept_late=%d)\n", g_accept_late);

    /* mimic the feeder: send a few "pacing" frames on the audio socket */
    char pace[24];
    memset(pace, 0xab, sizeof pace);
    for (int i = 0; i < 3; i++) send(saud, pace, sizeof pace, 0);

    int reset = 0;
    for (int i = 0; i < 6; i++) {
        reset |= probe("host.ctl", sctl);
        reset |= probe("host.audio", saud);
        Sleep(50);
    }
    if (!reset) fprintf(stderr, "REPRO: host side — NO reset after idle\n");

    WaitForSingleObject((HANDLE)th, 2000);
    fprintf(stderr, "REPRO: done (reset=%d)\n", reset);
    return 0;
}
