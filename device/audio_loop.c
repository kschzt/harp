/* audio_loop.c — the device's audio render+emit loop (§8.7 free-running RTP and
 * §8.3 host-paced DETERMINISTIC mode), extracted from engine.c into the harpdevice
 * library so every device daemon (the refdev and downstream synths) reuses ONE
 * tested loop and supplies only the render seam: render_output() + engine_voices_cold()
 * (device.h). Moved verbatim — behavior is byte-identical (the golden / §8.7
 * bit-exact tests are the gate). */
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <sys/socket.h>
#endif
#include "device.h"
#include "fence_wait.h" /* §8.3.1 fence-wait + count predicates (pure, unit-tested) */
#include "log_ring.h" /* §4.2 stream `log` ring — route the audio loop's log lines (§14.4) */

/* §8.3.1 real-time host-paced fence bound: "a few milliseconds". The fence wait is normally
 * µs (events keep up); this is the safety cap so a host that fences beyond what it feeds cannot
 * wedge the real-time stream — at the bound we render the range with the late event applied and
 * count the expiry. The deterministic OFFLINE bounce ignores it (unbounded barrier). */
#define HARP_FENCE_RT_BOUND_NS 5000000ull /* 5 ms */

/* monotonic now in ns for the real-time fence deadline (CLOCK_MONOTONIC, as elsewhere here) */
static inline uint64_t fence_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* sockets are owned by the daemon main (harp-deviced.c) */
void audio_rtp_emit(audio_state *a, const float *samples, size_t payload_bytes, uint64_t msc);
void audio_rtp_close(audio_state *a);
int  audio_open_tcp_paced(uint32_t peer_ip_net, int port);

/* Host-paced pacing-channel I/O. On Windows that channel is a Winsock SOCKET, which
 * rejects read()/write() — route through recv()/send(). On POSIX read()/harp_write_all
 * work on both the TCP socket and the FunctionFS endpoint fd, so they stay verbatim. */
#ifdef _WIN32
/* The pacing recv carries a SO_RCVTIMEO (set at connect-back) so it returns periodically
 * even with no data: a stop (audio.running=false) then unwinds the loop within one tick.
 * This is the ONLY reliable wakeup on Windows — a blocking Winsock recv pending in the
 * audio thread is NOT interruptible from audio_stop's thread by pthread_cancel OR by
 * shutdown()/closesocket() (they leave the recv parked until the peer closes), so the
 * thread must poll its own running flag. While streaming, a timeout just means "no pacing
 * yet" — retry; the host paces continuously so this never spuriously ends a live stream. */
static ssize_t hp_read(audio_state *a, void *buf, size_t n) {
    for (;;) {
        int r = recv((SOCKET)a->out_fd, (char *)buf, (int)(n > 0x7fffffff ? 0x7fffffff : n), 0);
        if (r >= 0) return r;
        if (WSAGetLastError() == WSAETIMEDOUT &&
            atomic_load_explicit(&a->running, memory_order_relaxed))
            continue; /* poll tick, still streaming */
        return -1; /* real error, or a stop (running=false) — unwind host_paced_loop */
    }
}
static bool hp_write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n) {
        int w = send((SOCKET)fd, p + off, (int)(n - off > 0x7fffffff ? 0x7fffffff : n - off), 0);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}
#else
#define hp_read(a, buf, n)       read((a)->out_fd, (buf), (n))
#define hp_write_all(fd, buf, n) harp_write_all((fd), (buf), (n))
#endif

