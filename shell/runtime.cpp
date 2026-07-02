#include "runtime.h"
#include "runtime_registry.h" /* §8.4 admission ledger (ledger_reserve/release/reserved) */
#include "runtime_log.h" /* log_msg / log_param_map_drift (shared w/ runtime_recall.cpp) */
#include "shell_config.h" /* HARP_SHELL_ENGINE_FILTER / HARP_SHELL_ETHERNET_ONLY (default = refdev) */
#include "ump.h"
#include "usb_transport.h" /* the concrete USB binding selectDevice() wraps */
#include "eth_transport.h" /* the §8.7 Ethernet binding (bit-exact host-locked) */
extern "C" {
#include "freerun.h" /* §8.7 ASRC: host-side clock recovery + resample (libsamplerate) */
}
#include <samplerate.h> /* SRC_* converter-quality enum for the freerun cfg */

#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <algorithm> /* §14.3: std::sort for the per-cycle RTT median */
#include <cmath>     /* §14.4 host-context-C: llround for est_ppm -> ppb */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "harp/plat.h" /* monotonic clock, hi-res sleep, UTC breakdown */

/* §8.8 never-silent guard: |sample| at or below this counts as silence. Above the
 * float denormal floor but far below any real audio, so the all-zeros wet of a broken
 * input path reads silent while a working reverb's wet reads live. */
static constexpr float kFxSilenceEps = 1e-6f;

/* §14.4 host-context-B: record a §12.1 state-machine transition into the
 * SessionHistory ring (control-path). Stamps wall-clock epoch + the current
 * stream MSC; copies a bounded `detail`. NOT on the audio path — see the ring's
 * thread-safety note. */
void HarpRuntime::recordTransition(uint8_t from, uint8_t to, uint8_t reason,
                                   const char *detail) {
    StateTransition t;
    t.tstamp_epoch = (uint64_t)time(nullptr);
    t.tstamp_msc = ssiRead_.load(std::memory_order_relaxed); /* stream-domain "now", 0 pre-stream */
    t.from_state = from;
    t.to_state = to;
    t.reason_code = reason;
    if (detail && detail[0]) {
        size_t n = strlen(detail);
        if (n >= sizeof t.detail) n = sizeof t.detail - 1;
        memcpy(t.detail, detail, n);
        t.detail[n] = '\0';
    }
    sessionHistory_.record(t);
}

/* §14.4 host-context-B: push a runtime log into the lock-free RuntimeLog ring.
 * WAIT-FREE — safe from any thread incl. audio. The stderr copy stays with
 * log_msg at the call site; this is the machine-readable copy for the bundle. */
void HarpRuntime::recordLog(uint8_t level, const char *tag, const char *msg) {
    LogRecord r;
    r.msc = ssiRead_.load(std::memory_order_relaxed);
    r.tstamp_epoch = (uint64_t)time(nullptr);
    r.level = level;
    if (tag && tag[0]) {
        size_t n = strlen(tag);
        if (n >= sizeof r.tag) n = sizeof r.tag - 1;
        memcpy(r.tag, tag, n);
        r.tag[n] = '\0';
    }
    if (msg && msg[0]) {
        size_t n = strlen(msg);
        if (n >= sizeof r.msg) n = sizeof r.msg - 1;
        memcpy(r.msg, msg, n);
        r.msg[n] = '\0';
    }
    runtimeLog_.push(r);
}

void HarpRuntime::defaultStoreDir(char *out, size_t n) {
#ifdef _WIN32
    /* %LOCALAPPDATA%\HARP\store. Forward slashes for the parts we append so the
     * store's mkdir -p (which splits on '/') creates each level; Win32 accepts
     * the mixed separators. */
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    snprintf(out, n, "%s/HARP/store", base && base[0] ? base : ".");
#else
    const char *home = getenv("HOME");
    snprintf(out, n, "%s/Library/Application Support/HARP/store", home ? home : "/tmp");
#endif
}

HarpRuntime::HarpRuntime() {
    harp_link_init(&link_);
    harp_cbuf_init(&msg_);
    char dir[512];
    defaultStoreDir(dir, sizeof dir);
    storeOk_ = harp_store_open(&store_, dir) == 0;
    if (!storeOk_) log_msg("cannot open host store at %s", dir);
}

HarpRuntime::~HarpRuntime() { stop(); }

/* ---------------- control plane ---------------- */

/* Shell error policy over the shared client: log and degrade (the DAW
 * must keep running). Caller holds ctlMutex_. */
bool HarpRuntime::request(harp_cbuf *req, harp_cbuf *rsp, harp_env *e) {
    int rc = harp_client_request(&client_, req, rsp, e);
    if (rc == HARP_CLIENT_EDEV)
        log_msg("device error on %s: %s %s", client_.err_method, client_.err_code,
                client_.err_msg);
    return rc == 0;
}

/* Build the union of all subscribed output slots and resolve each sink's column
 * indices. unionSlots_ = owner outSlots_ first, then each registered sink's
 * slots, with duplicates dropped (the same physical slot appears once in the
 * frame and several sinks can read that one column). Each slot's column index
 * is its position in unionSlots_. Caller holds sinksMutex_. */
void HarpRuntime::computeUnionSlotsLocked() {
    unionSlots_.clear();
    auto addSlot = [&](uint32_t s) {
        for (uint32_t u : unionSlots_)
            if (u == s) return; /* already in the union — one column for it */
        unionSlots_.push_back(s);
    };
    /* owner main mix first, so its columns are the contiguous prefix {0,1,...}:
     * the default {0,1}-only union puts the main mix at columns 0,1 exactly as
     * the pre-P5b contiguous frame, keeping the owner's reader copy identical. */
    for (uint32_t s : outSlots_) addSlot(s);
    for (size_t i = 0; i < nSinks_; i++)
        for (uint32_t s : sinks_[i]->slots) addSlot(s);
    /* §14.3: when the LoopbackMeasurer is armed, the device echoes the impulse onto
     * the out-slot's column — so the host MUST subscribe to that slot (active-slots-
     * out, key 4) or the column is never streamed back and the echo is invisible. Add
     * it to the union (after the real outputs, so the main-mix columns 0,1 stay the
     * contiguous prefix — the golden demux is undisturbed). Off the probe (not armed)
     * the union is exactly the historical {0,1}+sinks set => byte-identical. */
    if (loopbackArmed()) addSlot((uint32_t)loopbackOut_);
    /* resolve each sink's slots -> column indices within the union order */
    for (size_t i = 0; i < nSinks_; i++) {
        AudioSink *sk = sinks_[i];
        size_t n = sk->slots.size();
        if (n > 2) n = 2; /* a sink delivers a stereo pair at most */
        for (size_t c = 0; c < n; c++) {
            uint16_t col = 0;
            for (size_t u = 0; u < unionSlots_.size(); u++)
                if (unionSlots_[u] == sk->slots[c]) { col = (uint16_t)u; break; }
            sk->cols[c] = col;
        }
        if (n == 1) sk->cols[1] = sk->cols[0]; /* mono pair: L=R */
        /* This sink's slots are now part of the streamed union (its cols index
         * real columns), so the next demuxed frame carries its audio. Advance its
         * epoch: a LATE sink that padded silence (accruing padDebt) before its
         * slots joined the union learns, on its next pull, to drop that bogus debt
         * + clear the stale ring instead of eating the first real samples — the
         * B3 fix. A sink already streaming bumps too; pullAudio's reset is then a
         * no-op (padDebt already ~0, ring current). */
        sk->epoch.fetch_add(1, std::memory_order_release);
    }
}

