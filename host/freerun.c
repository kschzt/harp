/* See freerun.h. Ring buffer + clock-recovery loop + libsamplerate ASRC.
 *
 * Recovery is FEEDFORWARD with a smoothed ratio:
 *   target ratio  = total output / total input arrived   (cumulative)
 *   applied ratio = one-pole low-pass of that target
 *
 * The cumulative output/arrival count is a bias-free estimate of host/device
 * that converges to a CONSTANT for a constant crystal drift — no feedback loop,
 * so libsamplerate's internal buffering (a dead-time that makes buffer-feedback
 * loops ring) can't destabilize it. The one-pole low-pass is the airtight part:
 * the raw cumulative ratio still steps by ~1 ppm per pull (integer division),
 * and even 1-ppm ratio steps FM-modulate the output enough to cap SINAD ~40 dB;
 * smoothing the applied ratio removes that, recovering the converter's full
 * quality (>120 dB on a constant ratio). Rate-matching keeps the elastic buffer
 * from drifting; the buffer absorbs per-packet jitter.
 *
 * Reported est_ppm is the EFFECTIVE ratio (total output / total input consumed)
 * — the ratio actually realized. */
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
    double     ratio;
    unsigned long long pushed_total, last_push;   /* arrivals (prebuffer excl.) */
    unsigned long long consumed_total, output_total;
    int        primed;
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
    fr->consumed_total += contig;
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

static void update_ratio(harp_freerun *fr) {
    if (!fr->primed) { fr->primed = 1; fr->last_push = fr->pushed_total; return; }
    unsigned long long pin = fr->pushed_total - fr->last_push;
    if (pin == 0 || fr->output_total == 0) return;
    double target = (double)fr->output_total / (double)pin;       /* host/dev */
    double lo = fr->nominal * (1.0 - FR_MAXADJ), hi = fr->nominal * (1.0 + FR_MAXADJ);
    if (target < lo) target = lo; else if (target > hi) target = hi;
    fr->ratio += FR_SMOOTH * (target - fr->ratio);                /* smooth -> low FM */
}

unsigned harp_freerun_pull(harp_freerun *fr, float *out, unsigned n) {
    update_ratio(fr);
    long made = src_callback_read(fr->src, fr->ratio, (long)n, out);
    if (made < 0) made = 0;
    fr->output_total += (unsigned)made;
    if ((unsigned)made < n) {
        memset(out + (size_t)made * fr->ch, 0,
               (size_t)(n - (unsigned)made) * fr->ch * sizeof(float));
        fr->underflow += n - (unsigned)made;
    }
    return (unsigned)made;
}

void harp_freerun_get_stats(const harp_freerun *fr, harp_freerun_stats *st) {
    st->ratio = fr->ratio;
    double eff = fr->consumed_total ? (double)fr->output_total / (double)fr->consumed_total
                                    : fr->nominal;
    st->est_ppm = (fr->nominal / eff - 1.0) * 1e6;
    st->fill_frames = fr->fill;
    st->underflow_frames = fr->underflow;
    st->overflow_frames = fr->overflow;
}