static void host_paced_loop(device *d) {
    audio_state *a = &d->audio;
    /* voice starts from zero at audio.start (T15); notes/arp are reset by
     * evq_reset_for_new_stream, so zero ONLY the per-part voices here */
    engine_voices_cold();
    /* frame + sample buffers sized for the full 34-channel map (§P2.2); the
     * default stereo main mix uses only the first 2 channels */
    uint8_t frame[HARP_AUDIO_HDR_LEN + AUDIO_MAX_NSAMPLES * 34 * 4];
    float samples[AUDIO_MAX_NSAMPLES * 34];
    /* §14.3 digital loopback: the single H->D in-slot column extracted from the
     * pacing-frame input payload, copied onto the rendered out-slot column below.
     * Only the one looped channel is kept (not the whole multi-slot payload), so
     * the audio thread's stack stays small. Untouched unless loopback_on. */
    float lpb_col[AUDIO_MAX_NSAMPLES];
    /* §8.8 audio.fx: planar input columns for an effect engine, allocated once per stream
     * (n_in_slots × AUDIO_MAX_NSAMPLES). NULL for a synth (engine_is_fx()==0) → never demuxed. */
    if (engine_is_fx() && a->n_in_slots > 0 && !a->fx_in)
        a->fx_in = calloc((size_t)a->n_in_slots * AUDIO_MAX_NSAMPLES, sizeof(float));
    /* buffered endpoint reads (packet-multiple, see ffs.c) */
    uint8_t rbuf[16384];
    size_t rlen = 0, rpos = 0;
    uint64_t expect_ssi = 0; /* §8.2: host-paced render cursor — the next in-order SSI */

    while (atomic_load_explicit(&a->running, memory_order_relaxed)) {
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
            ssize_t r = hp_read(a, rbuf, sizeof rbuf);
            if (r <= 0) { /* endpoint died, or a stop unwound the recv (running=false) */
#ifdef _WIN32
                harp_devlog(HARP_LOG_INFO, "audio", "harp-deviced: pacing read ended (%s)\n", r == 0 ? "EOF" : "recv error");
#else
                harp_devlog(HARP_LOG_INFO, "audio", "harp-deviced: pacing read ended: %s\n", r == 0 ? "EOF" : strerror(errno));
#endif
                return;
            }
            rlen = (size_t)r;
            rpos = 0;
        }
        harp_audio_hdr h;
        if (!harp_audio_hdr_decode(hdr, &h) || !(h.dirflags & HARP_AUDIO_DIR_H2D)) {
            CTR_INC(d->frame_errors);
            harp_devlog(HARP_LOG_ERROR, "audio", "harp-deviced: malformed pacing frame (%02x %02x ...)\n",
                    hdr[0], hdr[1]);
            return; /* §4.2 spirit: malformed stream is fatal */
        }
        /* event fence (§8.3.1): events ride the link endpoint, pacing rides this
         * one — two pipes, no ordering between them. A fenced frame names how many
         * evt messages must be consumed before its range may render; ordering
         * becomes structural instead of probabilistic. Two regimes, decided by
         * a->offline (the pure predicates are in fence_wait.h, unit-tested):
         *   - OFFLINE deterministic bounce (§8.3-over-§8.7 TCP pull): faster than
         *     real time, no stream to wedge, and it MUST reproduce the EXACT fenced
         *     event set bit-for-bit — so the fence is an UNBOUNDED barrier (a timed-
         *     out fence would render before the event landed, breaking determinism).
         *   - REAL-TIME host-paced (USB; a running wall clock): an unbounded fence
         *     IS a wedged stream, so §8.3.1 MANDATES a few-ms bound — at the deadline
         *     we render the range with the late event applied and count the expiry
         *     (g_fence_timeouts; evt_late is the probe for the late apply). Events
         *     normally keep up over USB, so the bound almost never fires. Session
         *     teardown (a->running false) ends either wait. */
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
                ssize_t r = hp_read(a, rbuf, sizeof rbuf);
                if (r <= 0) return;
                rlen = (size_t)r;
                rpos = 0;
            }
            uint32_t want = (uint32_t)fb[0] | ((uint32_t)fb[1] << 8) |
                            ((uint32_t)fb[2] << 16) | ((uint32_t)fb[3] << 24);
            int32_t pending = (int32_t)(want - atomic_load_explicit(
                                            &g_evt_consumed, memory_order_acquire));
            if (pending > 0) {
                CTR_INC(g_fence_waits);
                struct timespec fts = {0, 50000}; /* 50 µs poll */
                uint64_t deadline = fence_mono_ns() + HARP_FENCE_RT_BOUND_NS;
                /* offline: unbounded barrier (deterministic bounce); real-time: bounded at the
                 * deadline, then fall through to render the range with the late event (§8.3.1). */
                while (harp_fence_keep_waiting(
                           pending,
                           atomic_load_explicit(&a->running, memory_order_relaxed),
                           a->offline, fence_mono_ns(), deadline)) {
                    nanosleep(&fts, NULL);
                    pending = (int32_t)(want - atomic_load_explicit(
                                            &g_evt_consumed, memory_order_acquire));
                }
                if (harp_fence_count_timeout(
                        pending,
                        atomic_load_explicit(&a->running, memory_order_relaxed),
                        a->offline))
                    CTR_INC(g_fence_timeouts); /* real-time deadline expiry — rendered late */
            }
        }
        /* Input payload: normally discarded (this engine has no input channels), so the
         * golden/offline-bounce path is byte-identical. §14.3: when the digital loopback
         * is armed, instead pick out the looped in-slot's column (payload is slot-
         * interleaved floats, columns in a->in_slots order) into lpb_col, to be copied
         * onto the rendered out-slot column after render_output. */
        bool lpb = atomic_load_explicit(&a->loopback_on, memory_order_acquire);
        int in_col = -1;
        if (lpb)
            for (uint8_t c = 0; c < a->n_in_slots; c++)
                if (a->in_slots[c] == a->loopback_in_slot) { in_col = c; break; }
        bool keep = lpb && in_col >= 0 && h.slots > 0 && (size_t)in_col < h.slots &&
                    h.nsamples <= AUDIO_MAX_NSAMPLES;
        /* §8.8 audio.fx: in addition to the single-column loopback, demux ALL host input
         * columns into a->fx_in (planar) for an EFFECT engine. A synth has engine_is_fx()==0
         * → fxmode false → this is inert and the discard/golden path is byte-identical. */
        bool fxmode = engine_is_fx() && a->fx_in && a->n_in_slots > 0 && h.slots > 0 &&
                      h.nsamples <= AUDIO_MAX_NSAMPLES;
        size_t fxcols = fxmode ? (a->n_in_slots <= h.slots ? a->n_in_slots : h.slots) : 0;
        size_t stride = (size_t)h.slots * 4; /* bytes per interleaved sample-row */
        size_t base = (size_t)in_col * 4;    /* first byte of in_col within a row  */
        size_t pcur = 0;                     /* absolute byte offset within payload */
        size_t skip = harp_audio_payload_len(&h);
        while (skip) {
            if (rpos < rlen) {
                size_t take = rlen - rpos;
                if (take > skip) take = skip;
                if (keep || fxmode) {
                    for (size_t i = 0; i < take; i++) {
                        size_t off = pcur + i, inrow = off % stride, smp = off / stride;
                        if (keep && inrow >= base && inrow < base + 4)         /* §14.3 loopback col */
                            ((uint8_t *)lpb_col)[smp * 4 + (inrow - base)] = rbuf[rpos + i];
                        if (fxmode) {                                          /* §8.8 all FX in-cols */
                            size_t col = inrow >> 2;
                            if (col < fxcols)
                                ((uint8_t *)(a->fx_in + col * AUDIO_MAX_NSAMPLES))
                                    [smp * 4 + (inrow & 3)] = rbuf[rpos + i];
                        }
                    }
                }
                pcur += take;
                rpos += take;
                skip -= take;
                continue;
            }
            ssize_t r = hp_read(a, rbuf, sizeof rbuf);
            if (r <= 0) return;
            rlen = (size_t)r;
            rpos = 0;
        }
        a->fx_in_n = (uint16_t)(fxmode ? h.nsamples : 0); /* §8.8: samples for the FX render */
        uint32_t n = h.nsamples;
        if (n > AUDIO_MAX_NSAMPLES) {
            CTR_INC(d->frame_errors);
            return;
        }
        /* §8.2: a pacing frame whose ts is behind the render cursor is late — the host
         * paces in order, so a rewound ts is a host bug. The whole frame is consumed above,
         * so discarding it keeps the stream in sync; count it and never re-render a range.
         * (h.ts == expect_ssi on every in-order frame, so the golden never discards.) */
        if (h.ts < expect_ssi) {
            CTR_INC(d->audio_late_frames);
            continue;
        }
        /* §P2.2: render the requested output slots; `slots` carries the count.
         * Default {0,1} is the unchanged 2-channel main mix (golden holds). */
        uint16_t slots = render_output(a, samples, n, (float)a->rate, h.ts);
        /* §14.3 digital loopback: overwrite the rendered out-slot column with the kept
         * in-slot column — a same-frame copy, so device-internal loop latency is 0 (the
         * start-rsp reports key5=0). `keep` implies the host declared both slots (in via
         * audio.start key3, out via key4); if the out-slot was not rendered we no-op and
         * the host's round-trip measurement fails loudly. Gated on loopback_on, so off
         * the golden path this is never entered and output stays byte-identical. */
        if (keep) {
            int out_col = -1;
            for (uint16_t c = 0; c < slots; c++)
                if (a->out_slots[c] == a->loopback_out_slot) { out_col = c; break; }
            if (out_col >= 0)
                for (uint32_t s = 0; s < n; s++)
                    samples[(size_t)s * slots + out_col] = lpb_col[s];
        }
        harp_audio_hdr out = {HARP_AUDIO_FVER, 0, slots, h.epoch, h.ts, (uint16_t)n,
                              HARP_AUDIO_FMT_F32};
        harp_audio_hdr_encode(&out, frame);
        size_t payload = (size_t)n * slots * 4;
        memcpy(frame + HARP_AUDIO_HDR_LEN, samples, payload);
        if (!hp_write_all(a->fd, frame, HARP_AUDIO_HDR_LEN + payload)) return;
        expect_ssi = h.ts + n; /* §8.2: advance the cursor past the rendered range */
    }
}

