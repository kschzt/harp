/* See freerun.h. SPSC ring + clock-recovery + libsamplerate ASRC.
 *
 * THREADING (the contract this file guarantees):
 *   - PRODUCER thread (the RTP rx thread): harp_freerun_push() + harp_freerun_observe(),
 *     always co-resident on ONE thread. Owns `head` and the regression accumulator.
 *   - CONSUMER thread (the DAW audio callback): harp_freerun_pull(). Owns `tail` and
 *     the smoothed `ratio`. NEVER blocks, allocates, or syscalls.
 *   - OBSERVER (any thread): harp_freerun_get_stats() / _warm() — reads only atomics.
 * The ring is a real SPSC structure (monotonic atomic head/tail, release on the
 * producer's head store publishes its sample writes; the consumer acquire-loads
 * head). `ratio_target` crosses producer->consumer as a lock-free atomic (double
 * bit-punned to u64). Counters are atomic. This is the runtime's actual topology
 * (rx thread push, audio thread pull); the single-threaded rtp-demo path is a
 * special case of it. Verified by tests/harp_freerun_mt_tests.c under TSan.
 *
 * Clock recovery is TIMESTAMP-based: observe() feeds (device sample index, host
 * arrival time); the device rate is the slope of a cumulative linear regression
 * of dev_ts against host_ns — sub-frame precise (a frame COUNT caps SINAD ~40 dB;
 * continuous arrival time averages jitter to a precise rate). ratio = host/dev,
 * one-pole-smoothed so it moves without per-update steps (even ~1 ppm steps
 * FM-modulate the tone; at a steady ratio the resampler is >120 dB). est_ppm
 * reports the recovered drift. The regression is rx-thread-exclusive. */
#include "freerun.h"

#include <samplerate.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FR_SMOOTH  0.002     /* one-pole LPF on the applied ratio (low FM)       */
#define FR_MAXADJ  0.05      /* ratio may deviate +/- 5% from nominal            */

struct harp_freerun {
    SRC_STATE *src;
    unsigned   ch;
    double     host_rate, dev_rate, nominal;   /* nominal = host/dev (out/in)  */
    size_t     cap, target;                    /* ring capacity / warm setpoint, frames */
    float     *ring;
    /* SPSC ring: monotonic frame indices. head = producer (push), tail = consumer
     * (fr_input_cb). occupancy = head - tail (never wraps; size_t is 64-bit). */
    _Atomic size_t head, tail;
    _Atomic int    warm;                       /* producer latches once occupancy>=target */
    /* clock recovery: ratio_target crosses rx->audio (atomic, double bit-pun);
     * ratio is consumer-private; the regression state is producer-exclusive. */
    _Atomic uint64_t ratio_target_bits;
    double     ratio;
    int        obs_primed;
    unsigned long long ts0, host0;
    double     Sw, Sx, Sy, Sxx, Sxy;
    _Atomic unsigned underflow, overflow;
};

static void   fr_store_target(harp_freerun *fr, double v) {
    uint64_t b; memcpy(&b, &v, sizeof b);
    atomic_store_explicit(&fr->ratio_target_bits, b, memory_order_release);
}
static double fr_load_target(const harp_freerun *fr) {
    uint64_t b = atomic_load_explicit(&fr->ratio_target_bits, memory_order_acquire);
    double v; memcpy(&v, &b, sizeof v); return v;
}

/* CONSUMER side (audio thread, via src_callback_read): hand SRC a contiguous span
 * of frames the producer has PUBLISHED (acquire-load head), advance tail. */
static long fr_input_cb(void *cb_data, float **data) {
    harp_freerun *fr = cb_data;
    size_t head = atomic_load_explicit(&fr->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&fr->tail, memory_order_relaxed); /* consumer owns tail */
    size_t avail = head - tail;
    if (avail == 0) { *data = NULL; return 0; }
    size_t pos = tail % fr->cap;
    size_t contig = fr->cap - pos;
    if (contig > avail) contig = avail;
    *data = fr->ring + pos * fr->ch;
    atomic_store_explicit(&fr->tail, tail + contig, memory_order_release); /* free the span */
    return (long)contig;
}

harp_freerun *harp_freerun_new(const harp_freerun_cfg *c) {
    if (!c || c->channels < 1 || c->host_rate_hz <= 0 || c->dev_rate_hz <= 0 ||
        c->capacity_frames < 2 || c->target_frames >= c->capacity_frames)
        return NULL;
    harp_freerun *fr = calloc(1, sizeof *fr);
    if (!fr) return NULL;
    fr->ch = c->channels;
    fr->host_rate = c->host_rate_hz;
    fr->dev_rate = c->dev_rate_hz;
    fr->nominal = c->host_rate_hz / c->dev_rate_hz;
    fr->cap = c->capacity_frames;
    fr->target = c->target_frames;
    fr->ratio = fr->nominal;
    fr_store_target(fr, fr->nominal);
    fr->ring = malloc((size_t)c->capacity_frames * c->channels * sizeof(float));
    int err = 0;
    fr->src = src_callback_new(fr_input_cb, c->quality, (int)c->channels, &err, fr);
    if (!fr->src || !fr->ring) { harp_freerun_free(fr); return NULL; }
    return fr;
}

