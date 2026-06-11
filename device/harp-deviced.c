/* harp-deviced — HARP reference device daemon (spec draft 0.3).
 *
 * Implements the device side of `harp-core` + `harp-recall` over a TCP dev
 * transport (the framed link of §4.2 over a socket). On the Pi 4B the same
 * daemon will later speak FunctionFS bulk endpoints; the link layer is
 * fd-based so only the accept path changes.
 *
 * NOTE (§4.4): TCP here is a development transport for the simulator and for
 * Linux boards (KR260), not a conformance-bearing `harp` network binding.
 *
 * The "engine" is a bank of named parameters — enough to make state real:
 * knob turns dirty the live ref, snapshots serialize it, refsets restore it.
 *
 * Implementation choices where the spec is silent (candidate HEP notes):
 *   - state.want responds {0: count-of-objects-queued} before sending.
 *   - refset closure validation covers root/tree/blob reachability but does
 *     NOT require snapshot parent ancestry to be present (§15.3 "full
 *     closure" would otherwise mean unbounded history in every bundle).
 */
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "harp/audio.h"
#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/object.h"
#include "harp/store.h"

#define PROTO_MAJOR 1
#define PROTO_MINOR 0
#define ENGINE_ID "refdev-null"
#define ENGINE_VERSION "1.0.0"
#define FW_VERSION "0.1.0"
#define CREDIT_GRANT (16u << 20)
#define LIVE_REF "live/project"
#define PARAMS_MEDIA "application/x.harp-refdev.params"

typedef struct {
    uint32_t id;
    const char *name;
    float value;
} dev_param;

static dev_param g_params[] = {
    {1, "Osc Pitch", 0.5f},   {2, "Osc Shape", 0.5f},    {3, "Filter Cutoff", 0.5f},
    {4, "Filter Reso", 0.5f}, {5, "Env Attack", 0.5f},   {6, "Env Release", 0.5f},
    {7, "Drone Mix", 0.5f},   {8, "Master Level", 0.5f},
};
#define NPARAMS (sizeof g_params / sizeof g_params[0])

/* live performance state (notes are events, not patch state — they never
 * touch the dirty flag). Mono, last-note priority. Written by the session
 * thread, read by the audio thread; benign word-sized races. */
static volatile int g_note = -1;       /* sounding MIDI note, -1 = none */
static volatile float g_note_vel = 0;  /* 0..1 */
static volatile uint32_t g_note_seq = 0; /* bumps on every note-on (retrigger) */

/* ---- timestamped event queue (§9.2): session thread pushes, render
 * thread pops at exact stream positions. ts is SSI (host-paced) or MSC
 * (free-running); ts == 0 means "now". ---- */
enum { DEV_EV_NOTE_ON, DEV_EV_NOTE_OFF, DEV_EV_PARAM_SET, DEV_EV_RAMP, DEV_EV_ALL_OFF };

typedef struct {
    uint64_t ts;   /* apply at this stream position (0 = asap) */
    uint8_t kind;
    uint32_t a;    /* note number or param id */
    float v;       /* velocity / value / ramp target */
    uint64_t end;  /* ramp end position */
} dev_event;

#define DEV_EVQ_CAP 256
static dev_event g_evq[DEV_EVQ_CAP];
static size_t g_evq_n;
static pthread_mutex_t g_evq_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_touch_pending; /* dirty-flag work deferred off the render thread */
static volatile uint64_t g_evq_drops; /* ring full — counted, never silent (§14.1) */
static volatile uint64_t g_evt_late;  /* notes/sets applied past ts (§14.2) — keep ZERO */
static volatile uint64_t g_ramp_late; /* ramps arriving past their END deadline: degraded
                                         to a late set. Structurally possible under flood
                                         (ramp start = previous point => one-block margin);
                                         budgeted, not zero-tolerance. */

static void evq_push(dev_event ev) {
    pthread_mutex_lock(&g_evq_mu);
    if (g_evq_n < DEV_EVQ_CAP)
        g_evq[g_evq_n++] = ev;
    else
        g_evq_drops++;
    pthread_mutex_unlock(&g_evq_mu);
}


/* per-param ramp state (§9.4); render-thread-only after activation */
typedef struct {
    bool active;
    uint64_t start, end;
    float start_val, target;
} dev_ramp;
static dev_ramp g_ramps[NPARAMS];

/* Stream state reset (§7.1 in miniature): a new stream is a new time
 * domain — queued events and active ramps from the previous session's SSI
 * timeline are stale BY DEFINITION and must not leak into this one.
 * (Learned from a jammed queue of never-due zombie events silently
 * dropping every new event.) */
static void evq_reset_for_new_stream(void) {
    pthread_mutex_lock(&g_evq_mu);
    g_evq_n = 0;
    pthread_mutex_unlock(&g_evq_mu);
    memset(g_ramps, 0, sizeof g_ramps);
    /* notes are performance state OF A STREAM: a note held across a stream
     * stop/restart is a stuck note (its note-off died with the old stream) */
    g_note = -1;
}

/* audio plane state (§8): one D→H stream.
 * mode 0: free-running, paced by the device clock, MSC timestamps.
 * mode 1: host-paced — no timers; renders exactly the SSI ranges the host
 *         supplies in pacing frames on the audio OUT endpoint. */
typedef struct {
    pthread_t thread;
    volatile bool running;
    bool thread_live;
    int fd;     /* audio IN endpoint: device -> host */
    int out_fd; /* audio OUT endpoint: host -> device (pacing, mode 1) */
    uint32_t mode, rate, nsamples, epoch;
    uint64_t reanchors;
} audio_state;

typedef struct {
    harp_store store;
    char serial[64];
    harp_hash param_map_hash;
    uint64_t boot_count;

    harp_io *io; /* NULL when no session transport is attached */
    harp_link link;
    bool hello_done;
    bool closing;
    uint64_t peer_credit;   /* bytes we may still send on obj */
    uint64_t granted;       /* unconsumed credit we granted the peer */

    audio_state audio;

    /* live-ref write coalescing: only the clean->dirty transition must hit
     * storage synchronously (it's the loss-safety edge); generation bumps
     * during continuous editing stay in memory (fsync per knob tick starves
     * the audio path on SD cards). Flushed before any reader (§10.3:
     * notification MAY be coalesced; terminal state must be visible). */
    harp_ref live_cache;
    bool live_cache_valid;
    uint64_t last_live_ntf_ms;

    /* knob sources are multi-threaded now (session loop, HTTP panel):
     * send_mu serializes link writes; state_mu guards the live-ref cache */
    pthread_mutex_t send_mu, state_mu;

    /* counters (§14.2) */
    uint64_t frame_errors, session_resets, snapshots_taken, audio_overruns;
} device;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

static device g_dev;

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
#define AUDIO_MAX_NSAMPLES 1024

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

