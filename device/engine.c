/* engine.c — the refdev synth engine (split from harp-deviced.c; see device.h).
 *
 * Owns everything the render thread touches: the parameter bank, the mono
 * note voice, the timestamped event queue (§9.2), per-param ramps (§9.4),
 * and the audio thread itself (free-running and host-paced loops, §8).
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "device.h"

dev_param g_params[NPARAMS] = {
    {1, "Osc Pitch", 0.5f},   {2, "Osc Shape", 0.5f},    {3, "Filter Cutoff", 0.5f},
    {4, "Filter Reso", 0.5f}, {5, "Env Attack", 0.5f},   {6, "Env Release", 0.5f},
    {7, "Drone Mix", 0.5f},   {8, "Master Level", 0.5f},
};

volatile int g_note = -1;
volatile float g_note_vel = 0;
volatile uint32_t g_note_seq = 0;

static dev_event g_evq[DEV_EVQ_CAP];
size_t g_evq_n;
pthread_mutex_t g_evq_mu = PTHREAD_MUTEX_INITIALIZER;
volatile int g_touch_pending;
volatile uint64_t g_evq_drops;
volatile uint64_t g_evt_late;
volatile uint64_t g_ramp_late;
volatile uint32_t g_evt_consumed;
volatile uint64_t g_fence_waits;
volatile uint64_t g_fence_timeouts;

void evq_push(dev_event ev) {
    pthread_mutex_lock(&g_evq_mu);
    if (g_evq_n < DEV_EVQ_CAP)
        g_evq[g_evq_n++] = ev;
    else
        g_evq_drops++;
    pthread_mutex_unlock(&g_evq_mu);
}

dev_ramp g_ramps[NPARAMS];

/* Stream state reset (§7.1 in miniature): a new stream is a new time
 * domain — queued events and active ramps from the previous session's SSI
 * timeline are stale BY DEFINITION and must not leak into this one.
 * (Learned from a jammed queue of never-due zombie events silently
 * dropping every new event.) */
void evq_reset_for_new_stream(void) {
    pthread_mutex_lock(&g_evq_mu);
    g_evq_n = 0;
    pthread_mutex_unlock(&g_evq_mu);
    memset(g_ramps, 0, sizeof g_ramps);
    /* notes are performance state OF A STREAM: a note held across a stream
     * stop/restart is a stuck note (its note-off died with the old stream) */
    g_note = -1;
    /* fence sequence space restarts with the stream (host resets its
     * queued-event counter at session start; both sides count from 0) */
    __atomic_store_n(&g_evt_consumed, 0, __ATOMIC_RELEASE);
}
/* ---------------- the sound engine ---------------- */

/* A small stereo drone synth: blended sine/saw oscillator (R detuned for
 * width) through a Chamberlin state-variable lowpass. Every voice parameter
 * is one of the 8 recallable params — the point is recall you can hear. */
typedef struct {
    float phase_l, phase_r;
    float low_l, band_l, low_r, band_r;
    /* control-rate-smoothed parameter values (§9.3: the device interpolates
     * at its declared control rate — instant steps are zipper clicks) */
    float s_pitch, s_shape, s_cutoff, s_reso, s_master, s_drone;
    bool s_init;
    /* note voice: envelope-gated, portamento via smoothed frequency */
    float n_phase_l, n_phase_r;
    float n_low_l, n_band_l, n_low_r, n_band_r;
    float n_freq;   /* smoothed toward the sounding note */
    float env;      /* envelope level 0..1 */
    uint8_t env_reset; /* sub-blocks of fast decay before a fresh attack */
    uint32_t seen_seq;
} synth_voice;

static float param_value(uint32_t id) {
    for (size_t i = 0; i < NPARAMS; i++)
        if (g_params[i].id == id) return g_params[i].value;
    return 0.0f;
}

#define SMOOTH_SUBBLOCK 32 /* samples; 1.5 kHz at 48 k — the declared control rate honored */

/* Advance active ramps to stream position `pos`: the base value moves along
 * the line (visible to echo/refs — ramps change the stored value, §9.4). */
static void ramps_advance(uint64_t pos) {
    for (size_t i = 0; i < NPARAMS; i++) {
        dev_ramp *r = &g_ramps[i];
        if (!r->active) continue;
        if (pos >= r->end || r->end <= r->start) {
            g_params[i].value = r->target;
            r->active = false;
        } else if (pos > r->start) {
            float t = (float)(pos - r->start) / (float)(r->end - r->start);
            g_params[i].value = r->start_val + (r->target - r->start_val) * t;
        }
    }
}