void harp_freerun_free(harp_freerun *fr) {
    if (!fr) return;
    if (fr->src) src_delete(fr->src);
    free(fr->ring);
    free(fr);
}

/* PRODUCER thread. */
void harp_freerun_observe(harp_freerun *fr, unsigned long long dev_ts,
                          unsigned long long host_ns) {
    if (!fr->obs_primed) { fr->obs_primed = 1; fr->ts0 = dev_ts; fr->host0 = host_ns; return; }
    double x = (double)(host_ns - fr->host0) / 1e9;       /* host seconds since anchor */
    double y = (double)(dev_ts - fr->ts0);                /* device samples since anchor */
    fr->Sw += 1; fr->Sx += x; fr->Sy += y; fr->Sxx += x*x; fr->Sxy += x*y;
    double denom = fr->Sw * fr->Sxx - fr->Sx * fr->Sx;
    if (denom <= 0) return;
    double slope = (fr->Sw * fr->Sxy - fr->Sx * fr->Sy) / denom;   /* dev samples/sec */
    if (slope <= 0) return;
    double t = fr->host_rate / slope;                     /* out/in == host/dev */
    double lo = fr->nominal * (1.0 - FR_MAXADJ), hi = fr->nominal * (1.0 + FR_MAXADJ);
    if (t < lo) t = lo; else if (t > hi) t = hi;
    fr_store_target(fr, t);
}

/* PRODUCER thread. */
unsigned harp_freerun_push(harp_freerun *fr, const float *in, unsigned n) {
    size_t head = atomic_load_explicit(&fr->head, memory_order_relaxed); /* producer owns head */
    size_t tail = atomic_load_explicit(&fr->tail, memory_order_acquire); /* see consumer frees */
    size_t space = fr->cap - (head - tail);
    unsigned take = (size_t)n < space ? n : (unsigned)space;
    if (n > take) atomic_fetch_add_explicit(&fr->overflow, n - take, memory_order_relaxed);
    size_t pos = head % fr->cap;
    size_t first = fr->cap - pos;
    if (first > take) first = take;
    memcpy(fr->ring + pos * fr->ch, in, (size_t)first * fr->ch * sizeof(float));
    if (take > first)
        memcpy(fr->ring, in + (size_t)first * fr->ch,
               (size_t)(take - first) * fr->ch * sizeof(float));
    atomic_store_explicit(&fr->head, head + take, memory_order_release); /* publish samples */
    if (!atomic_load_explicit(&fr->warm, memory_order_relaxed) &&
        (head + take - tail) >= fr->target)
        atomic_store_explicit(&fr->warm, 1, memory_order_release);
    return take;
}

/* CONSUMER thread (DAW audio callback). Never blocks/allocates/syscalls. */
unsigned harp_freerun_pull(harp_freerun *fr, float *out, unsigned n) {
    if (!atomic_load_explicit(&fr->warm, memory_order_acquire)) {
        memset(out, 0, (size_t)n * fr->ch * sizeof(float)); /* prebuffering: clean silence */
        return 0;
    }
    fr->ratio += FR_SMOOTH * (fr_load_target(fr) - fr->ratio);  /* smooth -> low FM */
    long made = src_callback_read(fr->src, fr->ratio, (long)n, out);
    if (made < 0) made = 0;
    if ((unsigned)made < n) {
        memset(out + (size_t)made * fr->ch, 0,
               (size_t)(n - (unsigned)made) * fr->ch * sizeof(float));
        atomic_fetch_add_explicit(&fr->underflow, n - (unsigned)made, memory_order_relaxed);
    }
    return (unsigned)made;
}

int harp_freerun_warm(const harp_freerun *fr) {
    return atomic_load_explicit(&fr->warm, memory_order_acquire);
}

void harp_freerun_get_stats(const harp_freerun *fr, harp_freerun_stats *st) {
    double tgt = fr_load_target(fr);
    st->ratio = tgt;
    st->est_ppm = (fr->nominal / tgt - 1.0) * 1e6;   /* recovered drift */
    size_t head = atomic_load_explicit(&fr->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&fr->tail, memory_order_acquire);
    st->fill_frames = (unsigned)(head - tail);
    st->underflow_frames = atomic_load_explicit(&fr->underflow, memory_order_relaxed);
    st->overflow_frames = atomic_load_explicit(&fr->overflow, memory_order_relaxed);
}
