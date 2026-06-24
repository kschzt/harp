/* harp_client — the host-side protocol client shared by every HARP host
 * (harp-probe CLI, the VST3 shell runtime; future daemons).
 *
 * One implementation of the host half of `harp-core` + `harp-recall`:
 * request/response correlation over the framed link, identity, refs,
 * snapshot-on-demand, closure fetch (want -> obj stream) and push
 * (have -> send -> obj stream), and the refset CAS. Extracted from two
 * drifting copies (probe and shell) — debt #2.
 *
 * Threading: NOT internally synchronized. The owner serializes calls and
 * any other reads of the shared harp_link (the shell holds its ctl mutex;
 * the probe is single-threaded).
 *
 * Error model: every op returns 0 on success,
 *   HARP_CLIENT_EIO    transport/protocol failure (link dead, bad envelope)
 *   HARP_CLIENT_EDEV   device error envelope — code/detail in err_code/err_msg
 * Callers decide policy (probe exits, shell logs and degrades).
 */
#ifndef HARP_CLIENT_H
#define HARP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/object.h"
#include "harp/store.h"

#define HARP_CLIENT_EIO (-1)
#define HARP_CLIENT_EDEV (-2)
#define HARP_CLIENT_EINCOMPAT (-3) /* §5.4: hello rejected 'incompatible'; supported range in incompat_major_* */

/* closure walks are bounded: a refdev-class state tree is < 10 objects;
 * 512 leaves room for real devices (wavetables, per-pad kits) */
#define HARP_CLIENT_MAX_CLOSURE 512
#define HARP_CLIENT_SENDQ_CAP 1024 /* §4.2.1 obj-send queue; > MAX_CLOSURE so an honest push can't overflow */

#define HARP_CLIENT_MAX_CAPS 32
typedef struct {
    char vendor[64], product[64];
    uint32_t vendor_id, product_id;
    char serial[64];
    char fw[32];
    char engine_id[64], engine_ver[32];
    harp_hash param_map_hash;
    uint64_t boot_count;
    uint64_t txn_max, txn_events; /* §9.6 transaction limits (identity key 13); 0 if not advertised */
    uint32_t eth_target_floor; /* §6.4 rt-profile (identity key 14 sub-key 0): device-declared safe ethTargetFrames floor (frames); 0 if not advertised */
    uint32_t eth_nsamples;     /* §6.4 rt-profile (identity key 14 sub-key 1): device-declared RTP packet size (frames); 0 if not advertised */
    char caps[HARP_CLIENT_MAX_CAPS][32]; /* capability strings (identity key 6) */
    size_t ncaps;
    /* §6.4 latency-profile (identity key 8): one entry per negotiated rate. Each
     * profile is {0 rate, 1 input-latency, 2 output-latency, 3 buffer-depth} in
     * samples. The §14.3 LoopbackMeasurer reads this to compute the expected round-
     * trip for the negotiated rate. nlat == 0 if the device omitted key 8. */
#define HARP_CLIENT_MAX_LAT 8
    struct {
        uint32_t rate, in_lat, out_lat, buf_depth;
    } lat[HARP_CLIENT_MAX_LAT];
    size_t nlat;
} harp_client_identity;

/* true if the device advertised `cap` */
bool harp_client_has_cap(const harp_client_identity *id, const char *cap);

typedef struct {
    harp_io *io;
    harp_link *link;   /* BORROWED: rx reassembly state is per-connection and
                          may also be polled by the owner (shell evt drain) */
    harp_store *store; /* obj-stream sink + closure source; may be NULL for
                          stateless use (identify, knob) */
    harp_cbuf msg;     /* rx scratch */
    uint64_t next_rid;
    uint64_t peer_credit; /* core.credit grants from the device */
    /* §4.2.1 credit flow-control (symmetric to the device). `granted` is the sliding
     * window of credit WE granted the device for its D->H sends — decremented as we
     * consume, re-granted when it halves (this was entirely missing: the device would
     * otherwise stall once our initial grant drained). sendq holds object hashes we must
     * push but couldn't yet for lack of peer_credit; push_closure self-pumps the link to
     * drain it (no background thread raises peer_credit). Single-owner client, no lock. */
    uint64_t granted;
    harp_hash sendq[HARP_CLIENT_SENDQ_CAP];
    size_t sendq_head, sendq_tail, sendq_count;
    uint64_t obj_drops;

    /* out-of-band notification hook (state.changed, log); may be NULL.
     * core.credit is consumed internally either way. */
    void (*on_ntf)(void *ud, const harp_env *e);
    void *ud;

    /* last device error (valid after HARP_CLIENT_EDEV) */
    char err_method[64];
    char err_code[64];
    char err_msg[128];

    /* §5.4: device's supported protocol major range, valid after HARP_CLIENT_EINCOMPAT */
    uint32_t incompat_major_min, incompat_major_max;

    /* §5.5 core.changed (D->H "re-query topic X" hint): the last topic the device asked
     * us to re-query, recorded as the ntf arrives (on_ntf still fires too). The owner polls
     * changed_pending and re-queries (e.g. core.identify on "identity"), then clears it.
     * Today only harp-probe's core-test reads it; the shell records but does not yet act on
     * it (the refdev never emits core.changed spontaneously — it is driven via a test seam). */
    char last_changed_topic[32];
    bool changed_pending;
} harp_client;