static void *audio_thread(void *arg) {
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

static void audio_stop(device *d) {
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

/* ---------------- engine state <-> objects ---------------- */

static void engine_encode_params_blob(harp_cbuf *out) {
    harp_cbuf payload;
    harp_cbuf_init(&payload);
    harp_cbor_map(&payload, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) { /* ascending ids == deterministic order */
        harp_cbor_uint(&payload, g_params[i].id);
        harp_cbor_float(&payload, g_params[i].value);
    }
    harp_obj_encode_blob(out, PARAMS_MEDIA, payload.buf, payload.len);
    harp_cbuf_free(&payload);
}

/* Serialize live state: params blob -> tree -> snapshot. Returns 0 and the
 * snapshot hash, or -1. */
static int engine_snapshot_objects(device *d, const harp_hash *parent, const char *msg,
                                   harp_hash *out_snap) {
    harp_cbuf enc;
    harp_cbuf_init(&enc);

    engine_encode_params_blob(&enc);
    harp_hash blob_h;
    if (harp_store_put(&d->store, enc.buf, enc.len, &blob_h) != 0) goto fail;

    harp_cbuf_reset(&enc);
    harp_tree_entry entries[1] = {{"params", blob_h, HARP_OBJ_BLOB}};
    harp_obj_encode_tree(&enc, entries, 1);
    harp_hash tree_h;
    if (harp_store_put(&d->store, enc.buf, enc.len, &tree_h) != 0) goto fail;

    harp_cbuf_reset(&enc);
    harp_obj_encode_snapshot(&enc, &tree_h, parent, parent ? 1 : 0,
                             (uint64_t)time(NULL), "device", ENGINE_VERSION, msg);
    if (harp_store_put(&d->store, enc.buf, enc.len, out_snap) != 0) goto fail;

    harp_cbuf_free(&enc);
    return 0;
fail:
    harp_cbuf_free(&enc);
    return -1;
}

/* Load a snapshot into the live engine. Returns 0, or -1 (closure incomplete
 * or malformed — nothing is applied; §11.3 atomic apply). */
struct load_ctx {
    device *d;
    float staged[NPARAMS];
    bool ok;
};

static bool load_tree_entry(const char *name, size_t name_len, const harp_hash *h,
                            uint32_t kind, void *ud) {
    struct load_ctx *ctx = ud;
    if (name_len != 6 || memcmp(name, "params", 6) != 0 || kind != HARP_OBJ_BLOB)
        return true; /* unknown entries are ignored, not fatal */
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    if (harp_store_get(&ctx->d->store, h, &enc) != 0) {
        harp_cbuf_free(&enc);
        ctx->ok = false;
        return false;
    }
    const uint8_t *payload;
    size_t payload_len;
    if (!harp_obj_parse_blob(enc.buf, enc.len, NULL, NULL, &payload, &payload_len)) {
        harp_cbuf_free(&enc);
        ctx->ok = false;
        return false;
    }
    harp_cdec dec;
    harp_cdec_init(&dec, payload, payload_len);
    uint64_t n;
    if (harp_cdec_map(&dec, &n)) {
        for (uint64_t i = 0; i < n; i++) {
            uint64_t id;
            double v;
            if (!harp_cdec_uint(&dec, &id) || !harp_cdec_float(&dec, &v)) break;
            for (size_t j = 0; j < NPARAMS; j++)
                if (g_params[j].id == id) ctx->staged[j] = (float)v;
        }
    }
    harp_cbuf_free(&enc);
    return true;
}

static int engine_load_snapshot(device *d, const harp_hash *snap_h) {
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    if (harp_store_get(&d->store, snap_h, &enc) != 0) goto fail;
    harp_hash root;
    if (!harp_obj_parse_snapshot_root(enc.buf, enc.len, &root)) goto fail;

    harp_cbuf tree_enc;
    harp_cbuf_init(&tree_enc);
    if (harp_store_get(&d->store, &root, &tree_enc) != 0) {
        harp_cbuf_free(&tree_enc);
        goto fail;
    }
    struct load_ctx ctx = {d, {0}, true};
    for (size_t i = 0; i < NPARAMS; i++) ctx.staged[i] = g_params[i].value;
    bool walked = harp_obj_tree_foreach(tree_enc.buf, tree_enc.len, load_tree_entry, &ctx);
    harp_cbuf_free(&tree_enc);
    if (!walked || !ctx.ok) goto fail;

    /* stage, then commit: all-or-nothing into the live engine */
    for (size_t i = 0; i < NPARAMS; i++) g_params[i].value = ctx.staged[i];
    harp_cbuf_free(&enc);
    return 0;
fail:
    harp_cbuf_free(&enc);
    return -1;
}

/* Canonical parameter descriptor array (§9.3). param-map-hash is the
 * SHA-256 of this exact deterministic encoding — identity and evt.params
 * MUST agree. */
static void encode_param_array(harp_cbuf *b) {
    harp_cbor_array(b, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) {
        harp_cbor_map(b, 3);
        harp_cbor_uint(b, 0);
        harp_cbor_uint(b, g_params[i].id);
        harp_cbor_uint(b, 1);
        harp_cbor_text(b, g_params[i].name);
        harp_cbor_uint(b, 8);
        harp_cbor_uint(b, 0x1); /* flags: automatable */
    }
}

static void compute_param_map_hash(device *d) {
    harp_cbuf b;
    harp_cbuf_init(&b);
    encode_param_array(&b);
    d->param_map_hash = harp_hash_compute(b.buf, b.len);
    harp_cbuf_free(&b);
}

/* ---------------- wire helpers ---------------- */

static int send_ctl(device *d, const harp_cbuf *msg) {
    pthread_mutex_lock(&d->send_mu);
    int rc = harp_link_send(d->io, HARP_STREAM_CTL, msg->buf, msg->len);
    pthread_mutex_unlock(&d->send_mu);
    return rc;
}

/* Echo a base-value change as a param event on the evt stream (§9.4:
 * REQUIRED for front-panel and internally-driven changes; host-driven
 * param events are NOT echoed back). Timestamp (0,0) = "now" until the
 * timestamping slice lands. */
static void evt_echo_param(device *d, uint32_t id, float v) {
    if (!d->io || !d->hello_done) return;
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_cbor_array(&m, 3);
    harp_cbor_array(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, 1); /* etype: param */
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, id);
    harp_cbor_uint(&m, 1);
    harp_cbor_float(&m, v);
    pthread_mutex_lock(&d->send_mu);
    harp_link_send(d->io, HARP_STREAM_EVT, m.buf, m.len);
    pthread_mutex_unlock(&d->send_mu);
    harp_cbuf_free(&m);
}

static void ntf_state_changed(device *d, const harp_ref *r) {
    if (!d->io || !d->hello_done) return;
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "state.changed", true);
    harp_cbor_map(&m, 4);
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, r->name);
    harp_cbor_uint(&m, 1);
    if (r->unborn)
        harp_cbor_null(&m);
    else
        harp_cbor_bytes(&m, r->hash.b, HARP_HASH_LEN);
    harp_cbor_uint(&m, 2);
    harp_cbor_uint(&m, r->generation);
    harp_cbor_uint(&m, 3);
    harp_cbor_bool(&m, r->dirty);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void grant_credit(device *d) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "core.credit", true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, CREDIT_GRANT);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    d->granted += CREDIT_GRANT;
}

static void send_error(device *d, uint64_t rid, const char *method, const char *code,
                       const char *msg) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    harp_env_error(&m, rid, method, code, msg);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* ---------------- identity (§6.2) ---------------- */