/* Would the audio.start union change if recomputed now? Build the candidate
 * union (owner outSlots_ then each registered sink's slots, deduped, order
 * preserved — exactly the prefix computeUnionSlotsLocked builds) into a local
 * vector and compare element-for-element to the LIVE unionSlots_. Read-only: it
 * never writes unionSlots_ or any sink's cols, so register/unregisterAudioSink
 * can ask "do I need a re-neg?" without disturbing the reader's in-flight demux.
 * Caller holds sinksMutex_. */
bool HarpRuntime::unionWouldChangeLocked() const {
    std::vector<uint32_t> cand;
    auto addSlot = [&](uint32_t s) {
        for (uint32_t u : cand)
            if (u == s) return;
        cand.push_back(s);
    };
    for (uint32_t s : outSlots_) addSlot(s);
    for (size_t i = 0; i < nSinks_; i++)
        for (uint32_t s : sinks_[i]->slots) addSlot(s);
    return cand != unionSlots_;
}

void HarpRuntime::audioStopLocked() {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client_, &req, "audio.stop", false);
    harp_env e;
    request(&req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

/* §14.3 host LoopbackMeasurer — PUBLIC entry (host/main thread). Arms the
 * feeder-thread probe (loopbackPending_) and BLOCKS, bounded, for its result. The
 * actual probe runs on the FEEDER (runLoopbackProbeLocked) so its H->D pacing
 * writes don't race the feeder's own pacing. Off the golden path entirely: only an
 * armed measureLoopback() call sets the pending flag. */
HarpRuntime::LoopbackResult HarpRuntime::measureLoopback() {
    LoopbackResult r;
    r.in_slot = loopbackIn_;
    r.out_slot = loopbackOut_;
    r.rate = rate_;
    if (!loopbackArmed()) {
        r.detail = "loopback not armed (setLoopbackSlots before start)";
        return r;
    }
    if (!connected_.load(std::memory_order_acquire)) {
        r.detail = "no live session";
        return r;
    }
    if (freeRunning_.load(std::memory_order_acquire)) {
        /* the device requires a live HOST-PACED stream (diag.loopback.digital) */
        r.detail = "loopback needs a host-paced stream (offline/USB), not free-running RTP";
        return r;
    }
    loopbackDone_.store(false, std::memory_order_release);
    loopbackPending_.store(true, std::memory_order_release);
    /* Wait for the feeder to run the probe. The feeder loop cycles at worst every
     * ~1 ms idle (8 ms drain-on-stall) and the probe itself is a few hundred ms of
     * impulse cycles, so the default 10 s bound is generous and never trips in
     * practice. A slow rig (loaded RPi/KR260, heavy CI runner) can stretch it, so
     * HARP_LOOPBACK_TIMEOUT_MS overrides the bound (clamped [1000, 120000] ms). We
     * log a one-shot WARNING once the wait crosses 80 % of the bound so a near-miss
     * is visible in the log before it actually times out. If the session dies
     * underneath us, connected_ flips and we bail. */
    int timeoutMs = 10000;
    if (const char *e = getenv("HARP_LOOPBACK_TIMEOUT_MS")) {
        int v = atoi(e);
        if (v >= 1000 && v <= 120000) timeoutMs = v;
    }
    int warnAt = (timeoutMs * 8) / 10; /* 80 % of the bound */
    bool warned = false;
    for (int i = 0; i < timeoutMs; i++) {
        if (loopbackDone_.load(std::memory_order_acquire)) {
            r = loopbackResult_;
            return r;
        }
        if (!connected_.load(std::memory_order_acquire)) {
            loopbackPending_.store(false, std::memory_order_release);
            r.detail = "session dropped during measurement";
            return r;
        }
        if (!warned && i >= warnAt) {
            warned = true;
            log_msg("§14.3 loopback: probe still running after %d ms (%.0f%% of the %d ms "
                    "HARP_LOOPBACK_TIMEOUT_MS bound) — approaching timeout",
                    i, 100.0 * i / (double)timeoutMs, timeoutMs);
        }
        harp_sleep_ns(1000000ull); /* 1 ms */
    }
    loopbackPending_.store(false, std::memory_order_release);
    r.detail = "timed out waiting for the feeder probe (HARP_LOOPBACK_TIMEOUT_MS=" +
               std::to_string(timeoutMs) + " ms)";
    return r;
}

/* §14.3 LoopbackMeasurer probe BODY — runs on the FEEDER thread under ctlMutex_
 * (so it serializes with the eventPump's wire writes and getState/setState, and we
 * are the sole pacer). Quiesces the reader so we own BOTH audio endpoints for the
 * probe (the audioRenegotiateLocked pattern — NO teardown, connected_ stays true),
 * arms the device loop, injects periodic one-sample impulses on the in-slot column
 * in H->D pacing frames, locates each echo on the out-slot column of the D->H
 * frames, and derives the round-trip in samples. NEVER mutates the render path: the
 * device's loopback_on atomic gates the echo copy, and the impulse rides a slot the
 * synth is not driving, so with the probe off the output is byte-identical. */
void HarpRuntime::runLoopbackProbeLocked() {
    LoopbackResult r;
    r.in_slot = loopbackIn_;
    r.out_slot = loopbackOut_;
    r.rate = rate_;

    /* 1. quiesce the reader — single owner of the audio-IN endpoint for the probe.
     * connected_ stays TRUE (NOT a teardown), exactly as the P5b re-neg does. */
    readerStop_.store(true, std::memory_order_release);
    if (readerThread_.joinable()) readerThread_.join();
    readerStop_.store(false, std::memory_order_release);

    auto respawnReader = [this]() {
        readerThread_ = std::thread([this] { reader(); });
    };

    /* 1b. OUT-SLOT PREFLIGHT (review M3). The device OVERWRITES (does not mix) the
     * echo onto the chosen out-slot's column of the SAME rendered frame, so the
     * out-slot MUST be one the synth is NOT generating onto — otherwise the echo
     * stomps real synth output and corrupts the live render (and the captured mix).
     * Two refusals, both BEFORE we arm the device (so a misconfigured probe never
     * touches the render path):
     *   (a) the owner MAIN-MIX pair {0,1} carries the synth's stereo output on every
     *       session, so an out-slot of 0 or 1 would overwrite it. Refuse outright.
     *   (b) the out-slot MUST be present in the live audio.start union (unionSlots_):
     *       audioStart/computeUnionSlotsLocked add loopbackOut_ to the union when
     *       armed, so a correctly-armed probe finds it. If it is absent the device
     *       never streams that column back and the echo is unobservable — refuse
     *       rather than scan a column the host never receives.
     * On either refusal we publish a clear result, respawn the reader, and return
     * without arming — connected_ stays true and the render is undisturbed. */
    {
        bool outInUnion = false;
        for (uint32_t s : unionSlots_)
            if ((int)s == loopbackOut_) { outInUnion = true; break; }
        if (loopbackOut_ == 0 || loopbackOut_ == 1) {
            r.detail = "loopback out-slot " + std::to_string(loopbackOut_) +
                       " overlaps the owner main-mix pair {0,1} (would overwrite synth "
                       "output); choose an out-slot the synth does not drive";
            log_msg("§14.3 loopback: REFUSED — %s", r.detail.c_str());
            loopbackResult_ = r;
            respawnReader();
            return;
        }
        if (!outInUnion) {
            r.detail = "loopback out-slot " + std::to_string(loopbackOut_) +
                       " is not in the active audio.start union (column never streamed "
                       "back); arm setLoopbackSlots before start so it joins the union";
            log_msg("§14.3 loopback: REFUSED — %s", r.detail.c_str());
            loopbackResult_ = r;
            respawnReader();
            return;
        }
    }

    /* 2. diag.loopback.start {0=>in,1=>out,2=>"digital",3=>rate}. Parse the reply:
     * 0 armed, 2 eff-in, 3 eff-out, 4 eff-rate, 5 device-internal loop latency. */
    harp_cbuf sreq, srsp;
    harp_cbuf_init(&sreq);
    harp_cbuf_init(&srsp);
    harp_client_req_head(&client_, &sreq, "diag.loopback.start", true);
    harp_cbor_map(&sreq, 4);
    harp_cbor_uint(&sreq, 0);
    harp_cbor_uint(&sreq, (uint64_t)loopbackIn_);
    harp_cbor_uint(&sreq, 1);
    harp_cbor_uint(&sreq, (uint64_t)loopbackOut_);
    harp_cbor_uint(&sreq, 2);
    harp_cbor_text(&sreq, "digital");
    harp_cbor_uint(&sreq, 3);
    harp_cbor_uint(&sreq, rate_);
    harp_env se = {};
    bool startOk = request(&sreq, &srsp, &se) && se.has_body;
    harp_cbuf_free(&sreq);

    uint64_t devLoopLatency = 0; /* start-rsp key 5: device-internal loop latency */
    int effIn = loopbackIn_, effOut = loopbackOut_;
    if (startOk) {
        harp_cdec b;
        harp_cdec_init(&b, se.body, se.body_len);
        uint64_t n;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key == 0) {
                    bool armed = false;
                    if (!harp_cdec_bool(&b, &armed)) break;
                    r.armed = armed;
                } else if (key == 2) {
                    uint64_t v;
                    if (!harp_cdec_uint(&b, &v)) break;
                    effIn = (int)v;
                } else if (key == 3) {
                    uint64_t v;
                    if (!harp_cdec_uint(&b, &v)) break;
                    effOut = (int)v;
                } else if (key == 5) {
                    if (!harp_cdec_uint(&b, &devLoopLatency)) break;
                } else if (!harp_cdec_skip(&b))
                    break;
            }
        }
    }
    harp_cbuf_free(&srsp);

    if (!startOk || !r.armed) {
        r.detail = startOk ? "device did not arm the loopback"
                           : "diag.loopback.start failed (no cap / device error)";
        loopbackResult_ = r;
        respawnReader();
        return;
    }
    /* §6.4 soft-fallback: if the device routed to slots OTHER than requested, note
     * it (the device clamps/remaps); we measure against the EFFECTIVE routing. */
    if (effIn != loopbackIn_ || effOut != loopbackOut_)
        r.detail = "device remapped routing (eff in=" + std::to_string(effIn) +
                   " out=" + std::to_string(effOut) + "); measuring effective";

    /* 3. expected round-trip (§6.4): pure transport buffering, device-internal loop
     * latency = 0 (the same-frame copy). Derived from the device's OWN latency-profile
     * (identity key 8): input-latency + output-latency + one buffer-depth turnaround,
     * matched to the negotiated rate. For the refdev (in=out=0, buf-depth=256 @ 48 kHz)
     * this is one 256-sample host-paced block — the single-frame pacing turnaround the
     * feeder documents ("its pacing turnaround is just render time"). A device with real
     * analog/converter buffers reports nonzero in/out latency and this grows to match
     * §6.4. Subtract the device-internal loop latency (start-rsp key 5 = 0 for digital)
     * — kept in the formula so a future analog loop's nonzero key 5 is handled too. */
    const uint64_t kImpulseFrames = 1; /* the impulse rides one pacing block */
    r.expected_samples = expectedLoopbackSamples(rate_);

    /* 4. drain any in-flight D->H tail so the probe starts from a clean read frontier
     * (sole reader now). Up to a brief quiet window, exactly as the re-neg drain. */
    {
        uint8_t junk[65536];
        int quiet = 0;
        while (quiet < 2) {
            int rd = transport_->audioRead(junk, sizeof junk, 40);
            if (rd < 0) break;
            quiet = (rd == 0) ? quiet + 1 : 0;
        }
    }

    /* 5. impulse/echo cycles. Each cycle: inject one impulse (sample 0 = 1.0) on the
     * in-slot's single column in an H->D pacing frame at SSI=tx, keep pacing silent
     * frames to fill the pipeline, then read D->H frames until the impulse appears on
     * the out-slot column. The echo frame carries the impulse frame's ts (same-frame
     * copy), and the host's send frontier (ssi_) has advanced past it by the in-flight
     * pipeline depth — that gap IS the round-trip in samples.
     *
     * ECHO VALIDATION (review BLOCKER). A peak above threshold is NOT proof of OUR
     * impulse — a stale tail frame, a wrong-frame peak, or residual content on the
     * out-slot column could all trip a bare peak finder and fold a spurious RTT into
     * the result. The device echoes IN->OUT in the SAME rendered frame and stamps the
     * echo header ts = the impulse frame's ts (engine.c §14.3: out.ts = h.ts), and the
     * impulse rides sample offset 0 of the impulse frame — so a GENUINE echo has a
     * peak whose absolute SSI position (h.ts + peakOffset) equals txTs + 0 EXACTLY,
     * i.e. h.ts == txTs AND peakOffset == 0. We require all three (frame match, offset
     * match, above threshold); any mismatch marks the cycle INVALID — no RTT folded.
     *
     * We collect EVERY valid cycle's RTT into rtts[] (review MAJOR), then take the
     * MEDIAN and reject outliers in step 6, instead of a mean that one jittered cycle
     * could skew. */
    const int kCycles = 16;     /* impulse cycles (a few invalid ones still leave a quorum) */
    const int kSpacing = 8;     /* silent pacing frames between impulses */
    bool anyEcho = false;
    int invalidCycles = 0;      /* peak seen but it failed echo validation */
    std::vector<double> rtts;   /* one entry per VALID cycle (review MAJOR: median over these) */
    rtts.reserve(kCycles);

    /* accumulator for partial D->H frames across reads */
    uint8_t acc[65536];
    size_t accLen = 0;
    /* The single declared in-slot column is column 0 of an h.slots=1 frame; the
     * out-slot column is resolved from the D->H frame's slot count against the
     * negotiated union order. The host knows the union: unionSlots_ (column index
     * of loopbackOut_ within it). */
    int outCol = -1;
    for (size_t u = 0; u < unionSlots_.size(); u++)
        if ((int)unionSlots_[u] == effOut) { outCol = (int)u; break; }

    for (int cyc = 0; cyc < kCycles && connected_.load(std::memory_order_relaxed); cyc++) {
        uint64_t txTs = ssi_;
        bool sentImpulse = false;
        bool foundThisCycle = false;
        /* a bounded number of pacing frames per cycle: 1 impulse + kSpacing silent,
         * then keep pacing-and-reading until the echo lands or we give up. */
        for (int step = 0; step < kSpacing + 64 && !foundThisCycle &&
                            connected_.load(std::memory_order_relaxed); step++) {
            /* build an H->D pacing frame. On the FIRST step it carries the impulse
             * (h.slots=1, payload = kBlock floats, sample 0 = 1.0); afterwards plain
             * silent pacing (h.slots=0, the byte-identical golden frame). */
            bool impulseFrame = !sentImpulse;
            uint16_t slots = impulseFrame ? 1 : 0;
            harp_audio_hdr pace = {HARP_AUDIO_FVER,
                                   (uint8_t)(HARP_AUDIO_DIR_H2D | HARP_AUDIO_FENCE),
                                   slots,
                                   0,
                                   ssi_,
                                   (uint16_t)kBlock,
                                   HARP_AUDIO_FMT_F32};
            uint8_t hdr[HARP_AUDIO_HDR_LEN + HARP_AUDIO_FENCE_LEN];
            harp_audio_hdr_encode(&pace, hdr);
            /* event fence = the live high-water minus the epoch baseline (saturating),
             * IDENTICAL to the feeder's pacing fence — so the device's barrier is
             * satisfied exactly as on a normal frame (no event is left pending). Same
             * EventManager::fenceStamp() the feeder stamps — one owner for both. */
            uint32_t seq = events_.fenceStamp();
            hdr[HARP_AUDIO_HDR_LEN + 0] = (uint8_t)seq;
            hdr[HARP_AUDIO_HDR_LEN + 1] = (uint8_t)(seq >> 8);
            hdr[HARP_AUDIO_HDR_LEN + 2] = (uint8_t)(seq >> 16);
            hdr[HARP_AUDIO_HDR_LEN + 3] = (uint8_t)(seq >> 24);
            if (impulseFrame) {
                /* one block of slot-interleaved (here single-column) floats: all zero
                 * except sample 0 = 1.0 — the dominant content on the out-slot, which
                 * the synth is not driving (the caller chose an unused out-slot). */
                float payload[kBlock];
                memset(payload, 0, sizeof payload);
                payload[0] = 1.0f;
                uint8_t frame[HARP_AUDIO_HDR_LEN + HARP_AUDIO_FENCE_LEN + sizeof payload];
                memcpy(frame, hdr, sizeof hdr);
                memcpy(frame + sizeof hdr, payload, sizeof payload);
                if (!transport_->audioWrite(frame, (int)sizeof frame, 50)) break;
                txTs = ssi_;
                sentImpulse = true;
            } else {
                if (!transport_->audioWrite(hdr, (int)sizeof hdr, 50)) break;
            }
            ssi_ += kBlock;
            framesSent_++;

            /* read whatever D->H frames are available and scan the out-slot column. */
            int rd = transport_->audioRead(acc + accLen, (int)(sizeof acc - accLen), 20);
            if (rd < 0) break;
            if (rd > 0) accLen += (size_t)rd;
            size_t off = 0;
            while (accLen - off >= HARP_AUDIO_HDR_LEN) {
                harp_audio_hdr h;
                if (!harp_audio_hdr_decode(acc + off, &h)) { accLen = 0; off = 0; break; }
                size_t need = HARP_AUDIO_HDR_LEN + harp_audio_payload_len(&h);
                if (accLen - off < need) break;
                /* scan the out-slot column for the impulse peak (it dominates — the
                 * out-slot carries no synth notes). A column index of -1 (out-slot not
                 * in the union) means the host can't see the echo; skip — echo_found
                 * stays false and the caller learns the routing was unobservable. */
                if (outCol >= 0 && (uint16_t)outCol < h.slots && !foundThisCycle) {
                    const float *pl = (const float *)(acc + off + HARP_AUDIO_HDR_LEN);
                    uint16_t S = h.slots;
                    int peak = -1;
                    float peakAbs = 0.25f; /* threshold: well above synth-free noise */
                    for (uint32_t s = 0; s < h.nsamples; s++) {
                        float v = pl[(size_t)s * S + outCol];
                        float a = v < 0 ? -v : v;
                        if (a > peakAbs) { peakAbs = a; peak = (int)s; }
                    }
                    if (peak >= 0) {
                        /* ECHO VALIDATION (review BLOCKER): match the located peak back
                         * to OUR impulse before trusting it.
                         *   - FRAME match: the device copies in->out in the SAME frame and
                         *     stamps the echo ts = the impulse frame's ts, so a genuine
                         *     echo has h.ts == txTs. A peak in any OTHER frame (a stale
                         *     tail, a later silent frame) is not this impulse.
                         *   - OFFSET match: we injected at sample 0, so the echoed peak
                         *     must land at offset 0 (kImpulseOffset). A peak at any other
                         *     offset is residual content, not the impulse.
                         * Only a peak that satisfies BOTH yields a trustworthy RTT
                         * (= ssi_ - txTs). A peak that fails either is a SPURIOUS hit:
                         * count it as an invalid cycle and KEEP scanning this cycle's
                         * later frames for the real echo (do NOT set foundThisCycle, so a
                         * subsequent matching frame can still be accepted). */
                        const uint32_t kImpulseOffset = 0; /* impulse rides sample 0 */
                        bool frameMatch = (h.ts == txTs);
                        bool offsetMatch = ((uint32_t)peak == kImpulseOffset);
                        if (frameMatch && offsetMatch) {
                            /* rx position in the SSI domain = this frame's ts + peak offset
                             * == txTs; round-trip = how far our send frontier (ssi_) ran
                             * past it. */
                            uint64_t rxTs = h.ts + (uint64_t)peak;
                            if (ssi_ > rxTs) {
                                rtts.push_back((double)(ssi_ - rxTs));
                                anyEcho = true;
                                foundThisCycle = true;
                            } else {
                                invalidCycles++; /* frontier behind the echo: impossible */
                            }
                        } else {
                            invalidCycles++; /* wrong frame or wrong offset — not our impulse */
                        }
                    }
                }
                off += need;
            }
            if (off) { memmove(acc, acc + off, accLen - off); accLen -= off; }
        }
        (void)txTs;
        (void)kImpulseFrames;
    }

    /* 6. diag.loopback.stop — disengage the device loop, restore normal routing. */
    {
        harp_cbuf preq, prsp;
        harp_cbuf_init(&preq);
        harp_cbuf_init(&prsp);
        harp_client_req_head(&client_, &preq, "diag.loopback.stop", false);
        harp_env pe = {};
        request(&preq, &prsp, &pe);
        harp_cbuf_free(&preq);
        harp_cbuf_free(&prsp);
    }

    r.echo_found = anyEcho;

    /* 6b. AGGREGATE (review MAJOR): MEDIAN over the per-cycle RTTs with OUTLIER
     * REJECTION, not a raw mean a single jittered cycle could skew.
     *   1. require a QUORUM of valid cycles (kMinValidCycles) — a result drawn from
     *      one or two echoes is not trustworthy on a jittery transport; fail loudly
     *      rather than report a fragile number.
     *   2. take the median of all valid RTTs (robust to a stray cycle, unlike a mean).
     *   3. REJECT any cycle whose RTT differs from the median by more than a tight
     *      threshold (kOutlierSamples), then re-median the survivors — so a smeared
     *      pacing cycle is dropped instead of dragging the reported RTT off the §6.4
     *      value. After culling, re-check the quorum on the survivors.
     *   4. report that culled median as r.rtt_samples (minus the device-internal loop
     *      latency, key 5 = 0 for the digital same-frame copy). */
    const size_t kMinValidCycles = 5;   /* quorum: fewer is not a trustworthy median */
    const double kOutlierSamples = 64.0; /* tight band around the median (1/4 pacing block) */
    if (rtts.size() >= kMinValidCycles) {
        std::sort(rtts.begin(), rtts.end());
        double med = rtts[rtts.size() / 2];
        /* cull cycles outside the tight band around the first-pass median */
        std::vector<double> kept;
        kept.reserve(rtts.size());
        for (double v : rtts) {
            double d = v - med;
            if (d < 0) d = -d;
            if (d <= kOutlierSamples) kept.push_back(v);
        }
        size_t rejected = rtts.size() - kept.size();
        if (kept.size() >= kMinValidCycles) {
            std::sort(kept.begin(), kept.end()); /* already sorted, but explicit */
            double meas = kept[kept.size() / 2]; /* median of the survivors */
            if (meas > (double)devLoopLatency) meas -= (double)devLoopLatency;
            r.rtt_samples = meas;
            r.delta_ms = (meas - r.expected_samples) * 1000.0 / (double)rate_;
            r.ok = true;
            if (r.detail.empty())
                r.detail = "measured " + std::to_string((int)(meas + 0.5)) +
                           " samples (median of " + std::to_string(kept.size()) +
                           " valid cycle(s); " + std::to_string(rejected) +
                           " outlier(s), " + std::to_string(invalidCycles) +
                           " invalid)";
        } else {
            r.detail = "too few in-band cycles after outlier rejection (" +
                       std::to_string(kept.size()) + " of " +
                       std::to_string(rtts.size()) + " within ±" +
                       std::to_string((int)kOutlierSamples) +
                       " samples; need " + std::to_string(kMinValidCycles) + ")";
        }
    } else if (anyEcho) {
        r.detail = "too few valid echo cycles (" + std::to_string(rtts.size()) +
                   " of " + std::to_string(kMinValidCycles) + " required; " +
                   std::to_string(invalidCycles) + " spurious peak(s) rejected)";
    } else {
        if (r.detail.empty())
            r.detail = "armed but no echo detected on the out-slot column";
    }

    loopbackResult_ = r;

    /* 7. respawn the reader on the (unchanged) union — SSI continuous, no reset. The
     * feeder resumes pacing on its next loop iteration from the advanced ssi_. */
    respawnReader();

    log_msg("§14.3 loopback: %s (in=%d out=%d, measured=%.0f expected=%.0f delta=%.3f ms)",
            r.ok ? "OK" : (r.echo_found ? "echo but no RTT" : "no echo"),
            effIn, effOut, r.rtt_samples, r.expected_samples, r.delta_ms);
}

