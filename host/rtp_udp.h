/* UDP transport for the HARP §8.7 RTP audio plane (POSIX sockets).
 *
 * Receiver: owns a bound UDP socket and a free-running receiver. poll() drains
 * arriving packets — stamping each with the host monotonic arrival time, the
 * timing that drives clock recovery (freerun.h) — and feeds observe()+push();
 * pull() renders host-rate blocks. The two are meant to run on different
 * threads (a network thread polling, the audio callback pulling), exactly the
 * §3.2 plane separation; for a single-threaded driver, just interleave them.
 *
 * Sender: packs interleaved float32 into RTP and sends. The device side.
 */
#ifndef HARP_RTP_UDP_H
#define HARP_RTP_UDP_H

#include <stdint.h>
#include "freerun.h"
#include "rtp.h"

/* ---- receiver (host) ---- */
typedef struct harp_rtp_rx harp_rtp_rx;

/* Bind UDP on `port` (all interfaces) and create a free-running receiver from
 * cfg. NULL on failure (message on stderr). */
harp_rtp_rx *harp_rtp_rx_open(int port, const harp_freerun_cfg *cfg);
void         harp_rtp_rx_close(harp_rtp_rx *rx);

/* Drain all packets currently available (waiting up to timeout_ms for the
 * first). Each is arrival-stamped, unpacked, unwrapped, observed and pushed.
 * Returns packets processed, or -1 on a fatal socket error. Malformed packets
 * are counted (see _lost/_bad below) and skipped, never fatal. */
int          harp_rtp_rx_poll(harp_rtp_rx *rx, int timeout_ms);

/* Render exactly n interleaved frames at the host rate. */
unsigned     harp_rtp_rx_pull(harp_rtp_rx *rx, float *out, unsigned n);

void         harp_rtp_rx_stats(const harp_rtp_rx *rx, harp_freerun_stats *st);
/* Wire-level counters: packets accepted, dropped-as-lost (seq gaps), malformed. */
void         harp_rtp_rx_counters(const harp_rtp_rx *rx, unsigned long *ok,
                                  unsigned long *lost, unsigned long *bad);

/* ---- sender (device) ---- */
typedef struct harp_rtp_tx harp_rtp_tx;

harp_rtp_tx *harp_rtp_tx_open(const char *host, int port, unsigned channels, uint32_t ssrc);
void         harp_rtp_tx_close(harp_rtp_tx *tx);

/* Send nframes of interleaved float32 with RTP timestamp `ts` (device sample
 * index of the first frame). Returns 0 on success, -1 on send error. */
int          harp_rtp_tx_send(harp_rtp_tx *tx, uint32_t ts, const float *frames, unsigned nframes);

#endif /* HARP_RTP_UDP_H */
