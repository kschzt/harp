/* host/mdns.c — §4.4.3 `_harp._tcp` browse-and-resolve. See mdns.h.
 * Compiled into a stub returning -1 unless HAVE_DNS_SD (macOS native / Linux avahi-compat). */
#include "mdns.h"

#include <stdio.h>
#include <string.h>

#ifdef HAVE_DNS_SD
#include <arpa/inet.h>
#include <dns_sd.h>
#include <sys/select.h>

typedef struct {
    harp_mdns_instance *out;
    size_t max, n;
} disc_ctx;

static void resolve_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t iface,
                       DNSServiceErrorType err, const char *fullname, const char *host,
                       uint16_t port, uint16_t txtlen, const unsigned char *txt, void *ctx) {
    (void)ref;
    (void)flags;
    (void)iface;
    (void)fullname;
    disc_ctx *dc = (disc_ctx *)ctx;
    if (err != kDNSServiceErr_NoError || dc->n >= dc->max) return;
    uint16_t hp = ntohs(port);
    for (size_t i = 0; i < dc->n; i++) /* dedup: a browse may re-report an instance */
        if (dc->out[i].port == hp && strcmp(dc->out[i].host, host) == 0) return;
    harp_mdns_instance *m = &dc->out[dc->n];
    memset(m, 0, sizeof *m);
    snprintf(m->host, sizeof m->host, "%s", host);
    m->port = hp;
    snprintf(m->proto, sizeof m->proto, "?");
    uint8_t plen = 0;
    const void *pv = TXTRecordGetValuePtr(txtlen, txt, "proto", &plen);
    if (pv && plen < sizeof m->proto) { /* TXT values are NOT null-terminated by the API */
        memcpy(m->proto, pv, plen);
        m->proto[plen] = 0;
    }
    uint8_t slen = 0;
    m->serial_leaked = TXTRecordGetValuePtr(txtlen, txt, "serial", &slen) != NULL; /* §16 */
    dc->n++;
}

/* pump a ref's socket for up to deadline_ms in 100 ms slices, returning as soon as *got grows
 * (a resolve landed) — so a fast LAN answer costs ~its real latency, not the full deadline. */
static void pump(DNSServiceRef ref, int deadline_ms, const size_t *got) {
    size_t start = got ? *got : 0;
    int fd = DNSServiceRefSockFD(ref);
    for (int waited = 0; waited < deadline_ms; waited += 100) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval tv = {0, 100000};
        if (select(fd + 1, &rs, NULL, NULL, &tv) > 0) {
            DNSServiceProcessResult(ref);
            if (got && *got > start) return;
        }
    }
}

static void browse_cb(DNSServiceRef ref, DNSServiceFlags flags, uint32_t iface,
                      DNSServiceErrorType err, const char *name, const char *type,
                      const char *domain, void *ctx) {
    (void)ref;
    if (err != kDNSServiceErr_NoError || !(flags & kDNSServiceFlagsAdd)) return;
    DNSServiceRef res;
    if (DNSServiceResolve(&res, 0, iface, name, type, domain, resolve_cb, ctx) ==
        kDNSServiceErr_NoError) {
        pump(res, 600, &((disc_ctx *)ctx)->n); /* ~0.6 s cap; returns the instant it resolves */
        DNSServiceRefDeallocate(res);
    }
}

int harp_mdns_discover(int timeout_ms, harp_mdns_instance *out, size_t max) {
    if (!out || max == 0) return 0;
    disc_ctx dc = {out, max, 0};
    DNSServiceRef br;
    if (DNSServiceBrowse(&br, 0, 0, "_harp._tcp", NULL, browse_cb, &dc) != kDNSServiceErr_NoError)
        return -1;
    int fd = DNSServiceRefSockFD(br);
    for (int waited = 0; waited < timeout_ms && dc.n < max; waited += 100) { /* early-exit at max */
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval tv = {0, 100000};
        if (select(fd + 1, &rs, NULL, NULL, &tv) > 0) DNSServiceProcessResult(br);
    }
    DNSServiceRefDeallocate(br);
    return (int)dc.n;
}

#else /* no dns_sd on this build (Windows, bare runner) */
int harp_mdns_discover(int timeout_ms, harp_mdns_instance *out, size_t max) {
    (void)timeout_ms;
    (void)out;
    (void)max;
    return -1;
}
#endif