void *audio_thread(void *arg) {
    device *d = arg;
    audio_state *a = &d->audio;
    /* §8.3.1: this is the real-time audio path — host-paced pacing/fence (mode 1) and
     * free-running RTP (mode 0). Promote it to SCHED_FIFO so it is never descheduled by
     * ordinary load: in host-paced mode it re-checks g_evt_consumed against the fenced
     * frame every 50 µs and must see an in-order event land before the few-ms bound (the
     * session thread promotes itself too); in free-running mode it must hit its emit
     * cadence. Once per stream; graceful degrade + log-once if RT can't be granted. */
    harp_device_thread_set_realtime("audio");
    /* §8.3-over-§8.7: host-paced deterministic render over TCP (offline bounce).
     * Connect back to the host's audio-listen port (key 7) HERE, on the audio
     * thread — NOT in handle_audio_start, which advances the §8.3.1 event fence on
     * the session thread (a blocking connect there deadlocks: the host accepts only
     * at its post-audioStart drain, which waits for the audio.start response, which
     * waits for handle_audio_start to return). The steady-state pacing recv is torn
     * down by SO_RCVTIMEO + the a->running poll (see hp_read / audio_stop), not by
     * pthread_cancel — a blocking Winsock recv is not cancellable from another thread. */
    if (a->host_paced_port > 0) {
        int s = audio_open_tcp_paced(d->rtp_peer_ip, a->host_paced_port);
        if (s < 0) {
            harp_devlog(HARP_LOG_ERROR, "audio", "harp-deviced: host-paced TCP connect-back to :%d failed\n",
                    a->host_paced_port);
            return NULL;
        }
        a->host_paced_sock = s;
        a->fd = s;     /* D->H rendered frames */
        a->out_fd = s; /* H->D pacing frames   */
#ifdef _WIN32
        /* SO_RCVTIMEO so the pacing recv self-terminates on stop (see hp_read): a
         * blocking Winsock recv pending here cannot be woken from audio_stop's thread
         * by pthread_cancel or shutdown()/closesocket, so it must poll a->running. */
        { DWORD tmo = 200; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo); }
#endif
    }
    harp_devlog(HARP_LOG_INFO, "audio", "harp-deviced: audio thread up: mode=%u fd=%d out_fd=%d\n", a->mode,
            a->fd, a->out_fd);
    if (a->mode == 1) {
        host_paced_loop(d);
        harp_devlog(HARP_LOG_INFO, "audio", "harp-deviced: host-paced loop exited\n");
        return NULL;
    }
    /* voice starts from zero at audio.start (T15); notes/arp are reset by
     * evq_reset_for_new_stream, so zero ONLY the per-part voices here */
    engine_voices_cold();
    /* frame + sample buffers sized for the full 34-channel map (§P2.2); the
     * default stereo main mix uses only the first 2 channels */
    uint8_t frame[HARP_AUDIO_HDR_LEN + AUDIO_MAX_NSAMPLES * 34 * 4];
    float samples[AUDIO_MAX_NSAMPLES * 34];
    uint64_t msc = 0;
    uint64_t period_ns = (uint64_t)a->nsamples * 1000000000ull / a->rate;
    bool discont = false;
    /* §8.7 prefill burst: emit rtp_prebuffer frames back-to-back (no pacing) at
     * stream start so the host's jitter buffer fills from the first block, then
     * pace at realtime. 0 (USB / no-prebuffer) => paced immediately, unchanged. */
    bool primed = (a->rtp_prebuffer == 0);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (atomic_load_explicit(&a->running, memory_order_relaxed)) {
        /* §P2.2: render the requested output slots; `slots` carries the count.
         * Default {0,1} is the unchanged 2-channel main mix (golden holds). */
        uint16_t slots = render_output(a, samples, a->nsamples, (float)a->rate, msc);
        harp_audio_hdr h = {HARP_AUDIO_FVER, discont ? HARP_AUDIO_DISCONT : 0, slots,
                            a->epoch, msc, (uint16_t)a->nsamples, HARP_AUDIO_FMT_F32};
        discont = false;
        harp_audio_hdr_encode(&h, frame);
        size_t payload = (size_t)a->nsamples * slots * 4;
        memcpy(frame + HARP_AUDIO_HDR_LEN, samples, payload);
        if (a->fd >= 0 && !harp_write_all(a->fd, frame, HARP_AUDIO_HDR_LEN + payload))
            break; /* endpoint died (stop/unplug) */
        audio_rtp_emit(a, samples, payload, msc);  /* §8.7: no-op unless rtp_fd set */
        msc += a->nsamples;
        a->msc_final = msc; /* §7.1: publish each block so a mid-sleep cancel can't lose it
                             * (read by the session thread after join) — the next time.epoch's
                             * old-msc-final. */

        /* §8.7 prefill burst: while priming, emit back-to-back (skip the pace)
         * until rtp_prebuffer frames are out; then anchor realtime pacing at now
         * so the steady-state period starts from a freshly-filled host buffer. */
        if (!primed) {
            if (msc < a->rtp_prebuffer)
                continue;
            primed = true;
            clock_gettime(CLOCK_MONOTONIC, &next);
        }

        /* §8.7 bit-exact (host-locked): apply the host's rate trim (ppb) to the
         * emit period so the device emits at exactly the host's consumption rate.
         * trim 0 => nominal period, byte-identical to the untrimmed free-run. */
        int trim_ppb = atomic_load_explicit(&a->rate_trim_ppb, memory_order_relaxed);
        period_ns = (uint64_t)((double)a->nsamples * 1000000000.0 /
                               ((double)a->rate * (1.0 + (double)trim_ppb * 1e-9)));

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
            CTR_INC(d->audio_overruns);
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

void audio_stop(device *d) {
    /* §9.9: stop the meter pump first (a no-op if none is live). Done BEFORE the
     * thread_live early-return so an orphaned pump is always reaped, and before
     * tearing down the render thread so no echo races a half-stopped stream. */
    meter_pump_stop(d);
    if (!atomic_load_explicit(&d->audio.thread_live, memory_order_relaxed)) return;
    atomic_store_explicit(&d->audio.running, false, memory_order_relaxed); /* _Atomic: keep every access atomic (matches session.c) */
    /* The audio thread may be parked in a blocking endpoint read (mode 1 pacing).
     * On POSIX, recv()/read() are pthread cancellation points, so pthread_cancel
     * interrupts the park and the loop unwinds at once. On Windows/winpthreads a
     * blocking Winsock recv() is NOT interruptible from this thread — NEITHER
     * pthread_cancel NOR shutdown()/closesocket wakes a recv pending in another
     * thread; only the peer closing or a recv TIMEOUT does. So the host-paced recv
     * carries SO_RCVTIMEO and polls a->running (see hp_read): running=false above
     * makes it self-terminate within one tick (<=200 ms), so the join completes and
     * the device can answer audio.stop instead of hanging the host's teardown.
     * shutdown() is a best-effort faster wake on stacks that honor it (and is what
     * the POSIX cancel races to anyway). */
    if (d->audio.host_paced_sock >= 0)
#ifdef _WIN32
        shutdown(d->audio.host_paced_sock, SD_BOTH);
#else
        shutdown(d->audio.host_paced_sock, SHUT_RDWR);
#endif
    pthread_cancel(d->audio.thread);
    pthread_join(d->audio.thread, NULL);
    atomic_store_explicit(&d->audio.thread_live, false, memory_order_relaxed);
    audio_rtp_close(&d->audio); /* §8.7: thread joined -> the RTP dest outlives no sender */
    if (d->audio.host_paced_sock >= 0) { /* §8.3-over-§8.7: same, for the TCP host-paced channel */
#ifdef _WIN32
        closesocket(d->audio.host_paced_sock); /* Winsock SOCKET: POSIX close() is the wrong handle table */
#else
        close(d->audio.host_paced_sock);
#endif
        d->audio.host_paced_sock = -1;
    }
    d->audio.host_paced_port = 0;
    /* render thread is joined: free every voice + clear pending panic so nothing
     * leaks into the next stream (the engine owns the voice/part layout). */
    engine_voices_quiet();
    /* §8.8: free the FX input-demux buffer the loop allocated for THIS stream. It is sized for
     * this stream's n_in_slots; leaving it across streams both leaks it (at device exit) and
     * would be reused at the wrong size if a later FX stream requests more slots (the start-side
     * `!a->fx_in` guard only allocates when NULL). Re-allocated on the next FX start. */
    if (d->audio.fx_in) {
        free(d->audio.fx_in);
        d->audio.fx_in = NULL;
    }
    harp_devlog(HARP_LOG_INFO, "audio", "harp-deviced: audio stream stopped (%llu reanchors)\n",
            (unsigned long long)d->audio.reanchors);
}