static void encode_identity(device *d, harp_cbuf *m) {
    harp_cbor_map(m, 11);
    harp_cbor_uint(m, 0); /* vendor */
    harp_cbor_map(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 0x1209); /* pid.codes prototype VID */
    harp_cbor_uint(m, 1);
    harp_cbor_text(m, "HARP Reference Project");
    harp_cbor_uint(m, 1); /* product */
    harp_cbor_map(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 0x0001);
    harp_cbor_uint(m, 1);
    harp_cbor_text(m, "harp-refdev");
    harp_cbor_uint(m, 2); /* serial */
    harp_cbor_text(m, d->serial);
    harp_cbor_uint(m, 3); /* firmware */
    harp_cbor_text(m, FW_VERSION);
    harp_cbor_uint(m, 4); /* engine */
    harp_cbor_map(m, 3);
    harp_cbor_uint(m, 0);
    harp_cbor_text(m, ENGINE_ID);
    harp_cbor_uint(m, 1);
    harp_cbor_text(m, ENGINE_VERSION);
    harp_cbor_uint(m, 2);
    harp_cbor_bytes(m, d->param_map_hash.b, HARP_HASH_LEN);
    harp_cbor_uint(m, 5); /* protocol in effect */
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, PROTO_MAJOR);
    harp_cbor_uint(m, PROTO_MINOR);
    harp_cbor_uint(m, 6); /* capabilities */
    harp_cbor_array(m, 9);
    harp_cbor_text(m, "harp-core");
    harp_cbor_text(m, "harp-recall");
    harp_cbor_text(m, "harp-stream");
    harp_cbor_text(m, "audio.host-paced");
    harp_cbor_text(m, "audio.deterministic");
    harp_cbor_text(m, "audio.offline-rate");
    harp_cbor_text(m, "evt.param");
    harp_cbor_text(m, "evt.param.echo");
    harp_cbor_text(m, "x.harp-refdev.sim");
    harp_cbor_uint(m, 7); /* channel map (§6.3): stereo main mix, D→H */
    harp_cbor_array(m, 2);
    for (int ch = 0; ch < 2; ch++) {
        harp_cbor_map(m, 6);
        harp_cbor_uint(m, 0);
        harp_cbor_uint(m, ch); /* slot */
        harp_cbor_uint(m, 1);
        harp_cbor_uint(m, 0); /* direction: device -> host */
        harp_cbor_uint(m, 2);
        harp_cbor_text(m, ch ? "Main R" : "Main L");
        harp_cbor_uint(m, 3);
        harp_cbor_text(m, "Mix");
        harp_cbor_uint(m, 4);
        harp_cbor_text(m, "main");
        harp_cbor_uint(m, 5);
        harp_cbor_bool(m, true); /* host-paced capable: pure-digital path */
    }
    harp_cbor_uint(m, 8); /* latency profile (§6.4): pure-digital engine */
    harp_cbor_array(m, 1);
    harp_cbor_map(m, 4);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 48000);
    harp_cbor_uint(m, 1);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, 3);
    harp_cbor_uint(m, 256);
    harp_cbor_uint(m, 9); /* build id */
    harp_cbor_text(m, "refdev sim " __DATE__);
    harp_cbor_uint(m, 10); /* boot count */
    harp_cbor_uint(m, d->boot_count);
}

/* ---------------- ref helpers ---------------- */

/* Bump generation + dirty on the live ref. Persists only the clean->dirty
 * transition; later bumps coalesce in memory (see device struct comment). */
static void live_ref_touch(device *d, bool dirty) {
    pthread_mutex_lock(&d->state_mu);
    if (!d->live_cache_valid) {
        if (harp_store_ref_read(&d->store, LIVE_REF, &d->live_cache) != 0) {
            pthread_mutex_unlock(&d->state_mu);
            return;
        }
        d->live_cache_valid = true;
    }
    bool transition = dirty && !d->live_cache.dirty;
    d->live_cache.generation++;
    d->live_cache.dirty = dirty;
    if (transition) harp_store_ref_write(&d->store, &d->live_cache);
    uint64_t now = now_ms();
    bool do_ntf = transition || now - d->last_live_ntf_ms >= 250;
    harp_ref snapshot_ref = d->live_cache;
    if (do_ntf) d->last_live_ntf_ms = now;
    pthread_mutex_unlock(&d->state_mu);
    if (do_ntf) ntf_state_changed(d, &snapshot_ref);
}

/* Flush the coalesced live ref before anything reads it from storage. */
static void live_cache_flush(device *d) {
    pthread_mutex_lock(&d->state_mu);
    if (d->live_cache_valid) {
        harp_store_ref_write(&d->store, &d->live_cache);
        d->live_cache_valid = false; /* re-read after external mutation */
    }
    pthread_mutex_unlock(&d->state_mu);
}

/* The one true front-panel path: web panel, vendor knob method, and any
 * future GPIO encoders all come through here — set, dirty, echo. */
static bool front_panel_set(device *d, uint32_t id, double v) {
    bool found = false;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    for (size_t i = 0; i < NPARAMS; i++)
        if (g_params[i].id == id) {
            g_params[i].value = (float)v;
            found = true;
        }
    if (!found) return false;
    live_ref_touch(d, true);
    evt_echo_param(d, id, (float)v);
    return true;
}

static int do_snapshot(device *d, const char *msg, harp_hash *out, uint64_t *out_gen) {
    live_cache_flush(d);
    harp_ref r;
    if (harp_store_ref_read(&d->store, LIVE_REF, &r) != 0) return -1;
    harp_hash snap;
    if (engine_snapshot_objects(d, r.unborn ? NULL : &r.hash, msg, &snap) != 0) return -1;
    r.unborn = false;
    r.hash = snap;
    r.generation++;
    r.dirty = false;
    if (harp_store_ref_write(&d->store, &r) != 0) return -1;
    d->snapshots_taken++;
    ntf_state_changed(d, &r);
    *out = snap;
    if (out_gen) *out_gen = r.generation;
    return 0;
}

/* Closure check for refset (§11.3): root snapshot -> tree -> children present.
 * Parent ancestry deliberately not required (see header note). */
struct closure_ctx {
    device *d;
    bool complete;
    int depth;
};

static bool closure_visit(const harp_hash *h, void *ud);

static void closure_walk(struct closure_ctx *ctx, const harp_hash *h) {
    if (!ctx->complete || ctx->depth > 16) return;
    if (!harp_store_have(&ctx->d->store, h)) {
        ctx->complete = false;
        return;
    }
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    if (harp_store_get(&ctx->d->store, h, &enc) != 0) {
        ctx->complete = false;
    } else {
        ctx->depth++;
        if (!harp_obj_foreach_child(enc.buf, enc.len, false, closure_visit, ctx))
            ctx->complete = false;
        ctx->depth--;
    }
    harp_cbuf_free(&enc);
}

static bool closure_visit(const harp_hash *h, void *ud) {
    closure_walk(ud, h);
    return ((struct closure_ctx *)ud)->complete;
}

/* ---------------- method handlers ---------------- */

static void rsp_head(harp_cbuf *m, uint64_t rid, const char *method, bool has_body) {
    harp_env_head(m, HARP_MSG_RESPONSE, rid, method, has_body);
}

static void handle_hello(device *d, const harp_env *e) {
    uint64_t peer_major = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 0) {
                    uint64_t alen, mj, mi;
                    if (harp_cdec_array(&b, &alen) && alen == 2 &&
                        harp_cdec_uint(&b, &mj) && harp_cdec_uint(&b, &mi))
                        peer_major = mj;
                } else {
                    harp_cdec_skip(&b);
                }
            }
        }
    }
    if (peer_major != PROTO_MAJOR) {
        send_error(d, e->rid, e->method, "incompatible", "device supports protocol 1.x only");
        return;
    }
    /* §5.4: hello resets all per-session state — including a running stream */
    audio_stop(d);
    d->hello_done = true;
    d->peer_credit = 0;
    d->granted = 0;

    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, 2);
    harp_cbor_uint(&m, PROTO_MAJOR);
    harp_cbor_uint(&m, PROTO_MINOR);
    harp_cbor_uint(&m, 1);
    encode_identity(d, &m);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    grant_credit(d);
}

struct refs_collect {
    harp_cbuf items; /* concatenated encoded refs */
    size_t count;
};

