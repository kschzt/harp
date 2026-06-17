/* See freerun.h. Ring buffer + clock-recovery loop + libsamplerate ASRC.
 *
 * Recovery structure: a FEEDFORWARD rate estimate (a slow EMA of input frames
 * arrived per output frame — i.e. the device/host rate, measured directly and
 * robustly) sets the bulk of the resample ratio, and a slow PROPORTIONAL trim
 * on the smoothed buffer level re-centers the elastic buffer. Feedforward is
 * driven by the arrival rate, not by the buffer level, so it is immune to the
 * lumpy way libsamplerate pulls input (its internal SINC buffering makes raw
 * ring-fill a noisy thing to control on). The trim is deliberately weak: the
 * buffer absorbs jitter, the loop tracks only the slow crystal drift. */
#include "freerun.h"

#include <samplerate.h>
#include <stdlib.h>
#include <string.h>

#define FR_BETA    0.02      /* arrival-rate EMA (feedforward) — carries the rate */
#define FR_ALPHA   0.02      /* buffer-level EMA (for the centering trim)        */
#define FR_KT      0.005     /* buffer-centering trim — gentle; FF does the work */
#define FR_MAXADJ  0.05      /* ratio may deviate +/- 5% from nominal            */

struct harp_freerun {
    SRC_STATE *src;
    unsigned   ch;
    double     host_rate, dev_rate, nominal;   /* nominal = host/dev (out/in)  */
    unsigned   cap, target;                    /* ring capacity / setpoint, fr */
    float     *ring;
    unsigned   rd, wr, fill;
    double     ipo_ema;                        /* input-per-output (1/rate)    */
    double     fill_ema, ratio;
    unsigned long long pushed_total, last_push;
    int        primed;                         /* first pull seeds last_push   */
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
    fr->ratio = fr->nominal;
    fr->ipo_ema = c->dev_rate_hz / c->host_rate_hz;   /* 1/nominal (in per out) */
    fr->fill_ema = (double)c->target_frames;
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
    fr->pushed_total += take;
    return take;
}

/* Once per pulled block of n output frames: update the feedforward rate
 * estimate from how much input arrived since the last pull, then apply the
 * slow buffer-centering trim. */
static void update_ratio(harp_freerun *fr, unsigned n) {
    unsigned long long delta = fr->pushed_total - fr->last_push;
    fr->last_push = fr->pushed_total;
    if (!fr->primed) {
        fr->primed = 1;            /* first delta spans the prebuffer -> discard */
    } else {
        double ipo = (double)delta / (double)n;      /* input arrived / output  */
        fr->ipo_ema += FR_BETA * (ipo - fr->ipo_ema);
    }
    double ratio_ff = 1.0 / fr->ipo_ema;             /* out/in == host/dev       */

    fr->fill_ema += FR_ALPHA * ((double)fr->fill - fr->fill_ema);
    double trim = FR_KT * (fr->fill_ema - (double)fr->target) / (double)fr->target;

    double corr = 1.0 - trim;                        /* fill high -> consume more */
    double r = ratio_ff * corr;
    double lo = fr->nominal * (1.0 - FR_MAXADJ), hi = fr->nominal * (1.0 + FR_MAXADJ);
    if (r < lo) r = lo; else if (r > hi) r = hi;
    fr->ratio = r;
}

unsigned harp_freerun_pull(harp_freerun *fr, float *out, unsigned n) {
    update_ratio(fr, n);
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
    st->est_ppm = (fr->nominal / fr->ratio - 1.0) * 1e6;
    st->fill_frames = fr->fill;
    st->underflow_frames = fr->underflow;
    st->overflow_frames = fr->overflow;
}