static void engine_render(synth_voice *v, float *interleaved, uint32_t n, float rate,
                          uint64_t pos) {
    if (!v->s_init) { /* first block: land on targets, no glide-in from zero */
        v->s_pitch = param_value(1);
        v->s_shape = param_value(2);
        v->s_cutoff = param_value(3);
        v->s_reso = param_value(4);
        v->s_master = param_value(8);
        v->s_drone = param_value(7);
        v->n_freq = 220.0f;
        v->s_init = true;
    }
    float detune = 1.004f;
    /* one-pole toward targets per sub-block; tau ~= 12 ms */
    float alpha = 1.0f - expf(-(float)SMOOTH_SUBBLOCK / (0.012f * rate));

    uint32_t i = 0;
    while (i < n) {
        uint32_t end = i + SMOOTH_SUBBLOCK;
        if (end > n) end = n;

        ramps_advance(pos + i);
        v->s_pitch += alpha * (param_value(1) - v->s_pitch);
        v->s_shape += alpha * (param_value(2) - v->s_shape);
        v->s_cutoff += alpha * (param_value(3) - v->s_cutoff);
        v->s_reso += alpha * (param_value(4) - v->s_reso);
        v->s_master += alpha * (param_value(8) - v->s_master);
        v->s_drone += alpha * (param_value(7) - v->s_drone);

        /* note voice control: gate + envelope. Pitch SNAPS on a fresh
         * attack and glides only when legato (env still high): a fixed-tau
         * glide on every note makes perceived onsets interval-dependent —
         * measured r=0.86 between grid deviation and leap size, ±23 ms of
         * musical slop on sequenced 16ths. Real monosynths snap too. */
        int note = g_note;
        bool retrig = v->seen_seq != g_note_seq;
        if (retrig) {
            v->seen_seq = g_note_seq;
            /* Envelope RESETS per note: without this, articulation depends
             * on how far the previous release decayed (measured transient
             * depth 0.06..0.99 across one take — heard as timing slop).
             * Two fast-decay sub-blocks (~1.3 ms) to near-zero, click-free,
             * then a uniform attack. */
            if (v->env > 0.07f) v->env_reset = 2;
        }
        bool gate = note >= 0;
        if (gate) {
            float target = 440.0f * exp2f(((float)note - 69.0f) / 12.0f);
            if (retrig && v->env < 0.2f)
                v->n_freq = target; /* fresh attack: arrive on time */
            else
                v->n_freq += alpha * (target - v->n_freq); /* legato glide */
        }
        /* attack 1 ms..1 s, release 5 ms..3 s (exp-mapped knobs) */
        float atk_tau = 0.001f * exp2f(param_value(5) * 10.0f);
        float rel_tau = 0.005f * exp2f(param_value(6) * 9.2f);
        if (v->env_reset) {
            v->env *= 0.25f; /* fast decay sub-block */
            v->env_reset--;
        } else {
            float env_target = gate ? g_note_vel : 0.0f;
            float env_alpha =
                1.0f - expf(-(float)SMOOTH_SUBBLOCK / ((gate ? atk_tau : rel_tau) * rate));
            v->env += env_alpha * (env_target - v->env);
        }

        float freq = 55.0f * exp2f(v->s_pitch * 4.0f); /* drone: 55 Hz .. 880 Hz */
        float fc = 60.0f * exp2f(v->s_cutoff * 6.0f);
        float f = 2.0f * sinf((float)M_PI * fc / rate);
        float q = 1.0f - 0.9f * v->s_reso;
        float shape = v->s_shape, master = v->s_master;
        float drone_lvl = v->s_drone, env = v->env;
        float nfreq = v->n_freq;

        for (; i < end; i++) {
            /* drone oscillator pair */
            v->phase_l += freq / rate;
            if (v->phase_l >= 1.0f) v->phase_l -= 1.0f;
            v->phase_r += freq * detune / rate;
            if (v->phase_r >= 1.0f) v->phase_r -= 1.0f;
            float osc_l = sinf(2.0f * (float)M_PI * v->phase_l);
            osc_l += ((2.0f * v->phase_l - 1.0f) - osc_l) * shape;
            float osc_r = sinf(2.0f * (float)M_PI * v->phase_r);
            osc_r += ((2.0f * v->phase_r - 1.0f) - osc_r) * shape;

            /* note oscillator pair (own filter state, same patch character) */
            v->n_phase_l += nfreq / rate;
            if (v->n_phase_l >= 1.0f) v->n_phase_l -= 1.0f;
            v->n_phase_r += nfreq * detune / rate;
            if (v->n_phase_r >= 1.0f) v->n_phase_r -= 1.0f;
            float nosc_l = sinf(2.0f * (float)M_PI * v->n_phase_l);
            nosc_l += ((2.0f * v->n_phase_l - 1.0f) - nosc_l) * shape;
            float nosc_r = sinf(2.0f * (float)M_PI * v->n_phase_r);
            nosc_r += ((2.0f * v->n_phase_r - 1.0f) - nosc_r) * shape;

            v->low_l += f * v->band_l;
            float high_l = osc_l - v->low_l - q * v->band_l;
            v->band_l += f * high_l;
            v->low_r += f * v->band_r;
            float high_r = osc_r - v->low_r - q * v->band_r;
            v->band_r += f * high_r;

            v->n_low_l += f * v->n_band_l;
            float nhigh_l = nosc_l - v->n_low_l - q * v->n_band_l;
            v->n_band_l += f * nhigh_l;
            v->n_low_r += f * v->n_band_r;
            float nhigh_r = nosc_r - v->n_low_r - q * v->n_band_r;
            v->n_band_r += f * nhigh_r;

            interleaved[2 * i] = (v->low_l * drone_lvl + v->n_low_l * env) * master * 0.5f;
            interleaved[2 * i + 1] =
                (v->low_r * drone_lvl + v->n_low_r * env) * master * 0.5f;
        }
    }
}