static void refs_cb(const harp_ref *r, void *ud) {
    struct refs_collect *c = ud;
    harp_ref_encode(&c->items, r);
    c->count++;
}

static void handle_state_refs(device *d, const harp_env *e) {
    live_cache_flush(d);
    struct refs_collect c = {{0}, 0};
    harp_cbuf_init(&c.items);
    harp_store_ref_list(&d->store, refs_cb, &c);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, c.count);
    harp_cbuf_put(&m, c.items.buf, c.items.len);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    harp_cbuf_free(&c.items);
}

static void handle_state_snapshot(device *d, const harp_env *e) {
    char refname[HARP_REF_NAME_MAX] = "";
    char msg[256] = "";
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                const char *s;
                size_t sl;
                if (key == 0 && harp_cdec_text(&b, &s, &sl) && sl < sizeof refname) {
                    memcpy(refname, s, sl);
                    refname[sl] = 0;
                } else if (key == 1 && harp_cdec_text(&b, &s, &sl) && sl < sizeof msg) {
                    memcpy(msg, s, sl);
                    msg[sl] = 0;
                } else if (key > 1) {
                    harp_cdec_skip(&b);
                }
            }
        }
    }
    if (strcmp(refname, LIVE_REF) != 0) {
        send_error(d, e->rid, e->method, "unsupported", "refdev snapshots live/project only");
        return;
    }
    harp_hash snap;
    uint64_t gen;
    if (do_snapshot(d, msg[0] ? msg : NULL, &snap, &gen) != 0) {
        send_error(d, e->rid, e->method, "storage", NULL);
        return;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_bytes(&m, snap.b, HARP_HASH_LEN);
    harp_cbor_uint(&m, 1);
    harp_cbor_uint(&m, gen);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_state_have(device *d, const harp_env *e) {
    harp_cbuf bools;
    harp_cbuf_init(&bools);
    size_t count = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n) && n >= 1) {
            uint64_t key, alen;
            if (harp_cdec_uint(&b, &key) && key == 0 && harp_cdec_array(&b, &alen)) {
                for (uint64_t i = 0; i < alen; i++) {
                    const uint8_t *p;
                    size_t pl;
                    if (!harp_cdec_bytes(&b, &p, &pl) || pl != HARP_HASH_LEN) break;
                    harp_hash h;
                    memcpy(h.b, p, HARP_HASH_LEN);
                    harp_cbor_bool(&bools, harp_store_have(&d->store, &h));
                    count++;
                }
            }
        }
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, count);
    harp_cbuf_put(&m, bools.buf, bools.len);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    harp_cbuf_free(&bools);
}

static void handle_state_want(device *d, const harp_env *e) {
    /* collect hashes we hold, respond with queued count, then send objects */
    harp_hash queue[256];
    size_t nq = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n) && n >= 1) {
            uint64_t key, alen;
            if (harp_cdec_uint(&b, &key) && key == 0 && harp_cdec_array(&b, &alen)) {
                for (uint64_t i = 0; i < alen; i++) {
                    const uint8_t *p;
                    size_t pl;
                    if (!harp_cdec_bytes(&b, &p, &pl) || pl != HARP_HASH_LEN) break;
                    if (nq < 256) {
                        memcpy(queue[nq].b, p, HARP_HASH_LEN);
                        if (harp_store_have(&d->store, &queue[nq])) nq++;
                    }
                }
            }
        }
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, nq);
    send_ctl(d, &m);
    harp_cbuf_free(&m);

    harp_cbuf enc;
    harp_cbuf_init(&enc);
    for (size_t i = 0; i < nq; i++) {
        harp_cbuf_reset(&enc);
        if (harp_store_get(&d->store, &queue[i], &enc) != 0) continue;
        /* §4.2.1: never exceed granted credit. The simulator grants 16 MiB up
         * front and objects are small; a full implementation would queue. */
        if (enc.len <= d->peer_credit) d->peer_credit -= enc.len;
        harp_link_send(d->io, HARP_STREAM_OBJ, enc.buf, enc.len);
    }
    harp_cbuf_free(&enc);
}

static void handle_state_send(device *d, const harp_env *e) {
    uint64_t total = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 1) {
                    if (!harp_cdec_uint(&b, &total)) break;
                } else {
                    harp_cdec_skip(&b);
                }
            }
        }
    }
    struct statvfs vs;
    if (statvfs(d->store.dir, &vs) == 0) {
        uint64_t freeb = (uint64_t)vs.f_bavail * vs.f_frsize;
        if (total > freeb) { /* §11.5: refuse BEFORE transfer */
            send_error(d, e->rid, e->method, "storage", "insufficient free space");
            return;
        }
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_state_refset(device *d, const harp_env *e) {
    char refname[HARP_REF_NAME_MAX] = "";
    bool expect_null = false, have_expect = false, have_new = false;
    harp_hash expect, newh;
    uint64_t flags = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                switch (key) {
                    case 0: {
                        const char *s;
                        size_t sl;
                        if (harp_cdec_text(&b, &s, &sl) && sl < sizeof refname) {
                            memcpy(refname, s, sl);
                            refname[sl] = 0;
                        }
                        break;
                    }
                    case 1:
                        if (harp_cdec_peek_null(&b)) {
                            harp_cdec_null(&b);
                            expect_null = true;
                            have_expect = true;
                        } else {
                            const uint8_t *p;
                            size_t pl;
                            if (harp_cdec_bytes(&b, &p, &pl) && pl == HARP_HASH_LEN) {
                                memcpy(expect.b, p, HARP_HASH_LEN);
                                have_expect = true;
                            }
                        }
                        break;
                    case 2: {
                        const uint8_t *p;
                        size_t pl;
                        if (harp_cdec_bytes(&b, &p, &pl) && pl == HARP_HASH_LEN) {
                            memcpy(newh.b, p, HARP_HASH_LEN);
                            have_new = true;
                        }
                        break;
                    }
                    case 3:
                        harp_cdec_uint(&b, &flags);
                        break;
                    default:
                        harp_cdec_skip(&b);
                }
            }
        }
    }
    if (!refname[0] || !have_expect || !have_new) {
        send_error(d, e->rid, e->method, "malformed", NULL);
        return;
    }
    bool create = flags & 1, force = flags & 2;

    if (strcmp(refname, LIVE_REF) == 0) live_cache_flush(d);
    harp_ref r;
    if (harp_store_ref_read(&d->store, refname, &r) != 0) {
        send_error(d, e->rid, e->method, "malformed", "bad ref name");
        return;
    }

    /* CAS (§11.3): expect matches AND !dirty, unless force */
    bool expect_ok =
        r.unborn ? (expect_null || create)
                 : (!expect_null && harp_hash_eq(&r.hash, &expect));
    if (!force && (!expect_ok || r.dirty)) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        harp_env_head(&m, HARP_MSG_ERROR, e->rid, e->method, true);
        harp_cbor_map(&m, 3);
        harp_cbor_uint(&m, 0);
        harp_cbor_text(&m, "conflict");
        harp_cbor_uint(&m, 1);
        harp_cbor_text(&m, r.dirty ? "ref is dirty" : "expect mismatch");
        harp_cbor_uint(&m, 2);
        harp_cbor_map(&m, 3);
        harp_cbor_uint(&m, 0);
        if (r.unborn)
            harp_cbor_null(&m);
        else
            harp_cbor_bytes(&m, r.hash.b, HARP_HASH_LEN);
        harp_cbor_uint(&m, 1);
        harp_cbor_uint(&m, r.generation);
        harp_cbor_uint(&m, 2);
        harp_cbor_bool(&m, r.dirty);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
        return;
    }

    /* closure must be fully present before any effect (§11.3 atomic apply) */
    struct closure_ctx cc = {d, true, 0};
    closure_walk(&cc, &newh);
    if (!cc.complete) {
        send_error(d, e->rid, e->method, "not-found", "object closure incomplete");
        return;
    }

    /* live refs activate in the engine; others are storage-only */
    if (strcmp(refname, LIVE_REF) == 0) {
        if (engine_load_snapshot(d, &newh) != 0) {
            send_error(d, e->rid, e->method, "malformed", "target is not a loadable snapshot");
            return;
        }
    }
    r.unborn = false;
    r.hash = newh;
    r.generation++;
    r.dirty = false;
    if (harp_store_ref_write(&d->store, &r) != 0) {
        send_error(d, e->rid, e->method, "storage", NULL);
        return;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, r.generation);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
    ntf_state_changed(d, &r);
}