/* Drain any asynchronous inbound link traffic: evt echoes (-> echoRing_)
 * and notifications. Non-blocking-ish: one short-timeout fill, then
 * consume complete messages. */
void HarpRuntime::pollEcho() {
    std::unique_lock<std::mutex> lk(ctlMutex_, std::try_to_lock);
    if (!lk.owns_lock()) return; /* a state op owns the link; echoes wait */
    if (!transport_->linkPoll(1)) return;
    while (transport_->linkPending() > 0) {
        uint8_t stream;
        if (harp_link_recv(transport_->ctlIo(), &link_, &stream, &msg_) != 0) {
            log_msg("link receive failed; device gone?");
            connected_.store(false, std::memory_order_release);
            return;
        }
        if (stream == HARP_STREAM_EVT) {
            harp_cdec dec;
            harp_cdec_init(&dec, msg_.buf, msg_.len);
            uint64_t alen, tn, ep, ts, etype;
            if (!harp_cdec_array(&dec, &alen) || alen < 3 ||
                !harp_cdec_array(&dec, &tn) || tn != 2 || !harp_cdec_uint(&dec, &ep) ||
                !harp_cdec_uint(&dec, &ts) || !harp_cdec_uint(&dec, &etype))
                continue;
            if (etype != 1) continue;
            uint64_t n, key, id = 0, part = 0;
            double v = 0;
            bool ok = harp_cdec_map(&dec, &n);
            for (uint64_t i = 0; ok && i < n; i++) {
                if (!harp_cdec_uint(&dec, &key)) break;
                if (key == 0)
                    ok = harp_cdec_uint(&dec, &id);
                else if (key == 1)
                    ok = harp_cdec_float(&dec, &v);
                else if (key == 5) /* §9.4 multitimbral part; omitted => part 0 */
                    ok = harp_cdec_uint(&dec, &part);
                else
                    ok = harp_cdec_skip(&dec);
            }
            if (ok) echoRing_.push({(uint32_t)id, (float)v, (uint16_t)part});
        } else if (stream == HARP_STREAM_OBJ && storeOk_) {
            /* §11.2: verify-on-receipt — discard a malformed object (harp_obj_kind < 0) rather than
             * store unparseable bytes under a meaningless content hash (the 3rd obj receiver,
             * matching device handle_obj + host pump_one). */
            if (harp_obj_kind(msg_.buf, msg_.len) >= 0)
                harp_store_put(&store_, msg_.buf, msg_.len, nullptr);
        }
        /* ctl notifications: tolerated and dropped for now */
    }
}

