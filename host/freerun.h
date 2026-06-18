/* HARP free-running stream receiver — host-side clock recovery + ASRC.
 *
 * The §8.3 free-running model: a device streams audio paced by its own crystal;
 * the host runs on a different clock (the DAW's). This module recovers the
 * device's effective rate from the stream alone — no PTP, no shared clock
 * (§7.3 receiver-side recovery, "P2") — and asynchronously resamples into the
 * host clock domain, holding a fixed-depth elastic buffer so arrival jitter
 * never underflows and reported latency stays constant.
 *
 * Transport-agnostic by design: the producer pushes interleaved frames as they
 * arrive (a USB §8.2 stream frame or an RTP/UDP §8.7 payload — this module
 * neither knows nor cares), the consumer pulls fixed host-rate blocks from the
 * DAW callback. The resampler is libsamplerate (varispeed SINC, which already
 * exceeds the §8.3 stopband floor); the jitter buffer and the rate-tracking
 * control loop — the genuinely HARP-specific policy — live here. This is why
 * the same core serves USB free-running (analog/FX devices) and the Ethernet
 * binding: only the front-end that calls push() differs.
 *
 * NOT for the host-paced/offline path (§8.3 mode 1): that renders exact SSI
 * ranges with no ASRC and stays byte-deterministic (T15). Free-running output
 * tracks a real, drifting clock and is non-deterministic by nature.
 */
#ifndef HARP_FREERUN_H
#define HARP_FREERUN_H

typedef struct harp_freerun harp_freerun;

typedef struct {
    unsigned channels;          /* interleave width (>= 1)                      */
    double   host_rate_hz;      /* output (DAW) sample rate                     */
    double   dev_rate_hz;       /* device nominal rate (seeds the initial ratio)*/
    unsigned target_frames;     /* elastic-buffer setpoint — the latency knob   */
    unsigned capacity_frames;   /* ring capacity; input past it drops + counts  */
    int      quality;           /* libsamplerate converter (SRC_SINC_*_QUALITY) */
} harp_freerun_cfg;

typedef struct {
    double   ratio;             /* resample ratio currently applied (host/dev)  */
    double   est_ppm;           /* estimated device drift vs host               */
    unsigned fill_frames;       /* current buffer occupancy                     */
    unsigned underflow_frames;  /* output frames synthesized on an empty buffer */
    unsigned overflow_frames;   /* input frames dropped on a full buffer        */
    double   jitter_us;         /* residual arrival jitter (RMS of the recovery
                                 * regression), as arrival-time error in µs — the
                                 * floor the recovery must average down (~50 µs in
                                 * the unit test that hits 90.9 dB) */
} harp_freerun_stats;

/* NULL on bad cfg or allocation/SRC-init failure. */
harp_freerun *harp_freerun_new(const harp_freerun_cfg *cfg);
void          harp_freerun_free(harp_freerun *fr);

/* Feed a timing observation for clock recovery: dev_ts is the device sample
 * index of an arriving packet (its RTP/stream timestamp), host_ns the host
 * monotonic time it arrived. The device rate is recovered from the regression
 * of dev_ts against host_ns — sub-frame precise, unlike a frame count, which is
 * what lifts SINAD to the resampler's ceiling. Call once per arriving packet,
 * before/with the matching push(). (Optional: with no observations the ratio
 * stays nominal.) */
void harp_freerun_observe(harp_freerun *fr, unsigned long long dev_ts,
                          unsigned long long host_ns);

/* Producer: enqueue nframes of interleaved input just arrived from the device.
 * Returns frames accepted (< nframes iff the ring overflowed; the rest drop). */
unsigned harp_freerun_push(harp_freerun *fr, const float *in, unsigned nframes);

/* Consumer (DAW callback): render exactly nframes of interleaved output at the
 * host rate. Always fills the whole block; on starvation it emits silence for
 * the shortfall and counts it. Returns the count of non-silent frames. */
unsigned harp_freerun_pull(harp_freerun *fr, float *out, unsigned nframes);

void harp_freerun_get_stats(const harp_freerun *fr, harp_freerun_stats *st);

/* True once the elastic buffer has filled to target_frames at least once (the
 * producer latches it). The consumer pulls silence until then. Lets the rx side
 * mark a session "ready" off the audio thread — never spin/poll on warm-up from
 * the audio callback. Observer-safe (atomic). */
int harp_freerun_warm(const harp_freerun *fr);

#endif /* HARP_FREERUN_H */