/* Free-running stream thread (§8.3 mode 0): the device clock owns the
 * stream; the MSC counts rendered samples; frames carry (epoch, msc). */

static void render_with_events(synth_voice *v, float *interleaved, uint32_t n,
                               float rate, uint64_t pos);

/* Host-paced loop (§8.3 mode 1): block on pacing frames, render the exact
 * SSI range each one names, echo its (epoch, ts) on the output frame. The
 * voice starts from zero at audio.start, so identical state + identical
 * pacing -> byte-identical output (audio.deterministic, T15). Pacing faster
 * than real time is automatic — there is no clock here (audio.offline-rate). */
static void host_paced_loop(device *d) {
    audio_state *a = &d->audio;
    synth_voice voice = {0};
    uint8_t frame[HARP_AUDIO_HDR_LEN + AUDIO_MAX_NSAMPLES * 2 * 4];
    float samples[AUDIO_MAX_NSAMPLES * 2];
    /* buffered endpoint reads (packet-multiple, see ffs.c) */
    uint8_t rbuf[16384];
    size_t rlen = 0, rpos = 0;

    while (a->running) {
        uint8_t hdr[HARP_AUDIO_HDR_LEN];
        size_t need = sizeof hdr, got = 0;
        while (got < need) {
            if (rpos < rlen) {
                size_t take = rlen - rpos;
                if (take > need - got) take = need - got;
                memcpy(hdr + got, rbuf + rpos, take);
                rpos += take;
                got += take;
                continue;
            }
            ssize_t r = read(a->out_fd, rbuf, sizeof rbuf);
            if (r <= 0) { /* endpoint died or stop */
                fprintf(stderr, "harp-deviced: pacing read ended: %s\n",
                        r == 0 ? "EOF" : strerror(errno));
                return;
            }
            rlen = (size_t)r;
            rpos = 0;
        }
        harp_audio_hdr h;
        if (!harp_audio_hdr_decode(hdr, &h) || !(h.dirflags & HARP_AUDIO_DIR_H2D)) {
            d->frame_errors++;
            fprintf(stderr, "harp-deviced: malformed pacing frame (%02x %02x ...)\n",
                    hdr[0], hdr[1]);
            return; /* §4.2 spirit: malformed stream is fatal */
        }
        /* event fence (§8.3.1): events ride the link endpoint, pacing rides
         * this one — two pipes, no ordering between them. A fenced frame
         * names how many evt messages must be consumed before its range may
         * render; ordering becomes structural instead of probabilistic.
         * The wait is wire+parse time of in-flight events (typically µs,
         * absorbed by the host's ring cushion); the 5 ms bound keeps a
         * stalled host from wedging the stream — a timeout means a possibly
         * late event, and evt_late tells the truth about that. */
        if (h.dirflags & HARP_AUDIO_FENCE) {
            uint8_t fb[HARP_AUDIO_FENCE_LEN];
            size_t fgot = 0;
            while (fgot < sizeof fb) {
                if (rpos < rlen) {
                    size_t take = rlen - rpos;
                    if (take > sizeof fb - fgot) take = sizeof fb - fgot;
                    memcpy(fb + fgot, rbuf + rpos, take);
                    rpos += take;
                    fgot += take;
                    continue;
                }
                ssize_t r = read(a->out_fd, rbuf, sizeof rbuf);
                if (r <= 0) return;
                rlen = (size_t)r;
                rpos = 0;
            }
            uint32_t want = (uint32_t)fb[0] | ((uint32_t)fb[1] << 8) |
                            ((uint32_t)fb[2] << 16) | ((uint32_t)fb[3] << 24);
            if ((int32_t)(want - __atomic_load_n(&g_evt_consumed, __ATOMIC_ACQUIRE)) >
                0) {
                g_fence_waits++;
                int spins = 0;
                struct timespec fts = {0, 50000}; /* 50 µs */
                while ((int32_t)(want -
                                 __atomic_load_n(&g_evt_consumed, __ATOMIC_ACQUIRE)) >
                           0 &&
                       spins++ < 100 && a->running)
                    nanosleep(&fts, NULL);
                if ((int32_t)(want -
                              __atomic_load_n(&g_evt_consumed, __ATOMIC_ACQUIRE)) > 0)
                    g_fence_timeouts++;
            }
        }
        /* discard any input payload (this engine has no input channels) */
        size_t skip = harp_audio_payload_len(&h);
        while (skip) {
            if (rpos < rlen) {
                size_t take = rlen - rpos;
                if (take > skip) take = skip;
                rpos += take;
                skip -= take;
                continue;
            }
            ssize_t r = read(a->out_fd, rbuf, sizeof rbuf);
            if (r <= 0) return;
            rlen = (size_t)r;
            rpos = 0;
        }
        uint32_t n = h.nsamples;
        if (n > AUDIO_MAX_NSAMPLES) {
            d->frame_errors++;
            return;
        }
        render_with_events(&voice, samples, n, (float)a->rate, h.ts);
        harp_audio_hdr out = {HARP_AUDIO_FVER, 0, 2, h.epoch, h.ts, (uint16_t)n,
                              HARP_AUDIO_FMT_F32};
        harp_audio_hdr_encode(&out, frame);
        memcpy(frame + HARP_AUDIO_HDR_LEN, samples, (size_t)n * 2 * 4);
        if (!harp_write_all(a->fd, frame, HARP_AUDIO_HDR_LEN + (size_t)n * 2 * 4)) return;
    }
}