void harp_client_init(harp_client *c, harp_io *io, harp_link *link, harp_store *store,
                      void (*on_ntf)(void *ud, const harp_env *e), void *ud);
void harp_client_free(harp_client *c);

/* ---- low level (for ops the library doesn't cover: audio.start, vendor
 * methods). Build the request with req_head + cbor, then request(). On
 * success *e points into rsp. ---- */
void harp_client_req_head(harp_client *c, harp_cbuf *out, const char *method,
                          bool has_body);
int harp_client_request(harp_client *c, harp_cbuf *req, harp_cbuf *rsp, harp_env *e);
/* split form, for flows that must service another pipe between the send
 * and the response (probe: audio.stop while draining the audio endpoint).
 * rid for wait is the one req_head stamped: client->next_rid. */
int harp_client_send(harp_client *c, const harp_cbuf *req);
int harp_client_wait(harp_client *c, uint64_t rid, harp_cbuf *rsp, harp_env *e);

/* ---- core ---- */
/* hello + identity parse + obj-credit grant. `agent` names this host. */
int harp_client_hello(harp_client *c, const char *agent, harp_client_identity *out);

/* §5.5 core methods (the device handlers exist; these are the missing host callers).
 *   identify — re-fetch identity (§6.2) WITHOUT resetting the session.
 *   ping     — liveness; the device echoes the body. Returns 0 iff the echo matches.
 *   bye      — orderly session end; the device acks then closes (MAY release host locks). */
int harp_client_identify(harp_client *c, harp_client_identity *out);
int harp_client_ping(harp_client *c);
int harp_client_bye(harp_client *c);

/* ---- recall ---- */
int harp_client_refs(harp_client *c, harp_ref *out, size_t cap, size_t *count);
int harp_client_find_ref(harp_client *c, const char *name, harp_ref *out);
/* snapshot-on-demand of `refname`; msg may be NULL */
int harp_client_snapshot(harp_client *c, const char *refname, const char *msg,
                         harp_hash *out_hash);
/* Fetch the closure of `root` into the local store (want -> obj stream).
 * fetched (may be NULL) reports objects actually transferred. */
int harp_client_fetch_closure(harp_client *c, const harp_hash *root, size_t *fetched);
/* Push the locally-held closure of `root` (have -> send -> obj stream).
 * sent/already (may be NULL) report the transfer split for narration. */
int harp_client_push_closure(harp_client *c, const harp_hash *root, size_t *sent,
                             size_t *already);
/* CAS: expect NULL = expect-unborn. create adds create-if-unborn; force (§11.3/
 * §11.4 action 1) overrides BOTH an expect mismatch and a dirty live ref. consent
 * (§13.4, flags bit 2 / 0x4) overrides the device's engine-version load gate — the
 * user's explicit "load this foreign-engine snapshot anyway". new_gen (may be NULL)
 * receives the post-CAS generation. */
int harp_client_refset(harp_client *c, const char *name, const harp_hash *expect,
                       const harp_hash *target, bool create, bool force, bool consent,
                       uint64_t *new_gen);

/* ---- §11.4 reconcile relay (front-panel-mediated) ----
 * offer: POST a conflict for the front panel (expect/live are short-hex DISPLAY
 * strings — the shell owns the real hashes). poll: read pending + the user's pick
 * (*choice: -1 none, 0 push / 1 pull / 2 read-only / 3 duplicate). */
int harp_client_reconcile_offer(harp_client *c, const char *expect, const char *live,
                                bool dirty);
int harp_client_reconcile_poll(harp_client *c, bool *pending, int *choice);

#endif /* HARP_CLIENT_H */