static void handle_diag_counters(device *d, const harp_env *e) {
    struct statvfs vs;
    uint64_t total = 0, freeb = 0;
    if (statvfs(d->store.dir, &vs) == 0) {
        total = (uint64_t)vs.f_blocks * vs.f_frsize;
        freeb = (uint64_t)vs.f_bavail * vs.f_frsize;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 14);
    harp_cbor_text(&m, "usb_errors");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "frame_errors");
    harp_cbor_uint(&m, d->frame_errors);
    harp_cbor_text(&m, "audio_underruns");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "audio_overruns");
    harp_cbor_uint(&m, d->audio_overruns);
    harp_cbor_text(&m, "audio_late_frames");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "msc_discontinuities");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "clock_drift_ppb");
    harp_cbor_int(&m, 0);
    harp_cbor_text(&m, "evt_late");
    harp_cbor_uint(&m, g_evt_late);
    harp_cbor_text(&m, "evt_stale_epoch");
    harp_cbor_uint(&m, 0);
    harp_cbor_text(&m, "x.harp-refdev.evq_drops");
    harp_cbor_uint(&m, g_evq_drops);
    harp_cbor_text(&m, "x.harp-refdev.ramp_late");
    harp_cbor_uint(&m, g_ramp_late);
    harp_cbor_text(&m, "session_resets");
    harp_cbor_uint(&m, d->session_resets);
    harp_cbor_text(&m, "storage_bytes_total");
    harp_cbor_uint(&m, total);
    harp_cbor_text(&m, "storage_bytes_free");
    harp_cbor_uint(&m, freeb);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

#ifdef __linux__
int harp_ffs_audio_in_fd(void);  /* device/ffs.c */
int harp_ffs_audio_out_fd(void); /* device/ffs.c */
#endif

/* audio.start (§8.2): free-running mode only, stereo D→H, USB transport only */
static void handle_audio_start(device *d, const harp_env *e) {
    uint64_t rate = 48000, nsamples = 256, mode = 0;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 0) {
                    if (!harp_cdec_uint(&b, &rate)) break;
                } else if (key == 1) {
                    if (!harp_cdec_uint(&b, &nsamples)) break;
                } else if (key == 5) {
                    if (!harp_cdec_uint(&b, &mode)) break;
                } else if (!harp_cdec_skip(&b))
                    break;
            }
        }
    }
    if (mode > 1) {
        send_error(d, e->rid, e->method, "unsupported", "unknown clock mode");
        return;
    }
    if (rate != 44100 && rate != 48000 && rate != 96000) {
        send_error(d, e->rid, e->method, "unsupported", "rate");
        return;
    }
    if (nsamples < 32 || nsamples > AUDIO_MAX_NSAMPLES) {
        send_error(d, e->rid, e->method, "unsupported", "nsamples");
        return;
    }
    int fd = -1, out_fd = -1;
#ifdef __linux__
    fd = harp_ffs_audio_in_fd();
    out_fd = harp_ffs_audio_out_fd();
#endif
    if (fd < 0 || (mode == 1 && out_fd < 0)) {
        send_error(d, e->rid, e->method, "unsupported",
                   "HARP stream requires the USB transport");
        return;
    }
    if (d->audio.thread_live) {
        send_error(d, e->rid, e->method, "busy", "stream already running");
        return;
    }
    evq_reset_for_new_stream();
    d->audio.fd = fd;
    d->audio.out_fd = out_fd;
    d->audio.mode = (uint32_t)mode;
    d->audio.rate = (uint32_t)rate;
    d->audio.nsamples = (uint32_t)nsamples;
    d->audio.reanchors = 0;
    d->audio.running = true;
    if (pthread_create(&d->audio.thread, NULL, audio_thread, d) != 0) {
        d->audio.running = false;
        send_error(d, e->rid, e->method, "internal", "thread");
        return;
    }
    d->audio.thread_live = true;
    fprintf(stderr, "harp-deviced: audio stream started (%u Hz, %u-sample blocks, %s)\n",
            d->audio.rate, d->audio.nsamples,
            mode ? "host-paced" : "free-running");

    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 2);
    harp_cbor_uint(&m, 0);
    harp_cbor_uint(&m, mode); /* clock-mode in effect */
    harp_cbor_uint(&m, 1);
    /* device pipeline: one block free-running; zero host-paced (pure render) */
    harp_cbor_uint(&m, mode ? 0 : d->audio.nsamples);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_audio_stop(device *d, const harp_env *e) {
    audio_stop(d);
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* evt.params (§9.3): the descriptor set */
static void handle_evt_params(device *d, const harp_env *e) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 4);
    harp_cbor_uint(&m, 0);
    harp_cbor_bytes(&m, d->param_map_hash.b, HARP_HASH_LEN);
    harp_cbor_uint(&m, 1);
    encode_param_array(&m);
    harp_cbor_uint(&m, 2);
    harp_cbor_uint(&m, 1000); /* control rate, Hz */
    harp_cbor_uint(&m, 3);
    harp_cbor_uint(&m, 4000); /* max sustained events/s */
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* evt stream (§9.2): timestamped event messages. Slice 1: etype 1 (param
 * set) applied at "now"; other types skipped. Events have no responses.
 * Host-driven sets do NOT echo (§9.4). */