#ifdef __APPLE__
void HarpRuntime::setWorkgroup(os_workgroup_t wg) {
    std::lock_guard<std::mutex> lk(wgMutex_);
    if (wg == wg_) return;
    if (wg) os_retain(wg);
    if (wg_) os_release(wg_);
    wg_ = wg;
    wgGen_.fetch_add(1, std::memory_order_release);
    log_msg(wg ? "joined the host's audio workgroup" : "left the audio workgroup");
}

void HarpRuntime::wgMaintain(WgState &st) {
    uint64_t gen = wgGen_.load(std::memory_order_acquire);
    if (gen == st.gen) return;
    if (st.joined) {
        os_workgroup_leave(st.joined, &st.token);
        os_release(st.joined);
        st.joined = nullptr;
    }
    os_workgroup_t target = nullptr;
    {
        std::lock_guard<std::mutex> lk(wgMutex_);
        target = wg_;
        if (target) os_retain(target);
        gen = wgGen_.load(std::memory_order_relaxed);
    }
    if (target) {
        if (os_workgroup_join(target, &st.token) == 0) {
            st.joined = target;
        } else { /* cancelled workgroup: drop the ref, stay unjoined */
            os_release(target);
        }
    }
    st.gen = gen;
}
#endif

/* ---------------- audio thread side ---------------- */