void *audio_thread(void *arg) {
    device *d = arg;
    audio_state *a = &d->audio;
    fprintf(stderr, "harp-deviced: audio thread up: mode=%u fd=%d out_fd=%d\n", a->mode,
            a->fd, a->out_fd);
    if (a->mode == 1) {
        host_paced_loop(d);
        fprintf(stderr, "harp-deviced: host-paced loop exited\n");
        return NULL;
    }
    synth_voice voice = {0};
    uint8_t frame[HARP_AUDIO_HDR_LEN + AUDIO_MAX_NSAMPLES * 2 * 4];
    float samples[AUDIO_MAX_NSAMPLES * 2];
    uint64_t msc = 0;
    uint64_t period_ns = (uint64_t)a->nsamples * 1000000000ull / a->rate;
    bool discont = false;

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (a->running) {
        render_with_events(&voice, samples, a->nsamples, (float)a->rate, msc);
        harp_audio_hdr h = {HARP_AUDIO_FVER, discont ? HARP_AUDIO_DISCONT : 0, 2,
                            a->epoch, msc, (uint16_t)a->nsamples, HARP_AUDIO_FMT_F32};
        discont = false;
        harp_audio_hdr_encode(&h, frame);
        size_t payload = (size_t)a->nsamples * 2 * 4;
        memcpy(frame + HARP_AUDIO_HDR_LEN, samples, payload);
        if (!harp_write_all(a->fd, frame, HARP_AUDIO_HDR_LEN + payload))
            break; /* endpoint died (stop/unplug) */
        msc += a->nsamples;

        next.tv_nsec += (long)(period_ns % 1000000000ull);
        next.tv_sec += (time_t)(period_ns / 1000000000ull);
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t behind_ns = (int64_t)(now.tv_sec - next.tv_sec) * 1000000000ll +
                            (now.tv_nsec - next.tv_nsec);
        if (behind_ns > 50 * 1000000ll) {
            /* transport stalled: re-anchor, surface it (§8.3 — never silent) */
            next = now;
            d->audio_overruns++;
            a->reanchors++;
            discont = true;
        } else {
#ifdef __linux__
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
#else
            /* macOS has no absolute clock_nanosleep; relative is fine for
             * the simulator (audio is USB/Linux-only anyway) */
            struct timespec rel = {0, (long)(-behind_ns)};
            if (behind_ns < 0) nanosleep(&rel, NULL);
#endif
        }
    }
    return NULL;
}