static void handle_evt_msg(device *d, const uint8_t *buf, size_t len) {
    harp_cdec dec;
    harp_cdec_init(&dec, buf, len);
    uint64_t alen, tn, ep, msc, etype;
    if (!harp_cdec_array(&dec, &alen) || alen < 3 || !harp_cdec_array(&dec, &tn) ||
        tn != 2 || !harp_cdec_uint(&dec, &ep) || !harp_cdec_uint(&dec, &msc) ||
        !harp_cdec_uint(&dec, &etype)) {
        d->frame_errors++;
        return;
    }
    if (etype == 0) { /* UMP carriage (§9.10): body = one packet, words big-endian */
        const uint8_t *p;
        size_t pl;
        if (!harp_cdec_bytes(&dec, &p, &pl) || pl < 4) return;
        uint32_t w = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
                     p[3];
        uint32_t mt = w >> 28;
        if (mt == 2) { /* MIDI 1.0 channel voice in UMP */
            uint32_t status = (w >> 20) & 0xf, note = (w >> 8) & 0x7f, vel = w & 0x7f;
            dev_event ev = {msc, 0, note, 0, 0};
            if (status == 0x9 && vel > 0) {
                ev.kind = DEV_EV_NOTE_ON;
                ev.v = (float)vel / 127.0f;
            } else if (status == 0x8 || (status == 0x9 && vel == 0)) {
                ev.kind = DEV_EV_NOTE_OFF;
            } else if (status == 0xB && (note == 120 || note == 123)) {
                /* CC 120 all-sound-off / 123 all-notes-off: PANIC. Applied
                 * immediately AND queued — belt and suspenders. */
                ev.kind = DEV_EV_ALL_OFF;
                g_note = -1;
            } else
                return;
            /* note-offs must never be lost: if the queue is full, apply NOW
             * (slightly early beats stuck forever) */
            if (ev.kind != DEV_EV_NOTE_ON) {
                pthread_mutex_lock(&g_evq_mu);
                bool full = g_evq_n >= DEV_EVQ_CAP;
                pthread_mutex_unlock(&g_evq_mu);
                if (full) {
                    if (ev.kind == DEV_EV_ALL_OFF || g_note == (int)note) g_note = -1;
                    return;
                }
            }
            evq_push(ev); /* ts 0 = asap; else applied at the exact sample (§9.2) */
        }
        return;
    }
    if (etype == 5) { /* ramp (§9.4): {0 param, 1 target, 2 end tstamp} */
        uint64_t n, key, id = 0, eep = 0, ets = 0;
        double target = 0;
        bool have_id = false, have_t = false, have_end = false;
        if (!harp_cdec_map(&dec, &n)) {
            d->frame_errors++;
            return;
        }
        for (uint64_t i = 0; i < n; i++) {
            if (!harp_cdec_uint(&dec, &key)) return;
            if (key == 0) {
                if (!harp_cdec_uint(&dec, &id)) return;
                have_id = true;
            } else if (key == 1) {
                if (!harp_cdec_float(&dec, &target)) return;
                have_t = true;
            } else if (key == 2) {
                uint64_t tn;
                if (!harp_cdec_array(&dec, &tn) || tn != 2 || !harp_cdec_uint(&dec, &eep) ||
                    !harp_cdec_uint(&dec, &ets))
                    return;
                have_end = true;
            } else if (!harp_cdec_skip(&dec))
                return;
        }
        if (!have_id || !have_t || !have_end) return;
        if (target < 0) target = 0;
        if (target > 1) target = 1;
        dev_event ev = {msc, DEV_EV_RAMP, (uint32_t)id, (float)target, ets};
        evq_push(ev);
        live_ref_touch(d, true);
        return;
    }
    if (etype != 1) return; /* unknown event types are skipped, not fatal */
    uint64_t n, key, id = 0;
    double v = 0;
    bool have_id = false, have_v = false;
    if (!harp_cdec_map(&dec, &n)) {
        d->frame_errors++;
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        if (!harp_cdec_uint(&dec, &key)) return;
        if (key == 0) {
            if (!harp_cdec_uint(&dec, &id)) return;
            have_id = true;
        } else if (key == 1) {
            if (!harp_cdec_float(&dec, &v)) return;
            have_v = true;
        } else if (!harp_cdec_skip(&dec))
            return;
    }
    if (!have_id || !have_v) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    if (msc == 0) { /* "now": apply directly */
        for (size_t i = 0; i < NPARAMS; i++)
            if (g_params[i].id == id) {
                g_params[i].value = (float)v;
                g_ramps[i].active = false;
            }
    } else {
        dev_event ev = {msc, DEV_EV_PARAM_SET, (uint32_t)id, (float)v, 0};
        evq_push(ev);
    }
    live_ref_touch(d, true);
}

/* x.harp-refdev.knob {0: param-id, 1: value} — front-panel simulation */
static void handle_knob(device *d, const harp_env *e) {
    uint64_t id = 0;
    double v = 0;
    bool ok = false;
    if (e->has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e->body, e->body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n) && n >= 2) {
            uint64_t key;
            if (harp_cdec_uint(&b, &key) && key == 0 && harp_cdec_uint(&b, &id) &&
                harp_cdec_uint(&b, &key) && key == 1 && harp_cdec_float(&b, &v))
                ok = true;
        }
    }
    if (!ok) {
        send_error(d, e->rid, e->method, "malformed", NULL);
        return;
    }
    if (!front_panel_set(d, (uint32_t)id, v)) {
        send_error(d, e->rid, e->method, "not-found", "no such param");
        return;
    }
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, false);
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

static void handle_dev_params(device *d, const harp_env *e) {
    harp_cbuf m;
    harp_cbuf_init(&m);
    rsp_head(&m, e->rid, e->method, true);
    harp_cbor_map(&m, 1);
    harp_cbor_uint(&m, 0);
    harp_cbor_array(&m, NPARAMS);
    for (size_t i = 0; i < NPARAMS; i++) {
        harp_cbor_array(&m, 3);
        harp_cbor_uint(&m, g_params[i].id);
        harp_cbor_text(&m, g_params[i].name);
        harp_cbor_float(&m, g_params[i].value);
    }
    send_ctl(d, &m);
    harp_cbuf_free(&m);
}

/* ---------------- dispatch ---------------- */