/* ---- audio sink registry (P5b; off the audio path) ---- */

/* Register a per-part audio sink for `slots`. Allocates an AudioSink and adds it
 * under sinksMutex_; reader()'s next demux pass splits the device frame into it
 * (its column indices are resolved by computeUnionSlotsLocked, called from
 * audioStart). Returns nullptr if the table is full (kMaxSinks parts) — the
 * caller then pulls silence, like an event-dormant 17th part. Empty slots is a
 * no-op (nullptr): such an instance just pulls the owner main mix or silence.
 *
 * P5b LIMITATION: the union is fixed at audio.start. A sink registered while a
 * session is ALREADY streaming is in the registry (reader() demuxes it) but its
 * slots are NOT in the live union, so its columns resolve to 0 — it would read
 * the owner's main-mix column, not its part. To avoid surprising audio we leave
 * cols at their default and the caller gets the main mix until the next
 * audio.start; in practice a DAW's tracks activate together at project load, so
 * every part's sink is registered before the owner starts and enters the union
 * then. (A clean stop+restart re-negotiation is possible but a mid-session
 * audio glitch is worse than fixed-at-start; see audioStart.) */
AudioSink *HarpRuntime::registerAudioSink(const std::vector<uint32_t> &slots) {
    if (slots.empty()) return nullptr;
    AudioSink *sink = new AudioSink(slots);
    std::lock_guard<std::mutex> lk(sinksMutex_);
    if (nSinks_ >= kMaxSinks) {
        delete sink;
        return nullptr;
    }
    sinks_[nSinks_++] = sink;
    haveSinks_.store(true, std::memory_order_release);
    /* P5b RE-NEGOTIATION: if we are ALREADY streaming and this sink's slots are
     * not in the live union, the feeder must re-stream the new union so the late
     * sink hears its part (otherwise its cols resolve outside the frame and it
     * reads silence — the pre-reneg fixed-at-start limitation). We do NOT touch
     * the wire here (this is the plugin's setActive / main thread); we only raise
     * a flag the feeder acts on at a safe boundary under ctlMutex_. Before start()
     * (not running_) this sink's slots simply enter the initial audio.start union
     * — unchanged, no re-neg. */
    if (running_.load(std::memory_order_acquire) && unionWouldChangeLocked())
        audioRenegPending_.store(true, std::memory_order_release);
    return sink;
}

