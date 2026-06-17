/* See freerun.h. Ring buffer + clock-recovery + libsamplerate ASRC.
 *
 * Clock recovery is TIMESTAMP-based: harp_freerun_observe() feeds (device sample
 * index, host arrival time) pairs, and the device rate is the slope of a linear
 * regression of dev_ts against host_ns. This is the airtight part — a frame
 * COUNT is quantized to whole frames, capping SINAD ~40 dB no matter how it is
 * smoothed, whereas host arrival time is continuous, so regressing against it
 * averages out packet-delay jitter to a sub-frame-precise rate. ratio =
 * host_rate / device_rate, one-pole-smoothed onto the applied ratio so it moves
 * without per-update steps (even ~1 ppm steps FM-modulate the tone). With the
 * ratio steady the resampler is pristine (>120 dB); the recovery's job is to
 * keep it steady and correct. No feedback loop on the buffer (libsamplerate's
 * internal dead-time makes those ring); the buffer just absorbs jitter.
 *
 * The regression here is cumulative (whole-stream) — simplest, and precision
 * improves over the run; it is well-conditioned for runs up to minutes. A
 * windowed/re-centred variant (for hours, or a clock that itself drifts) is a
 * later refinement. est_ppm reports the recovered device drift. */
#include "freerun.h"

#include <samplerate.h>
#include <stdlib.h>
#include <string.h>

#define FR_SMOOTH  0.002     /* one-pole LPF on the applied ratio (low FM)       */
#define FR_MAXADJ  0.05      /* ratio may deviate +/- 5% from nominal            */

struct harp_freerun {
    SRC_STATE *src;
    unsigned   ch;
    double     host_rate, dev_rate, nominal;   /* nominal = host/dev (out/in)  */
    unsigned   cap, target;
    float     *ring;
    unsigned   rd, wr, fill;
    double     ratio, ratio_target;
    /* timestamp regression (centred on the first observation) */
    int        obs_primed;
    unsigned long long ts0, host0;
    double     Sw, Sx, Sy, Sxx, Sxy;
    unsigned   underflow, overflow;
};

static long fr_input_cb(void *cb_data, float **data) {
    harp_freerun *fr = cb_data;
    if (fr->fill == 0) { *data = NULL; return 0; }
    unsigned contig = fr->cap - fr->rd;
    if (contig > fr->fill) contig = fr->fill;
    *data = fr->ring + (size_t)fr->rd * fr->ch;
    fr->rd = (fr->rd + contig) % fr->cap;
    fr->fill -= contig;
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
    fr->ratio = fr->ratio_target = fr->nominal;
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
    fr->ratio_target = t;
}

unsigned harp_freerun_push(harp_freerun *fr, const float *in, unsigned n) {
    unsigned space = fr->cap - fr->fill;
    unsigned take = n < space ? n : space;
    fr->overflow += n - take;
    unsigned first = fr->cap - fr->wr;
    if (first > take) first = take;
    memcpy(fr->ring + (size_t)fr->wr * fr->ch, in,
           (size_t)first * fr->ch * sizeof(float));
    if (take > first)
        memcpy(fr->ring, in + (size_t)first * fr->ch,
               (size_t)(take - first) * fr->ch * sizeof(float));
    fr->wr = (fr->wr + take) % fr->cap;
    fr->fill += take;
    return take;
}

unsigned harp_freerun_pull(harp_freerun *fr, float *out, unsigned n) {
    fr->ratio += FR_SMOOTH * (fr->ratio_target - fr->ratio);   /* smooth -> low FM */
    long made = src_callback_read(fr->src, fr->ratio, (long)n, out);
    if (made < 0) made = 0;
    if ((unsigned)made < n) {
        memset(out + (size_t)made * fr->ch, 0,
               (size_t)(n - (unsigned)made) * fr->ch * sizeof(float));
        fr->underflow += n - (unsigned)made;
    }
    return (unsigned)made;
}

void harp_freerun_get_stats(const harp_freerun *fr, harp_freerun_stats *st) {
    st->ratio = fr->ratio;
    st->est_ppm = (fr->nominal / fr->ratio_target - 1.0) * 1e6;   /* recovered drift */
    st->fill_frames = fr->fill;
    st->underflow_frames = fr->underflow;
    st->overflow_frames = fr->overflow;
}