static void handle_ctl(device *d, const uint8_t *buf, size_t len) {
    /* dirty-flag work the render thread deferred (queued sets/ramps applied) */
    if (g_touch_pending) {
        g_touch_pending = 0;
        live_ref_touch(d, true);
    }
    harp_env e;
    if (!harp_env_parse(buf, len, &e)) {
        d->frame_errors++;
        return;
    }
    if (e.msgtype == HARP_MSG_NOTIFICATION) {
        if (strcmp(e.method, "core.credit") == 0 && e.has_body) {
            harp_cdec b;
            harp_cdec_init(&b, e.body, e.body_len);
            uint64_t n, key, v;
            if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
                harp_cdec_uint(&b, &v))
                d->peer_credit += v;
        }
        return; /* unknown notifications are ignored (§5.2) */
    }
    if (e.msgtype != HARP_MSG_REQUEST) return;

    if (!d->hello_done && strcmp(e.method, "core.hello") != 0) {
        send_error(d, e.rid, e.method, "denied", "core.hello required first");
        return;
    }
    if (strcmp(e.method, "core.hello") == 0)
        handle_hello(d, &e);
    else if (strcmp(e.method, "core.identify") == 0) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, true);
        encode_identity(d, &m);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
    } else if (strcmp(e.method, "core.ping") == 0) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, e.has_body);
        if (e.has_body) harp_cbuf_put(&m, e.body, e.body_len);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
    } else if (strcmp(e.method, "core.bye") == 0) {
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, false);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
        d->closing = true;
    } else if (strcmp(e.method, "state.refs") == 0)
        handle_state_refs(d, &e);
    else if (strcmp(e.method, "state.snapshot") == 0)
        handle_state_snapshot(d, &e);
    else if (strcmp(e.method, "state.have") == 0)
        handle_state_have(d, &e);
    else if (strcmp(e.method, "state.want") == 0)
        handle_state_want(d, &e);
    else if (strcmp(e.method, "state.send") == 0)
        handle_state_send(d, &e);
    else if (strcmp(e.method, "state.refset") == 0)
        handle_state_refset(d, &e);
    else if (strcmp(e.method, "evt.params") == 0)
        handle_evt_params(d, &e);
    else if (strcmp(e.method, "audio.start") == 0)
        handle_audio_start(d, &e);
    else if (strcmp(e.method, "audio.stop") == 0)
        handle_audio_stop(d, &e);
    else if (strcmp(e.method, "diag.counters") == 0)
        handle_diag_counters(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.knob") == 0)
        handle_knob(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.params") == 0)
        handle_dev_params(d, &e);
    else if (strcmp(e.method, "x.harp-refdev.restart") == 0) {
        /* dev-loop helper: exit cleanly so systemd (Restart=always) respawns
         * the daemon from the (possibly updated) binary on disk — the
         * fw.commit pattern in miniature, no root needed for deploys */
        harp_cbuf m;
        harp_cbuf_init(&m);
        rsp_head(&m, e.rid, e.method, false);
        send_ctl(d, &m);
        harp_cbuf_free(&m);
        fprintf(stderr, "harp-deviced: restart requested; exiting for respawn\n");
        audio_stop(d);
        exit(0);
    } else
        send_error(d, e.rid, e.method, "unsupported", NULL);
}

static void handle_obj(device *d, const uint8_t *buf, size_t len) {
    /* one object per obj-stream message; verify-on-receipt is intrinsic */
    if (harp_store_put(&d->store, buf, len, NULL) != 0) d->frame_errors++;
    if (d->granted >= len)
        d->granted -= len;
    else
        d->granted = 0;
    if (d->granted < CREDIT_GRANT / 2) grant_credit(d);
}

/* ---------------- panel API (device frontend boundary) ----------------
 *
 * Line-oriented JSON over a Unix socket: the single sanctioned boundary
 * for every frontend — web sidecar, future GPIO encoders, MIDI control
 * surfaces. Frontends reach the engine ONLY through front_panel_set
 * (the device-side mirror of the spec's no-SysEx-side-doors rule):
 * edits here dirty the live ref and echo as §9.4 events, exactly like
 * hands on hardware.
 *
 *   request line             response line (JSON)
 *   params                   {"product","serial","dirty","params":[..]}
 *   knob <id> <value01>      {"ok":true} | {"ok":false,...}
 *   refs                     {"refs":[{"name","hash","gen","dirty"},..]}
 *   counters                 {"frame_errors",...}
 *
 * The web server lives in a sidecar (web/harp-panel.py) — HTTP robustness
 * is a solved problem there; this stays dependency-free C.
 */

static void panel_json_params(device *d, char *body, size_t sz) {
    size_t off = 0;
    pthread_mutex_lock(&d->state_mu);
    bool dirty = d->live_cache_valid ? d->live_cache.dirty : false;
    if (!d->live_cache_valid) {
        harp_ref r;
        if (harp_store_ref_read(&d->store, LIVE_REF, &r) == 0) dirty = r.dirty;
    }
    pthread_mutex_unlock(&d->state_mu);
    off += (size_t)snprintf(body + off, sz - off,
                            "{\"product\":\"harp-refdev\",\"serial\":\"%s\","
                            "\"dirty\":%s,\"params\":[",
                            d->serial, dirty ? "true" : "false");
    for (size_t i = 0; i < NPARAMS; i++)
        off += (size_t)snprintf(body + off, sz - off,
                                "%s{\"id\":%u,\"name\":\"%s\",\"value\":%.4f}",
                                i ? "," : "", g_params[i].id, g_params[i].name,
                                g_params[i].value);
    snprintf(body + off, sz - off, "]}");
}

struct refs_json {
    device *d;
    char *body;
    size_t sz, off;
    int n;
};

static void panel_refs_cb(const harp_ref *r, void *ud) {
    struct refs_json *j = ud;
    harp_ref shown = *r;
    /* the live ref may be ahead on the coalesced in-memory cache */
    pthread_mutex_lock(&j->d->state_mu);
    if (j->d->live_cache_valid && strcmp(r->name, LIVE_REF) == 0)
        shown = j->d->live_cache;
    pthread_mutex_unlock(&j->d->state_mu);
    char hex[2 * HARP_HASH_LEN + 1] = "";
    if (!shown.unborn) {
        harp_hash_hex(&shown.hash, hex);
        hex[12] = 0;
    }
    j->off += (size_t)snprintf(j->body + j->off, j->sz - j->off,
                               "%s{\"name\":\"%s\",\"hash\":\"%s\",\"gen\":%llu,"
                               "\"dirty\":%s}",
                               j->n++ ? "," : "", shown.name, hex,
                               (unsigned long long)shown.generation,
                               shown.dirty ? "true" : "false");
}

static void panel_json_refs(device *d, char *body, size_t sz) {
    struct refs_json j = {d, body, sz, 0, 0};
    j.off = (size_t)snprintf(body, sz, "{\"refs\":[");
    harp_store_ref_list(&d->store, panel_refs_cb, &j);
    snprintf(body + j.off, sz - j.off, "]}");
}

static void panel_json_counters(device *d, char *body, size_t sz) {
    snprintf(body, sz,
             "{\"frame_errors\":%llu,\"session_resets\":%llu,"
             "\"audio_overruns\":%llu,\"snapshots\":%llu,\"evq_drops\":%llu,"
             "\"evt_late\":%llu,\"ramp_late\":%llu,\"boot\":%llu,"
             "\"session\":%s,\"streaming\":%s}",
             (unsigned long long)d->frame_errors, (unsigned long long)d->session_resets,
             (unsigned long long)d->audio_overruns, (unsigned long long)d->snapshots_taken,
             (unsigned long long)g_evq_drops, (unsigned long long)g_evt_late,
             (unsigned long long)g_ramp_late, (unsigned long long)d->boot_count,
             d->hello_done ? "true" : "false", d->audio.thread_live ? "true" : "false");
}

/* Front-panel patch load: point live/project at a stored ref's state (the
 * §11.4 asymmetry applies to front panels too — snapshot-if-dirty first, so
 * reverting never loses anything). Echoes all params so an attached DAW's
 * knobs follow. */
static bool panel_revert(device *d, const char *refname) {
    harp_ref src;
    if (harp_store_ref_read(&d->store, refname, &src) != 0 || src.unborn) return false;
    live_cache_flush(d);
    harp_ref live;
    if (harp_store_ref_read(&d->store, LIVE_REF, &live) != 0) return false;
    if (live.dirty) {
        harp_hash snap;
        if (do_snapshot(d, "pre-revert", &snap, NULL) != 0) return false;
    }
    if (engine_load_snapshot(d, &src.hash) != 0) return false;
    live_cache_flush(d);
    harp_store_ref_read(&d->store, LIVE_REF, &live);
    live.unborn = false;
    live.hash = src.hash;
    live.generation++;
    live.dirty = false;
    if (harp_store_ref_write(&d->store, &live) != 0) return false;
    ntf_state_changed(d, &live);
    for (size_t i = 0; i < NPARAMS; i++)
        evt_echo_param(d, g_params[i].id, g_params[i].value);
    return true;
}

/* One panel-API connection at a time (the sidecar holds one persistent
 * connection); line in, JSON line out. */
static void panel_serve_conn(device *d, int fd) {
    char buf[512];
    size_t len = 0;
    char body[4096];
    for (;;) {
        ssize_t r = read(fd, buf + len, sizeof buf - 1 - len);
        if (r <= 0) return;
        len += (size_t)r;
        buf[len] = 0;
        char *nl;
        while ((nl = memchr(buf, '\n', len))) {
            *nl = 0;
            if (strcmp(buf, "params") == 0)
                panel_json_params(d, body, sizeof body);
            else if (strcmp(buf, "refs") == 0)
                panel_json_refs(d, body, sizeof body);
            else if (strcmp(buf, "counters") == 0)
                panel_json_counters(d, body, sizeof body);
            else if (strcmp(buf, "snapshot") == 0) {
                /* the front-panel save button (§10.4 snapshot-on-demand) */
                harp_hash snap;
                uint64_t gen;
                if (do_snapshot(d, "front panel", &snap, &gen) == 0) {
                    char hex[2 * HARP_HASH_LEN + 1];
                    harp_hash_hex(&snap, hex);
                    hex[12] = 0;
                    snprintf(body, sizeof body, "{\"ok\":true,\"hash\":\"%s\",\"gen\":%llu}",
                             hex, (unsigned long long)gen);
                } else
                    snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"storage\"}");
            } else if (strcmp(buf, "panic") == 0) {
                g_note = -1; /* same engine path the CC 120/123 handler takes */
                snprintf(body, sizeof body, "{\"ok\":true}");
            } else if (strncmp(buf, "revert ", 7) == 0) {
                if (panel_revert(d, buf + 7))
                    snprintf(body, sizeof body, "{\"ok\":true}");
                else
                    snprintf(body, sizeof body,
                             "{\"ok\":false,\"error\":\"unknown ref or load failed\"}");
            } else if (strncmp(buf, "knob ", 5) == 0) {
                unsigned id = 0;
                double v = -1;
                if (sscanf(buf + 5, "%u %lf", &id, &v) == 2 && v >= 0 && v <= 1 &&
                    front_panel_set(d, id, v))
                    snprintf(body, sizeof body, "{\"ok\":true}");
                else
                    snprintf(body, sizeof body,
                             "{\"ok\":false,\"error\":\"knob <id 1..8> <value 0..1>\"}");
            } else
                snprintf(body, sizeof body, "{\"error\":\"unknown command\"}");
            size_t blen = strlen(body);
            body[blen] = '\n';
            if (!harp_write_all(fd, body, blen + 1)) return;
            size_t consumed = (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, len - consumed);
            len -= consumed;
        }
        if (len >= sizeof buf - 1) return; /* oversized request line */
    }
}