/* Remove + free a sink on release, keeping reader() safe. SAFE-FREE: a sink's
 * ring is only WRITTEN by reader(), and only while it holds sinksMutex_ (it
 * demuxes every frame into every registered sink under the lock — see reader).
 * We take that SAME lock and remove the sink from the array
 * FIRST: reader()'s next pass can no longer see it, so we are its sole accessor
 * (the consumer is quiescent — the host stops process() before release) and can
 * free it. Idempotent on nullptr. */
void HarpRuntime::unregisterAudioSink(AudioSink *sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lk(sinksMutex_);
    for (size_t i = 0; i < nSinks_; i++) {
        if (sinks_[i] == sink) {
            sinks_[i] = sinks_[nSinks_ - 1]; /* compact: last fills the hole */
            sinks_[--nSinks_] = nullptr;
            if (nSinks_ == 0) haveSinks_.store(false, std::memory_order_release);
            delete sink;
            /* P5b RE-NEGOTIATION on removal too: dropping a sink can shrink the
             * union (its private slots leave it), so re-stream the narrower union
             * — same feeder-driven, off-the-wire flag as registerAudioSink. If the
             * removed slots were shared with another sink / the owner the union is
             * unchanged and unionWouldChangeLocked() is false, so nothing fires. */
            if (running_.load(std::memory_order_acquire) && unionWouldChangeLocked())
                audioRenegPending_.store(true, std::memory_order_release);
            return;
        }
    }
}

/* Padded stream positions are SPENT: ssiRead_ always advances by the full
 * request, and the late-arriving samples for those positions are dropped
 * (padDebtFloats_) when they show up. The wrong policy — playing late
 * arrivals anyway — grows latency by every pad and audibly "echoes" the
 * missing moment while the DAW grid drifts.
 *
 * §8.8 audio.fx EXCEPTION: an armed effect (fxArmed) is a FIXED-LATENCY DELAY
 * LINE, not a 1:1 free-running synth stream. Its wet is inherently PDC-late
 * (round-trip ≈ the reported latencySamples()): the first ~PDC pulls
 * legitimately underrun while the H→D→wet pipeline primes, then the wet flows
 * continuously (the device runs faster than real-time, so once primed the ring
 * stays full). For the FX, a late wet IS the signal — just delayed by the PDC
 * the plugin already reports (the DAW compensates) — NOT a dropout to skip.
 * Dropping it (the spent-position policy) re-empties the ring every block and
 * yields 100% silence. So skip the settle for the FX: the PDC-late wet
 * accumulates in audioRing_ and plays. fxArmed()/fxInSlots_ is fixed before
 * start() (never mutated mid-session), so reading it here is race-free, and the
 * INSTRUMENT shell never arms it — its synth/free-running path is unchanged. */
void HarpRuntime::settlePadDebt() {
    if (fxArmed()) return;
    while (padDebtFloats_) {
        float scratch[1024];
        size_t take = padDebtFloats_ < 1024 ? padDebtFloats_ : 1024;
        size_t got = audioRing_.read(scratch, take);
        if (!got) break; /* not arrived yet; keep owing */
        padDebtFloats_ -= got;
    }
}

/* per-sink analogue of settlePadDebt: a demuxed sink rings just like audioRing_,
 * so the same spent-SSI policy applies — drop late arrivals owed to its padded
 * positions. The sink's pullAudio is its sole consumer (SPSC), so padDebt is
 * audio-thread-owned just like padDebtFloats_. */
void HarpRuntime::settleSinkPadDebt(AudioSink &sink) {
    while (sink.padDebt) {
        float scratch[1024];
        size_t take = sink.padDebt < 1024 ? sink.padDebt : 1024;
        size_t got = sink.ring.read(scratch, take);
        if (!got) break; /* not arrived yet; keep owing */
        sink.padDebt -= got;
    }
}

/* B3: when this sink's slots (re)enter the streamed union, computeUnionSlotsLocked
 * advances sink.epoch. On the FIRST pull that observes a new epoch the consumer
 * (pullAudio — sole toucher of padDebt + sole ring reader) ZEROES the pad debt and
 * clear()s the ring. This discards the silence the sink padded WHILE WAITING for a
 * late re-negotiation to stream its slots (and any stale pre-re-neg ring contents),
 * so the first real demuxed frame plays instead of being eaten by settleSinkPadDebt
 * paying down a bogus debt. ring.clear() is a consumer-side tail move — safe against
 * the reader's producer-side writes. A no-op cost on a sink already in the union
 * (epoch unchanged between pulls). */