/* Apply queue events due at `pos`; return the next event timestamp inside
 * (pos, limit) for render segmentation, or 0 if none. Render thread only. */
static uint64_t evq_apply_due(uint64_t pos, uint64_t limit) {
    uint64_t next = 0;
    pthread_mutex_lock(&g_evq_mu);
    size_t w = 0;
    for (size_t i = 0; i < g_evq_n; i++) {
        dev_event *ev = &g_evq[i];
        if (ev->ts <= pos) {
            /* late = past the musical deadline (§9.2/§14.2): for ramps the
             * deadline is END (the start is the previous automation point,
             * legitimately in the recent past); for everything else, ts. */
            if (ev->kind == DEV_EV_RAMP) {
                if (ev->end && ev->end < pos) g_ramp_late++;
            } else if (ev->ts && ev->ts < pos)
                g_evt_late++;
            switch (ev->kind) {
                case DEV_EV_NOTE_ON:
                    g_note_vel = ev->v;
                    g_note = (int)ev->a;
                    g_note_seq++;
                    break;
                case DEV_EV_NOTE_OFF:
                    if (g_note == (int)ev->a) g_note = -1;
                    break;
                case DEV_EV_ALL_OFF:
                    g_note = -1;
                    break;
                case DEV_EV_PARAM_SET:
                    for (size_t j = 0; j < NPARAMS; j++)
                        if (g_params[j].id == ev->a) {
                            g_params[j].value = ev->v;
                            g_ramps[j].active = false; /* set supersedes ramp (§9.4) */
                        }
                    g_touch_pending = 1;
                    break;
                case DEV_EV_RAMP:
                    for (size_t j = 0; j < NPARAMS; j++)
                        if (g_params[j].id == ev->a) {
                            g_ramps[j].active = true;
                            g_ramps[j].start = pos;
                            g_ramps[j].end = ev->end;
                            g_ramps[j].start_val = g_params[j].value;
                            g_ramps[j].target = ev->v;
                        }
                    g_touch_pending = 1;
                    break;
            }
            continue; /* consumed */
        }
        if (ev->ts < limit && (next == 0 || ev->ts < next)) next = ev->ts;
        g_evq[w++] = *ev;
    }
    g_evq_n = w;
    pthread_mutex_unlock(&g_evq_mu);
    return next;
}

/* Render n samples starting at stream position `pos`, splitting at event
 * timestamps so application is sample-accurate (§9.2). */
static void render_with_events(synth_voice *v, float *interleaved, uint32_t n,
                               float rate, uint64_t pos) {
    uint32_t done = 0;
    while (done < n) {
        uint64_t next = evq_apply_due(pos + done, pos + n);
        uint32_t seg = next ? (uint32_t)(next - (pos + done)) : n - done;
        if (seg > n - done) seg = n - done;
        engine_render(v, interleaved + 2 * done, seg, rate, pos + done);
        done += seg;
    }
}

void audio_stop(device *d) {
    if (!d->audio.thread_live) return;
    d->audio.running = false;
    /* The thread may be parked in a blocking endpoint read (mode 1 pacing,
     * or a stalled write); read/write are cancellation points, and the loop
     * holds no resources that outlive it. A production device would use AIO
     * with a wakeup — for the refdev, cancel is honest and simple. */
    pthread_cancel(d->audio.thread);
    pthread_join(d->audio.thread, NULL);
    d->audio.thread_live = false;
    g_note = -1; /* never let a note outlive its stream */
    fprintf(stderr, "harp-deviced: audio stream stopped (%llu reanchors)\n",
            (unsigned long long)d->audio.reanchors);
}