struct panel_args {
    device *d;
    const char *path;
};

static void *panel_main(void *arg) {
    struct panel_args *a = arg;
    unlink(a->path);
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", a->path);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(sfd, 4) != 0) {
        fprintf(stderr, "harp-deviced: panel api: cannot listen on %s\n", a->path);
        return NULL;
    }
    chmod(a->path, 0666); /* sidecar runs unprivileged */
    fprintf(stderr, "harp-deviced: panel api on %s\n", a->path);
    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        panel_serve_conn(a->d, cfd);
        close(cfd);
    }
    return NULL;
}

/* ---------------- session / main ---------------- */

void harp_deviced_run_session(device *d, harp_io *io) {
    d->io = io;
    d->hello_done = false;
    d->closing = false;
    harp_link_init(&d->link);
    harp_cbuf msg;
    harp_cbuf_init(&msg);
    for (;;) {
        uint8_t stream;
        int rc = harp_link_recv(io, &d->link, &stream, &msg);
        if (rc == -1) break; /* peer gone */
        if (rc == -2) {      /* protocol violation: session reset (§12.4) */
            d->session_resets++;
            break;
        }
        if (stream == HARP_STREAM_CTL)
            handle_ctl(d, msg.buf, msg.len);
        else if (stream == HARP_STREAM_OBJ)
            handle_obj(d, msg.buf, msg.len);
        else if (stream == HARP_STREAM_EVT)
            handle_evt_msg(d, msg.buf, msg.len);
        if (d->closing) break;
    }
    harp_cbuf_free(&msg);
    harp_link_free(&d->link);
    audio_stop(d); /* session gone -> stream gone (§12) */
    live_cache_flush(d); /* persist the terminal generation (§10.3) */
    d->io = NULL;
}

#ifdef __linux__
/* FunctionFS gadget transport (device/ffs.c) */
int harp_ffs_serve(const char *ffs_dir, const char *gadget_path,
                   void (*session)(void *ud, harp_io *io), void *ud);

static void ffs_session_cb(void *ud, harp_io *io) {
    harp_deviced_run_session(ud, io);
    fprintf(stderr, "harp-deviced: usb session ended; awaiting reattach\n");
}
#endif

static uint64_t bump_boot_count(const char *dir) {
    char path[600];
    snprintf(path, sizeof path, "%s/bootcount", dir);
    uint64_t n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%llu", (unsigned long long *)&n) != 1) n = 0;
        fclose(f);
    }
    n++;
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "%llu\n", (unsigned long long)n);
        fclose(f);
    }
    return n;
}

int main(int argc, char **argv) {
    const char *state_dir = "./refdev-state";
    const char *serial = "SIM-0001";
    const char *ffs_dir = NULL;
    const char *gadget = "/sys/kernel/config/usb_gadget/harp";
    int port = 47800;
    const char *panel_sock = "/tmp/harp-panel.sock"; /* "" disables */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc)
            state_dir = argv[++i];
        else if (strcmp(argv[i], "--serial") == 0 && i + 1 < argc)
            serial = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ffs") == 0 && i + 1 < argc)
            ffs_dir = argv[++i];
        else if (strcmp(argv[i], "--gadget") == 0 && i + 1 < argc)
            gadget = argv[++i];
        else if (strcmp(argv[i], "--panel-sock") == 0 && i + 1 < argc)
            panel_sock = argv[++i];
        else {
            fprintf(stderr,
                    "usage: harp-deviced [--state-dir DIR] [--serial S] "
                    "[--panel-sock PATH] "
                    "[--port P | --ffs FFS_DIR [--gadget CONFIGFS_PATH]]\n");
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    device *d = &g_dev;
    memset(d, 0, sizeof *d);
    d->io = NULL;
    snprintf(d->serial, sizeof d->serial, "%s", serial);
    if (harp_store_open(&d->store, state_dir) != 0) {
        fprintf(stderr, "harp-deviced: cannot open state dir %s\n", state_dir);
        return 1;
    }
    d->boot_count = bump_boot_count(state_dir);
    compute_param_map_hash(d);
    pthread_mutex_init(&d->send_mu, NULL);
    pthread_mutex_init(&d->state_mu, NULL);

    if (panel_sock[0]) {
        static struct panel_args pa;
        pa.d = d;
        pa.path = panel_sock;
        pthread_t pt;
        pthread_create(&pt, NULL, panel_main, &pa);
        pthread_detach(pt);
    }

    /* Recall across power cycles: load the live ref if clean; first boot
     * snapshots the factory state so the ref is born. */
    harp_ref live;
    if (harp_store_ref_read(&d->store, LIVE_REF, &live) == 0) {
        if (live.unborn) {
            harp_hash snap;
            if (do_snapshot(d, "factory state", &snap, NULL) == 0)
                fprintf(stderr, "harp-deviced: initialized factory state\n");
        } else if (!live.dirty) {
            if (engine_load_snapshot(d, &live.hash) == 0)
                fprintf(stderr, "harp-deviced: restored live/project (gen %llu)\n",
                        (unsigned long long)live.generation);
        }
    }

    if (ffs_dir) {
#ifdef __linux__
        fprintf(stderr, "harp-deviced: serial %s, state %s, USB gadget via %s (boot %llu)\n",
                d->serial, state_dir, ffs_dir, (unsigned long long)d->boot_count);
        return harp_ffs_serve(ffs_dir, gadget, ffs_session_cb, d);
#else
        (void)gadget;
        fprintf(stderr, "harp-deviced: --ffs requires Linux\n");
        return 2;
#endif
    }

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(sfd, 4) != 0) {
        fprintf(stderr, "harp-deviced: cannot listen on port %d: %s\n", port,
                strerror(errno));
        return 1;
    }
    fprintf(stderr, "harp-deviced: serial %s, state %s, listening on %d (boot %llu)\n",
            d->serial, state_dir, port, (unsigned long long)d->boot_count);

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        harp_io_fd tio;
        harp_io_fd_init(&tio, cfd, cfd);
        harp_deviced_run_session(d, &tio.io);
        close(cfd);
        fprintf(stderr, "harp-deviced: session ended; awaiting reattach\n");
    }
    return 0;
}