void HarpRuntime::syncSinkEpoch(AudioSink &sink) {
    uint32_t ep = sink.epoch.load(std::memory_order_acquire);
    if (ep != sink.epochSeen) {
        sink.epochSeen = ep;
        /* Only drop the ring when there is BOGUS padding to clear. The epoch bump
         * (computeUnionSlotsLocked, every sink) signals "this sink's slots joined the
         * union". For a LATE-attached sink that padded silence first, padDebt > 0 and the
         * ring holds stale silence — drop both (the B3 fix). But a MULTI-OUT sink registered
         * BEFORE audio.start has padDebt == 0 and a ring of VALID prefill from the initial
         * union; clearing it would discard real audio and (with 2+ sinks) cascade into a
         * perpetual underrun — the offline/USB multi-out hang. So clear only when padDebt>0. */
        if (sink.padDebt > 0) {
            sink.padDebt = 0;
            sink.ring.clear();
        }
    }
}

/* Shared RT-underrun tail for the pullAudio*() family — see the header for the
 * contract. Keeps the four short-read epilogues (owner/sink × realtime/blocking) in
 * one place: zero-fill [got,want), accrue pad debt, count the underrun. */
size_t HarpRuntime::padUnderrun(float *dst, size_t got, size_t want, size_t *padDebt,
                                bool count) {
    memset(dst + got, 0, (want - got) * sizeof(float));
    if (padDebt) *padDebt += want - got;
    if (count) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
        padSamples_.fetch_add((want - got) / 2, std::memory_order_relaxed);
    }
    return (want - got) / 2;
}

size_t HarpRuntime::pullAudio(float *dst, size_t nFrames) {
    /* Reads the stable audioRing_ for BOTH bindings — USB's reader() demuxes the
     * host-paced frames into it; Ethernet's reader() writes the 1:1 RTP frames
     * into it (bit-exact). The DAW audio thread therefore never touches transport_,
     * so a reconnect reaping the transport can't race this thread. */
    settlePadDebt();
    size_t want = nFrames * 2;
    size_t got = audioRing_.read(dst, want);
    ssiRead_.fetch_add(nFrames, std::memory_order_relaxed);
    size_t shortBy = 0;
    if (got < want) {
        /* §8.8: only the 1:1 synth path owes droppable pad debt. For an armed FX
         * (fxArmed) this short read is PRIMING silence while the fixed-latency wet
         * pipeline fills — the wet is PDC-late, not spent — so pass a null accumulator
         * (no drop scheduled; settlePadDebt early-returns for the FX too). We still
         * COUNT the underrun when connected_: for the FX these are the expected handful
         * of priming blocks, after which the ring stays full and there are none.
         * fxInSlots_ is fixed before start(). */
        shortBy = padUnderrun(dst, got, want, fxArmed() ? nullptr : &padDebtFloats_,
                              connected_.load(std::memory_order_acquire));
    }
    /* §8.8 never-silent guard: observe the wet just delivered (read-only). Gated on
     * fxArmed, so the instrument path is byte-identical (the golden gate). */
    if (fxArmed()) observeFxWet(dst, nFrames);
    return shortBy;
}

/* Per-part pull: drain THIS sink's demuxed stereo ring. Structurally identical
 * to the owner pull above but on the sink's own ring/padDebt — the sink's ring
 * holds interleaved L/R, written by reader()'s demux. It does NOT advance
 * ssiRead_: that is the OWNER's main-mix stream clock (pacing + event timing)
 * and only the owner pull moves it; per-part instances ride the owner's clock,
 * which is why their process() timestamps against the shared streamPos(). A null
 * sink (the registry was full, or no per-part slots) pulls clean silence — the
 * caller is the audio-silent fallback (like an event-dormant part). */
size_t HarpRuntime::pullAudio(AudioSink *sink, float *dst, size_t nFrames) {
    if (!sink) {
        memset(dst, 0, nFrames * 2 * sizeof(float));
        return nFrames;
    }
    syncSinkEpoch(*sink); /* B3: drop pre-(re)negotiation pad debt + stale ring */
    settleSinkPadDebt(*sink);
    size_t want = nFrames * 2;
    size_t got = sink->ring.read(dst, want);
    if (got < want)
        return padUnderrun(dst, got, want, &sink->padDebt,
                           connected_.load(std::memory_order_acquire));
    return 0;
}

size_t HarpRuntime::pullAudioBlocking(float *dst, size_t nFrames, unsigned timeoutMs) {
    size_t want = nFrames * 2;
    size_t got = 0;
    unsigned waited = 0;
    bool settled = false; /* settlePadDebt + ssiRead_ advance run ONCE, after any flip clears */
    while (got < want) {
        /* §8.3-over-§8.7 mid-stream toggle fence: while a live<->offline re-dial is in
         * flight, the OLD session's ring holds stale samples and ssiRead_/padDebt belong
         * to the OLD SSI domain. Don't read, don't pad-settle, don't treat !connected_ as
         * terminal — wait for the NEW session (ABSOLUTE gen test, so a pull that starts
         * after the target was reached still releases). sessionUp clear()'d the ring and
         * reset ssiRead_/padDebt for the new domain. The no-flip path is byte-identical to
         * before: first iteration settles + advances + reads, exactly as the old top-of-fn. */
        bool flipping = modeFlipPending_.load(std::memory_order_acquire) &&
                        sessionGen_.load(std::memory_order_acquire) <
                            flipTargetGen_.load(std::memory_order_acquire);
        if (!flipping) {
            if (!settled) {
                settlePadDebt();
                ssiRead_.fetch_add(nFrames, std::memory_order_relaxed);
                settled = true;
            }
            got += audioRing_.read(dst + got, want - got);
            if (got >= want) break;
        }
        if ((!flipping && !connected_.load(std::memory_order_acquire)) || waited >= timeoutMs) {
            if (!settled) ssiRead_.fetch_add(nFrames, std::memory_order_relaxed); /* advance EXACTLY once per call */
            size_t shortBy = padUnderrun(dst, got, want, &padDebtFloats_, true);
            if (fxArmed()) observeFxWet(dst, nFrames); /* §8.8 never-silent guard (offline) */
            return shortBy;
        }
        harp_sleep_ns(500000ull); /* 0.5 ms */
        waited++;
    }
    if (fxArmed()) observeFxWet(dst, nFrames); /* §8.8 never-silent guard (offline) */
    return 0;
}

/* Offline per-part pull: block until the sink's demuxed range has arrived. Like
 * the owner blocking pull but on the sink's ring/padDebt and WITHOUT advancing
 * ssiRead_ (the owner's clock). A null sink yields silence immediately. */
size_t HarpRuntime::pullAudioBlocking(AudioSink *sink, float *dst, size_t nFrames,
                                      unsigned timeoutMs) {
    if (!sink) {
        memset(dst, 0, nFrames * 2 * sizeof(float));
        return nFrames;
    }
    size_t want = nFrames * 2;
    size_t got = 0;
    unsigned waited = 0;
    bool settled = false; /* syncSinkEpoch + settleSinkPadDebt run ONCE, after any flip clears */
    while (got < want) {
        /* §8.3-over-§8.7 mid-stream toggle fence (same as the owner form): wait out a
         * live<->offline re-dial before touching the sink ring/epoch, which belong to the
         * OLD SSI domain. No ssiRead_ here — the sink form never advances the owner clock. */
        bool flipping = modeFlipPending_.load(std::memory_order_acquire) &&
                        sessionGen_.load(std::memory_order_acquire) <
                            flipTargetGen_.load(std::memory_order_acquire);
        if (!flipping) {
            if (!settled) {
                syncSinkEpoch(*sink); /* B3: drop pre-(re)negotiation pad debt + stale ring */
                settleSinkPadDebt(*sink);
                settled = true;
            }
            got += sink->ring.read(dst + got, want - got);
            if (got >= want) break;
        }
        if ((!flipping && !connected_.load(std::memory_order_acquire)) || waited >= timeoutMs) {
            return padUnderrun(dst, got, want, &sink->padDebt, true);
        }
        harp_sleep_ns(500000ull); /* 0.5 ms */
        waited++;
    }
    return 0;
}

/* §8.8 audio.fx: the FX shell's process() pushes the track's input here each
 * block; the feeder pops kBlock-frame chunks and frames them H→D (see feeder()).
 * Interleaved by fxInSlots_.size() columns (mono in => 1). Lock-free SPSC — the
 * feeder is the sole consumer. No-op (returns 0) when not armed, so the
 * instrument shell never touches this. Drops the tail on overflow (the feeder
 * fell behind: the output underruns in lockstep, which the pull side counts). */
size_t HarpRuntime::writeFxInput(const float *interleaved, size_t nFrames) {
    if (fxInSlots_.empty() || !interleaved || nFrames == 0) return 0;
    size_t cols = fxInSlots_.size();
    /* §8.8 never-silent guard: track how long the host has been pushing NON-silent
     * input. A run of non-silent blocks means the input path is live and the device
     * MUST return wet; a single silent block breaks the run (a pause is not the trap,
     * so a decaying reverb tail going quiet during a pause is never a false positive).
     * Audio-thread only (sole producer), in lockstep with observeFxWet's wet side. */
    bool energy = false;
    for (size_t i = 0, n = nFrames * cols; i < n; i++)
        if (interleaved[i] > kFxSilenceEps || interleaved[i] < -kFxSilenceEps) {
            energy = true;
            break;
        }
    fxInRunFrames_ = energy ? fxInRunFrames_ + nFrames : 0;
    return fxInRing_.write(interleaved, nFrames * cols) / cols;
}

/* §8.8 audio.fx NEVER-SILENT guard — see runtime.h. Runs on the audio/process thread
 * from the FX pull side each block (pullAudio / pullAudioBlocking when fxArmed). It
 * correlates the input-energy run (writeFxInput) against the wet energy delivered here
 * and trips LOUDLY when input has been live for a full window yet the wet stayed silent
 * the whole time — the broken-input-path ("§8.8 trap") signature. READ-ONLY w.r.t. the
 * wet buffer, so the FX render (and the instrument golden, which never arms) is
 * byte-unchanged. */
void HarpRuntime::observeFxWet(const float *wet, size_t nFrames) {
    if (!fxArmed() || !wet || nFrames == 0) return;
    /* Reset on a (re)negotiation: a new stream's wet starts from priming silence, which
     * must not count against the previous run. sessionGen_ bumps at sessionUp. */
    uint64_t gen = sessionGen_.load(std::memory_order_relaxed);
    if (gen != fxWatchdogGen_) {
        fxWatchdogGen_ = gen;
        fxInRunFrames_ = 0;
        fxWetSilentRunFrames_ = 0;
        fxSilentWetEpisode_ = false;
    }
    bool wetEnergy = false;
    for (size_t i = 0, n = nFrames * 2; i < n; i++)
        if (wet[i] > kFxSilenceEps || wet[i] < -kFxSilenceEps) {
            wetEnergy = true;
            break;
        }
    if (wetEnergy) {
        /* the wet is alive — clear the silent run AND the episode latch, so a LATER
         * break (a mid-session input-path failure) re-fires its own count. */
        fxWetSilentRunFrames_ = 0;
        fxSilentWetEpisode_ = false;
        return;
    }
    /* the wet is silent THIS block. ONLY the trap — continuously-fed input with a dead
     * wet — accumulates; the instant input stops (fxInRunFrames_ == 0) the run resets,
     * so a reverb tail decaying to silence during a pause is never a false positive. */
    if (fxInRunFrames_ == 0) {
        fxWetSilentRunFrames_ = 0;
        return;
    }
    fxWetSilentRunFrames_ += nFrames;
    if (!fxSilentWetEpisode_ && fxInRunFrames_ >= fxSilentWetWindowFrames_ &&
        fxWetSilentRunFrames_ >= fxSilentWetWindowFrames_) {
        fxSilentWetEpisode_ = true; /* one count per silent episode, not per block */
        fxSilentWetFaults_.fetch_add(1, std::memory_order_relaxed);
        fxSilentWetTripped_.store(true, std::memory_order_relaxed);
        char msg[224];
        snprintf(msg, sizeof msg,
                 "armed FX wet SILENT for %llu frames while input was live — the H->D "
                 "input path is dead (device free-running / not host-paced?): §8.8 trap",
                 (unsigned long long)fxWetSilentRunFrames_);
        recordLog(HARP_LOG_ERROR, "audio.fx", msg);
        log_msg("§8.8 NEVER-SILENT: %s", msg); /* loud stderr copy */
    }
}

/* §8.7 eth RTP audio NEVER-SILENT guard — see runtime.h. The detection seam the reader()
 * eth loop runs on every receive poll (both the bit-exact and the ASRC arm). `floats` =
 * audio floats received this poll (0 = none/timeout/still-assembling), `silentMs` = the
 * transport's time since the LAST RTP packet. A live free-running stream always emits
 * packets — even a musical rest is silence-CONTENT, not packet-absence — so silentMs only
 * grows past the window on a REAL stall (RTP stopped / ASRC starved of input / device gone).
 * Trips LOUDLY then: ERROR log + the host-readable x.harp.rtp_silent counter + the sticky
 * tripped flag the offline/headless bounce fails on. NEVER fires on legitimate silence (a
 * rest keeps packets flowing → silentMs low) nor on startup (no packet yet → silentMs 0).
 * Returns true exactly when the stall is detected (the caller records the §12.1 transition,
 * drops connected_, and breaks so the supervisor reconnects). ONE count per episode: the
 * latch clears the instant packets resume, so a LATER stall re-fires its own count. */
bool HarpRuntime::rtpStallTrip(unsigned floats, unsigned silentMs) {
    if (floats > 0) {
        /* a packet landed this poll — the stream is live (incl. a silent-content rest);
         * clear the episode latch so a later stall re-counts. */
        rtpSilentEpisode_ = false;
        return false;
    }
    if (silentMs <= rtpSilentWindowMs_)
        return false; /* a brief gap / jitter / still-assembling — not a stall, and startup
                       * (no packet yet) reads silentMs 0, so it is never a false positive. */
    if (!rtpSilentEpisode_) {
        rtpSilentEpisode_ = true; /* one count per stall episode, not per poll */
        rtpSilentFaults_.fetch_add(1, std::memory_order_relaxed);
        rtpSilentTripped_.store(true, std::memory_order_relaxed);
        char msg[224];
        snprintf(msg, sizeof msg,
                 "eth RTP audio SILENT for %ums while streaming — no packets (the stream "
                 "stalled / ASRC starved / device gone): §8.7 trap", silentMs);
        recordLog(HARP_LOG_ERROR, "audio.eth", msg);
        log_msg("§8.7 NEVER-SILENT: %s", msg); /* loud stderr copy */
    }
    return true;
}
