#include "runtime.h"
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

#define LIVE_REF "live/project"
#define CREDIT_GRANT (16u << 20)
#define BUNDLE_MAGIC "harpb"

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "harp-shell: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

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

bool HarpRuntime::helloAndIdentity() {
    harp_client_identity id;
    int rc = harp_client_hello(&client_, "harp-shell 0.1 (VST3)", &id);
    if (rc != 0) {
        if (rc == HARP_CLIENT_EDEV)
            log_msg("device error on %s: %s %s", client_.err_method, client_.err_code,
                    client_.err_msg);
        return false;
    }
    vendorId_ = id.vendor_id;
    vendorName_ = id.vendor;
    productId_ = id.product_id;
    productName_ = id.product;
    serial_ = id.serial;
    engineId_ = id.engine_id;
    engineVer_ = id.engine_ver;
    paramMapHash_ = id.param_map_hash;
    deviceRateLock_ = harp_client_has_cap(&id, "audio.rate-lock"); /* §8.7: honors audio.trim */
    /* §6.4 latency-profile (key 8): cache for the §14.3 LoopbackMeasurer's expected-
     * RTT. Off the loopback path this is just stored, never read (no render effect). */
    nLat_ = 0;
    for (size_t i = 0; i < id.nlat && nLat_ < kMaxLatProfiles; i++) {
        latProfiles_[nLat_].rate = id.lat[i].rate;
        latProfiles_[nLat_].in_lat = id.lat[i].in_lat;
        latProfiles_[nLat_].out_lat = id.lat[i].out_lat;
        latProfiles_[nLat_].buf_depth = id.lat[i].buf_depth;
        nLat_++;
    }
    return true;
}

bool HarpRuntime::audioStart(uint32_t rate) {
    /* P5b: the audio.start subscription is the UNION of every instance's
     * requested output slots — the owner's outSlots_ (the main mix {0,1} by
     * default) PLUS each registered per-part sink's slots. The device streams
     * that union in ONE frame, interleaved by slot in this exact order, and
     * reader() demuxes each instance's columns out of it. With NO sink
     * registered the union is exactly outSlots_, so a single instance (or any
     * instance requesting only the main mix) sends the byte-identical {0,1}
     * request — the golden gate. computeUnionSlotsLocked resolves each sink's
     * column indices against this order under sinksMutex_. */
    {
        std::lock_guard<std::mutex> lk(sinksMutex_);
        computeUnionSlotsLocked();
    }
    /* the reader demuxes RTP packets by this width (USB carries it per-frame). Set
     * before audio.start so it is live before the device's first post-start RTP. */
    unionWidth_.store((uint16_t)unionSlots_.size(), std::memory_order_relaxed);
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client_, &req, "audio.start", true);
    if (freeRunning_) {
        /* §8.7 Ethernet: free-running (key 5 = 0) to the RTP dest port (key 6),
         * carrying the SAME slot union (key 4) the USB path sends — so the device
         * RTP-streams main {0,1} PLUS every per-part sink pair, and reader()
         * demuxes per part exactly as on USB. Default (no sink) = {0,1}, the
         * stereo main mix (bit-exact, as the proven eth-bitexact-test). */
        harp_cbor_map(&req, 6);
        harp_cbor_uint(&req, 0);
        harp_cbor_uint(&req, rate);
        harp_cbor_uint(&req, 1);
        harp_cbor_uint(&req, ethNsamples()); /* device RTP packet (HARP_ETH_NSAMPLES; default kBlock=256) */
        harp_cbor_uint(&req, 2);
        harp_cbor_uint(&req, ethTargetFrames()); /* prefill-burst depth = trim setpoint (avoids startup silence) */
        harp_cbor_uint(&req, 4); /* active-slots-out union (main + per-part pairs) */
        harp_cbor_array(&req, unionSlots_.size());
        for (uint32_t slot : unionSlots_) harp_cbor_uint(&req, slot);
        harp_cbor_uint(&req, 5);
        harp_cbor_uint(&req, 0); /* free-running */
        harp_cbor_uint(&req, 6);
        harp_cbor_uint(&req, (uint64_t)transport_->audioPort()); /* RTP dest port */
    } else {
        /* host-paced (deterministic). USB (audioPort7()==0): the proven byte-
         * identical 6-key map. §8.3-over-§8.7 Ethernet offline bounce
         * (audioPort7()>0): the SAME map PLUS key 7 = the host's TCP audio-listen
         * port the device dials back, and NO key 6 (no RTP). The device then runs
         * host_paced_loop verbatim over TCP. */
        int hpPort = transport_->audioPort7();
        harp_cbor_map(&req, hpPort ? 7 : 6);
        harp_cbor_uint(&req, 0);
        harp_cbor_uint(&req, rate);
        harp_cbor_uint(&req, 1);
        harp_cbor_uint(&req, kBlock);
        harp_cbor_uint(&req, 2);
        harp_cbor_uint(&req, kTargetDepthFrames);
        harp_cbor_uint(&req, 3);
        /* active-slots-IN (H->D): the engine has no input on the golden path, so
         * the historical declaration is [0,1] and the feeder sends h.slots=0 frames
         * (no payload) — byte-identical. §14.3: when the LoopbackMeasurer is armed,
         * declare the SINGLE chosen in-slot here so the device resolves exactly one
         * input column (d->audio.in_slots = {in}); the probe then injects on that one
         * column. This only matters once diag.loopback.start arms the device — off
         * the probe the device ignores in_slots and the render is unchanged. */
        if (loopbackArmed()) {
            harp_cbor_array(&req, 1);
            harp_cbor_uint(&req, (uint64_t)loopbackIn_);
        } else {
            harp_cbor_array(&req, 2);
            harp_cbor_uint(&req, 0);
            harp_cbor_uint(&req, 1);
        }
        harp_cbor_uint(&req, 4); /* active-slots-out: the UNION the host subscribes
                                  * to. DEFAULT (no per-part sink) = outSlots_ {0,1}
                                  * (the stereo main mix), which renders exactly as
                                  * the historical empty [] did — the golden byte-
                                  * identical default. Per-part sinks append their
                                  * pairs {2+2N,3+2N}; reader() demuxes them out. */
        harp_cbor_array(&req, unionSlots_.size());
        for (uint32_t slot : unionSlots_) harp_cbor_uint(&req, slot);
        harp_cbor_uint(&req, 5);
        harp_cbor_uint(&req, 1); /* host-paced */
        if (hpPort) { /* §8.3-over-§8.7: ascending key order, after key 5 */
            harp_cbor_uint(&req, 7);
            harp_cbor_uint(&req, (uint64_t)hpPort);
        }
    }
    harp_env e;
    bool ok = request(&req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return ok;
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

/* P5b RE-NEGOTIATION on the feeder/control thread (under ctlMutex_). See the
 * header for the full contract. The §8.6 transition is audio.stop -> (new
 * union) -> audio.start, on a CONTINUOUS SSI domain and a fresh fence epoch,
 * WITHOUT a session teardown.
 *
 * NO RECONNECT / NO STATE RESET (B1). The initial audio.start is safe to pair
 * with sessionUp's RT-state reset because no audio thread is live yet; a
 * MID-SESSION re-neg is NOT. So we never go through sessionDown/sessionUp here:
 * connected_ stays true the whole time, and ssi_/ssiRead_/framesSent_/
 * framesRecv_/framesRecvAtomic_/padDebtFloats_ and the audio + sink rings — all
 * owned by the live reader/audio threads — are left UNTOUCHED. The SSI domain is
 * continuous: the device renders whatever range each pacing frame names. The
 * reader observes the restart as old-width frames followed by new-width frames,
 * each self-describing (h.slots), and adapts per frame.
 *
 * RELIABLE STOP/START (B3). We QUIESCE the reader (readerStop_ + join) so the
 * audio-IN endpoint has a SINGLE owner across the stop/start, then audio.stop ->
 * drain the stream tail (sole reader, exactly as sessionDown does) so the device's
 * audio writes never block its single-threaded session loop while the slow
 * instrumented control round-trip is in flight -> audio.start. With the tail
 * drained the round-trip completes instead of timing the link out (the old
 * intermittent "audio.start failed"). Then respawn the reader on the new union.
 *
 * FENCE EPOCH (§8.3.1, B2). evtEpochBase_ = evtQueuedSeq_.load() — a single store
 * under ctlMutex_, no reset of the monotonic counter, so it never races queue*'s
 * lock-free fetch_add. The device's audio.start wipes g_evt_consumed to 0; the
 * host's per-frame fence becomes (evtQueuedSeq_ - evtEpochBase_), which also
 * restarts at 0 for events queued AFTER this point. Straddling events (queued
 * pre-reneg, written post-reset) are below the baseline -> they under-count ->
 * §8.3.1 fence is a minimum -> at worst evt_late, never a wedge. */
void HarpRuntime::audioRenegotiateLocked() {
    /* 1. quiesce the reader so we own the audio-IN endpoint for the stop/start.
     * connected_ stays TRUE — this is NOT a teardown, just a reader handoff. */
    readerStop_.store(true, std::memory_order_release);
    if (readerThread_.joinable()) readerThread_.join();
    readerStop_.store(false, std::memory_order_release);

    /* 2. audio.stop -> drain the stream tail (sole owner now) -> audio.start with
     * the NEW union. audioStart() takes sinksMutex_ and calls computeUnionSlotsLocked,
     * which rebuilds unionSlots_ AND re-resolves every sink's `cols` against the new
     * order, so the late sink's columns are correct the moment the wider frames
     * arrive. Draining with no concurrent reader is what makes audio.start reliable
     * under the instrumented host (the device's session loop is never write-blocked). */
    audioStopLocked();
    /* §12.1: intra-STREAMING re-negotiation — record the audio.stop leg (the
     * temporary stop) keeping the STREAMING state; the audio.start leg below
     * pairs with it. Control-path (feeder thread, under ctlMutex_). */
    recordTransition(HARP_ST_STREAMING, HARP_ST_STREAMING, HARP_TR_AUDIO_STOP,
                     "re-negotiation: audio.stop");
    if (transport_) {
        uint8_t junk[16384];
        int quiet = 0;
        while (quiet < 2) {
            int r = transport_->audioRead(junk, sizeof junk, 80);
            if (r < 0) break;
            quiet = (r == 0) ? quiet + 1 : 0;
        }
    }

    /* 3. fence epoch baseline (monotonic counter untouched — no race with queue*) */
    evtEpochBase_.store(evtQueuedSeq_.load(std::memory_order_acquire),
                        std::memory_order_release);

    bool ok = audioStart(rate_);

    /* 4. respawn the reader on the new union (SSI continuous — no reset). Even if
     * audio.start failed, respawn so a subsequent device recovery still streams;
     * a true transport death surfaces as the reader's r<0 -> connected_=false ->
     * the supervisor reconnects (a real sessionUp, with no live race because that
     * path tears down first). */
    readerThread_ = std::thread([this] { reader(); });

    if (!ok) {
        log_msg("re-negotiation: audio.start failed (sink reads silence until it recovers)");
        recordTransition(HARP_ST_STREAMING, HARP_ST_STREAMING, HARP_TR_AUDIO_START,
                         "re-negotiation: audio.start failed");
        recordLog(HARP_LOG_WARN, "reneg", "re-negotiation: audio.start failed");
    } else {
        renegCount_.fetch_add(1, std::memory_order_release);
        log_msg("re-negotiated audio stream: %zu union slot(s) now streamed",
                unionSlots_.size());
        char d[96];
        snprintf(d, sizeof d, "re-negotiated: %zu union slot(s) streamed", unionSlots_.size());
        recordTransition(HARP_ST_STREAMING, HARP_ST_STREAMING, HARP_TR_AUDIO_START, d);
        recordLog(HARP_LOG_INFO, "reneg", d);
    }
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
             * satisfied exactly as on a normal frame (no event is left pending). */
            uint32_t hw = evtQueuedSeq_.load(std::memory_order_acquire);
            uint32_t base = evtEpochBase_.load(std::memory_order_acquire);
            uint32_t seq = hw > base ? hw - base : 0;
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

/* Param set as a §9.4 event message: fire-and-forget, no response.
 * ts is an SSI (0 = "now"). Encode-only; the feeder frames and batches. */
void HarpRuntime::encodeParamEvent(harp_cbuf *m, uint32_t id, float v, uint64_t ts,
                                   uint8_t channel) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 1); /* etype: param */
    harp_cbor_map(m, channel ? 3 : 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, v);
    if (channel) { /* key 5 = multitimbral part (§9.4); omitted for part 0 */
        harp_cbor_uint(m, 5);
        harp_cbor_uint(m, channel);
    }
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

/* ---------------- lifecycle ---------------- */

/* The device-selection policy (see header). The mutual exclusion is the
 * USB claim inside harp_usb_open_match: a device owned by another plugin
 * instance fails the claim and the scan advances, so two fresh instances
 * land on different units without any coordination here. */
/* Wrap a freshly claimed USB transport (or a failed claim) as a ShellTransport.
 * Step 1 only ever builds UsbTransport here; the Ethernet binding is selected
 * elsewhere (next step). nullptr in => nullptr out (no device was claimed). */
static ShellTransport *wrapUsb(harp_io *io) { return io ? new UsbTransport(io) : nullptr; }

/* §8.3-over-§8.7 mid-stream live<->offline toggle. The shell calls this from its
 * offline-render hook (a host/main thread — never the audio thread). On a genuine mode
 * change on a LIVE Ethernet session, arm a re-dial and BLOCK (bounded) until the new-mode
 * session is up, so the host's next process()->pull is deterministic host-paced (not the
 * stale free-running ring). Pre-start / no-session / USB are early no-ops. */
void HarpRuntime::setOffline(bool o) {
    bool prev = wantHostPaced_.exchange(o, std::memory_order_release);
    if (prev == o) return;                                          /* idempotent: no change */
    if (!running_.load(std::memory_order_acquire)) return;          /* pre-start: first dial reads it */
    if (!connected_.load(std::memory_order_acquire)) return;        /* no live session: next sessionUp reads it */
    const char *e = getenv("HARP_ETH_DEVICE");
    if (!(e && e[0])) return;                                       /* USB: host-paced always -> no-op */
    /* freeRunning_!=o means the live session is ALREADY in the requested mode (want
     * offline o=true <-> host-paced freeRunning_=false): nothing to re-dial. */
    if (freeRunning_.load(std::memory_order_acquire) != o) return;
    /* Genuine flip on a live eth session. Publish the absolute target BEFORE the sticky
     * flag; the supervisor re-dials, sessionUp bumps sessionGen_ to the target + clears
     * the flag, and the pull fence releases on sessionGen_>=flipTargetGen_. */
    uint64_t g0 = sessionGen_.load(std::memory_order_acquire);
    flipTargetGen_.store(g0 + 1, std::memory_order_release);
    modeFlipPending_.store(true, std::memory_order_release);
    /* Bounded host-thread wait (~2s covers a slow RPi/KR260 connect-back); wakes on stop.
     * Done when a new session is up (gen advanced) AND it is the requested mode. */
    for (int i = 0; i < 4000; i++) {
        if (!running_.load(std::memory_order_acquire)) break;
        if (sessionGen_.load(std::memory_order_acquire) >= g0 + 1 &&
            freeRunning_.load(std::memory_order_acquire) != o)
            break;
        harp_sleep_ns(500000ull); /* 0.5 ms */
    }
}

ShellTransport *HarpRuntime::selectDevice() {
    /* Ethernet binding (§8.7): HARP_ETH_DEVICE=HOST:PORT routes to the RTP/TCP
     * transport instead of USB. Unset (the default, and every golden run) falls
     * straight through to the USB path below — byte-identical. */
    if (const char *eth = getenv("HARP_ETH_DEVICE"))
        if (eth[0]) /* host-paced (deterministic) when the DAW is rendering offline; else free-running RTP */
            return EthTransport::dial(eth, wantHostPaced_.load(std::memory_order_relaxed));

    /* reconnect: pinned to the exact unit this instance already owns — the
     * same-model fallback must NOT fire here, or a replug could let this
     * instance steal a sibling track's device. */
    if (!boundSerial_.empty())
        return wrapUsb(harp_usb_open_match_ctx(usbCtx_, boundSerial_.c_str(), false, 0, 0));

    /* first bind: what does the loaded project want? */
    std::string wantSerial;
    bool wantModel = false;
    uint16_t wvid = 0, wpid = 0;
    {
        std::lock_guard<std::mutex> blk(bundleMutex_);
        if (wantUsb_) {
            wantSerial = wantUsbSerial_;
            wantModel = true;
            wvid = wantUsbVid_;
            wpid = wantUsbPid_;
        }
    }
    /* test/field override: force a specific unit regardless of the bundle */
    if (const char *env = getenv("HARP_DEVICE_SERIAL"))
        if (env[0]) {
            wantSerial = env;
            wantModel = false; /* exact-or-nothing when explicitly forced */
        }

    if (!wantSerial.empty()) {
        harp_io *io = harp_usb_open_match_ctx(usbCtx_, wantSerial.c_str(), false, 0, 0); /* exact */
        if (!io && wantModel) /* serial gone: first unclaimed of the SAME model */
            io = harp_usb_open_match_ctx(usbCtx_, nullptr, true, wvid, wpid);
        return wrapUsb(io); /* a known model is never satisfied by a different model */
    }
    /* fresh instance (or a bundle predating usb-identity), no env pin: a multi-board
     * desk can name a DEFAULT unit without an env var — the serial in
     * ~/.config/harp/device. Tried as a preference (exact match); if it is not on the
     * bus, fall through so a single-board setup still just works. The bundle's own
     * serial and HARP_DEVICE_SERIAL both take precedence (handled above). */
    if (const char *home = getenv("HOME")) {
        char pref[64] = {0};
        std::string path = std::string(home) + "/.config/harp/device";
        if (FILE *f = fopen(path.c_str(), "r")) {
            if (fgets(pref, sizeof pref, f)) pref[strcspn(pref, "\r\n \t")] = 0; /* trim */
            fclose(f);
        }
        if (pref[0])
            if (harp_io *io = harp_usb_open_match_ctx(usbCtx_, pref, false, 0, 0))
                return wrapUsb(io);
    }
    /* else: first unclaimed HARP device of any model — adopts whatever is there and
     * records it on first save. */
    return wrapUsb(harp_usb_open_match_ctx(usbCtx_, nullptr, false, 0, 0));
}

/* One connection attempt: claim, hello, re-assert the project bundle,
 * start the stream, spawn the reader. Caller: start() or supervisor(). */
bool HarpRuntime::sessionUp() {
    transport_ = selectDevice();
    if (!transport_) return false;
    if (!transport_->hasAudio()) {
        delete transport_;
        transport_ = nullptr;
        return false;
    }
    /* cache the binding mode once, off the RT path (review m2). USB => false,
     * so every host-paced branch below is reached exactly as before. */
    freeRunning_ = transport_->isFreeRunning();
    {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        /* capture the bound device's USB identity (vid:pid:serial) UNDER
         * ctlMutex_ — getStateBundle reads it there on the main thread, so
         * an unlocked write here would be a cross-thread race on the
         * std::string (the project holds itself to zero benign races). */
        harp_usb_devinfo di;
        if (transport_->identity(&di)) {
            usbVid_ = di.vendor_id;
            usbPid_ = di.product_id;
            usbSerial_ = di.serial;
            if (boundSerial_.empty()) boundSerial_ = di.serial; /* pin for reconnect */
        }
        /* fresh per session: rid space, credit, AND the link reassembly
         * state — a half-assembled frame from a dead session must not
         * poison the next one */
        harp_link_free(&link_);
        harp_link_init(&link_);
        harp_client_free(&client_);
        harp_client_init(&client_, transport_->ctlIo(), &link_, storeOk_ ? &store_ : nullptr,
                         nullptr, nullptr);
        if (!helloAndIdentity()) {
            log_msg("hello failed");
            harp_client_free(&client_);
            delete transport_;
            transport_ = nullptr;
            return false;
        }
        log_msg("connected: %s %s (serial %s, engine %s %s)", vendorName_.c_str(),
                productName_.c_str(), serial_.c_str(), engineId_.c_str(),
                engineVer_.c_str());
        /* §12.1: hello-ok — the device identified, the session is ATTACHED. */
        {
            char d[256];
            snprintf(d, sizeof d, "%s %s (serial %s, engine %s %s)", vendorName_.c_str(),
                     productName_.c_str(), serial_.c_str(), engineId_.c_str(),
                     engineVer_.c_str());
            recordTransition(HARP_ST_DETACHED, HARP_ST_ATTACHED, HARP_TR_HELLO_OK, d);
            recordLog(HARP_LOG_INFO, "session", d);
        }
        /* §8.7 clock mode (auto-select): a free-running (Ethernet/RTP) device that
         * advertises audio.rate-lock honors our audio.trim, so the feeder closes the
         * rate loop and pullAudio plays 1:1 = bit-exact. One that does NOT must be
         * ASRC-resampled host-side. USB is host-paced (bitExact_ unused there). */
        bitExact_ = !freeRunning_ || deviceRateLock_;
        if (freeRunning_ && !deviceRateLock_)
            log_msg("warning: Ethernet device lacks audio.rate-lock -> ASRC resample "
                    "(host-locked bit-exact unavailable)");
        /* re-assert the project's bundle ("Live wins") — covers both a
         * setState that arrived pre-connect and state the device grew
         * while unplugged. Copy the target out: pushStateLocked takes
         * bundleMutex_ itself. */
        bool haveBundle = false;
        harp_hash target{};
        {
            std::lock_guard<std::mutex> blk(bundleMutex_);
            haveBundle = hasBundle_;
            target = bundleTarget_;
        }
        if (haveBundle) {
            if (pushStateLocked(target)) {
                log_msg("project state re-asserted");
                recordLog(HARP_LOG_INFO, "recall", "project state re-asserted");
            } else {
                log_msg("project state apply failed (will retry on reconnect)");
                recordLog(HARP_LOG_WARN, "recall",
                          "project state apply failed (will retry on reconnect)");
            }
        }
        if (!audioStart(rate_)) {
            log_msg("audio.start failed");
            /* §12.1: ATTACHED but the stream never came up -> back to DETACHED. */
            recordTransition(HARP_ST_ATTACHED, HARP_ST_DETACHED, HARP_TR_AUDIO_START,
                             "audio.start failed");
            recordLog(HARP_LOG_ERROR, "audio.start", "audio.start failed");
            harp_client_free(&client_);
            delete transport_;
            transport_ = nullptr;
            return false;
        }
        /* §12.1: audio.start accepted -> SYNCED (stream negotiated, clock locked). */
        recordTransition(HARP_ST_ATTACHED, HARP_ST_SYNCED, HARP_TR_AUDIO_START,
                         bitExact_ ? "audio.start ok (bit-exact)" : "audio.start ok (ASRC)");
        recordLog(HARP_LOG_INFO, "audio.start", "audio stream negotiated");
        /* §14.3 LoopbackMeasurer SAFETY (review MINOR). A prior probe that crashed /
         * was killed mid-measurement (or a device that survived a host restart) could
         * leave the device's loopback_on engaged — it would then keep overwriting the
         * out-slot column with stale H->D input and silently corrupt this fresh
         * session's render. The host never persists loopback_on, so it is ALWAYS off
         * for a clean session; assert that by sending an unconditional, idempotent
         * diag.loopback.stop here (cheap, off the render path, under ctlMutex_) so the
         * device is guaranteed disarmed before any audio flows. The device treats a
         * stop with no active loop as a no-op, so this is harmless when nothing was
         * armed. Logged so a stray engaged loop is visible in the session log. */
        {
            harp_cbuf lreq, lrsp;
            harp_cbuf_init(&lreq);
            harp_cbuf_init(&lrsp);
            harp_client_req_head(&client_, &lreq, "diag.loopback.stop", false);
            harp_env le = {};
            request(&lreq, &lrsp, &le);
            harp_cbuf_free(&lreq);
            harp_cbuf_free(&lrsp);
            log_msg("§14.3 loopback safety: cleared device loopback_on (=false) at session start");
        }
    }
    /* drain any stale stream bytes before pacing */
    uint8_t junk[16384];
    while (transport_->audioRead(junk, sizeof junk, 30) > 0) {}

    /* new session = new stream = new SSI time domain (§7.1). Events still
     * queued from the previous session carry STALE timestamps — drain EVERY
     * source (no pump is running yet, so consuming here is safe; the lock
     * guards against an attached instance registering/removing concurrently),
     * and the fence sequence space restarts from zero on both sides. */
    {
        std::lock_guard<std::mutex> lk(sourcesMutex_);
        for (size_t i = 0; i < nSources_; i++) {
            TimedEv stale;
            while (sources_[i]->ring.pop(stale)) {}
        }
    }
    evtQueuedSeq_.store(0, std::memory_order_release);
    evtEpochBase_.store(0, std::memory_order_release); /* fresh fence epoch == 0 */
    ssi_ = framesSent_ = framesRecv_ = 0;
    framesRecvAtomic_.store(0, std::memory_order_relaxed);
    ssiRead_.store(0, std::memory_order_relaxed);
    padDebtFloats_ = 0;
    /* §14.4 host-context-C: reset the clock-stats snapshot for the new session
     * (trimCount_/lastTrimPpb_ are per-session, like framesSent_; asrcLive_ flips
     * true only when the ASRC reader branch runs). Off the render path. */
    lastTrimPpb_.store(0, std::memory_order_relaxed);
    trimCount_.store(0, std::memory_order_relaxed);
    asrcLive_.store(false, std::memory_order_relaxed);
    ahead_ = 2; /* small fixed pipeline; the reader thread keeps RTT short */
    audioRing_.clear();
    /* P5b: clear every per-part sink's ring + pad debt for the new SSI domain,
     * exactly as audioRing_/padDebtFloats_ above. No reader runs yet (spawned
     * below), and the lock guards against a sink register/unregister racing. */
    {
        std::lock_guard<std::mutex> lk(sinksMutex_);
        for (size_t i = 0; i < nSinks_; i++) {
            sinks_[i]->ring.clear();
            sinks_[i]->padDebt = 0;
        }
    }
    /* connected_ goes true BEFORE the pump spawns: its run loop gates on
     * it, and spawning first would race a clean instant exit */
    connected_.store(true, std::memory_order_release);
    /* §8.3-over-§8.7 mid-stream toggle: publish the new session generation, then clear
     * any pending mode flip (the clear is LAST, after the gen bump, so an offline pull
     * that already observed modeFlipPending_=true still releases via the absolute
     * sessionGen_>=flipTargetGen_ test). EVERY sessionUp re-reads wantHostPaced_, so the
     * session is always in the latest requested mode — even a coincidental reconnect
     * satisfies a pending flip. */
    sessionGen_.fetch_add(1, std::memory_order_release);
    modeFlipPending_.store(false, std::memory_order_release);
    /* The reader thread feeds audioRing_ for BOTH bindings: USB drains the
     * host-paced audio-IN endpoint (+ demux); Ethernet receives the RTP stream
     * 1:1 (reader() branches on freeRunning_). Spawning it here — and joining it
     * in sessionDown BEFORE transport_ is freed — is what keeps the DAW audio
     * thread (which touches only audioRing_) clear of transport_ teardown. */
    readerThread_ = std::thread([this] { reader(); });
    eventPumpThread_ = std::thread([this] { eventPump(); });
    /* §12.1: reader + event pump are live, connected_ is true -> STREAMING. */
    recordTransition(HARP_ST_SYNCED, HARP_ST_STREAMING, HARP_TR_AUDIO_START,
                     "reader + event pump up; streaming");
    recordLog(HARP_LOG_INFO, "session", "streaming");
    return true;
}

/* Tear a session down: reap the reader, orderly audio.stop if the device
 * is still talking to us, release the claim. Safe on a dead transport. */
void HarpRuntime::sessionDown() {
    bool wasConnected = connected_.exchange(false, std::memory_order_acq_rel);
    /* §12.1: orderly detach. Record the transition off the audio path (this runs
     * on the supervisor thread). A device-gone teardown reaches here too, but the
     * transport-error transition was already filed by the reader (below) — this
     * detach record marks the lifecycle close either way. */
    if (wasConnected)
        recordTransition(HARP_ST_STREAMING, HARP_ST_DETACHED, HARP_TR_DETACH,
                         "session torn down");
    /* QUIESCE the reader before joining it. On a mid-stream live<->offline flip the
     * transport is STILL ALIVE, so the free-running reader's recvAudio keeps returning
     * data and its loop (running_ && !readerStop_) never exits on its own — connected_
     * =false does NOT stop it (the reader is the thread that SETS connected_=false on
     * device-gone, so by design it can't gate on connected_; the eventPump DOES gate on
     * connected_, so its join below already returns). Without this, join() hangs forever
     * on a flip and the supervisor never re-dials -> the pullAudioBlocking flip-fence
     * nanosleeps forever. Mirror audioRenegotiateLocked's quiesce: readerStop_ makes the
     * reader exit within one recvAudio timeout (<=100 ms); reset it so the next sessionUp
     * spawns a clean reader. On a dead-transport RECONNECT the reader already self-exits
     * (silentMs>1s) — this just makes the reap prompt and explicit, never relying on it. */
    readerStop_.store(true, std::memory_order_release);
    if (readerThread_.joinable()) readerThread_.join();
    readerStop_.store(false, std::memory_order_release);
    if (eventPumpThread_.joinable()) eventPumpThread_.join();
    if (!transport_) return;
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (wasConnected) {
        audioStopLocked();
        /* drain the tail of the stream so the device thread can park */
        uint8_t junk[16384];
        int quiet = 0;
        while (quiet < 2) {
            int r = transport_->audioRead(junk, sizeof junk, 80);
            if (r < 0) break;
            quiet = (r == 0) ? quiet + 1 : 0;
        }
    }
    harp_client_free(&client_);
    delete transport_; /* UsbTransport::~ closes the claim; the libusb ctx survives */
    transport_ = nullptr;
}

/* The supervisor owns the session for the plugin's whole active life:
 * run the feeder while connected, reconnect (1 s cadence) when the
 * transport dies or no device was present at start. Replug recovery is
 * just the connect path again — same hello, same bundle re-assert. */
void HarpRuntime::supervisor() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    bool everConnected = connected_.load(std::memory_order_acquire);
    while (running_.load(std::memory_order_acquire)) {
        /* §8.3-over-§8.7 mid-stream toggle: a host flipped offline<->live on a LIVE
         * session, so the feeder returned with connected_ still true and modeFlipPending_
         * set. Re-dial in the new mode using the EXISTING teardown+bring-up (sessionUp
         * re-reads wantHostPaced_, bumps sessionGen_, clears the flag) — no bespoke
         * lifecycle path, so the UAF-safe join-before-delete invariant is reused verbatim. */
        if (connected_.load(std::memory_order_acquire) &&
            modeFlipPending_.load(std::memory_order_acquire)) {
            sessionDown();
            if (!running_.load(std::memory_order_acquire)) break;
            if (sessionUp()) log_msg("audio mode re-dialed (live<->offline)");
            continue;
        }
        if (connected_.load(std::memory_order_acquire)) {
            everConnected = true;
            feeder(); /* returns when !running_, the transport died, or a mode flip is pending */
            continue;
        }
        sessionDown(); /* reap whatever is left of the dead session */
        if (!running_.load(std::memory_order_acquire)) break;
        if (sessionUp()) {
            /* a reconnect is news; the very first attach already logged */
            if (everConnected) log_msg("device reconnected; stream re-established");
            else log_msg("device attached; stream established");
            continue;
        }
        for (int i = 0; i < 10 && running_.load(std::memory_order_acquire); i++) {
            harp_sleep_ns(100000000ull); /* 100 ms x10 = 1 s total, stop-responsive */
        }
    }
    sessionDown();
}

bool HarpRuntime::start(uint32_t sampleRate) {
    if (running_.load(std::memory_order_acquire)) return connected();
    harp_plat_init(); /* hi-res timers for the sub-ms pacing/idle waits (Windows) */
#ifdef _WIN32
    /* Winsock MUST be started before any getaddrinfo/socket (the §8.7 dial) — without
     * it harp_sock_dial fails "cannot resolve" and the host never connects, silently
     * rendering silence. The host process owns WSAStartup (sock_io.h); the runtime is
     * that owner. Process-once (refcounted; we never WSACleanup — process exit reclaims). */
    static std::once_flag harp_wsa_once;
    std::call_once(harp_wsa_once, [] {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            fprintf(stderr, "harp-shell: WSAStartup failed — §8.7 networking unavailable\n");
    });
#endif
    rate_ = sampleRate;
    /* test/field override: HARP_OUT_SLOTS="a,b,..." forces the active-slots-out
     * subscription regardless of any setOutSlots() call. This is how an
     * out-of-process host (harp-vst3-host --part N) reaches the runtime across
     * the plugin boundary — it sets the env, exactly as HARP_DEVICE_SERIAL pins
     * the device. UNSET (the default) leaves outSlots_ = {0,1}, the golden
     * main-mix request. */
    if (const char *e = getenv("HARP_OUT_SLOTS"))
        if (e[0]) {
            std::vector<uint32_t> slots;
            for (const char *p = e; *p;) {
                char *end = nullptr;
                unsigned long v = strtoul(p, &end, 10);
                if (end == p) break; /* no digits: stop at the garbage */
                slots.push_back((uint32_t)v);
                p = (*end == ',') ? end + 1 : end;
            }
            setOutSlots(slots); /* no-op if it parsed to nothing */
        }
    /* test/field override: HARP_CHANNEL=k pins the multitimbral part this
     * instance's PARAM events carry (key 5). The out-of-process host
     * (harp-vst3-host --channel) reaches the in-plugin runtime through it,
     * exactly as HARP_OUT_SLOTS carries --part. UNSET (the default) leaves
     * ownerSource_.chan = 0 => the key is omitted => byte-identical golden wire. */
    if (const char *e = getenv("HARP_CHANNEL"))
        if (e[0]) {
            int v = atoi(e);
            if (v >= 0 && v <= 15) setChannel((uint8_t)v);
        }
    /* §14.3 LoopbackMeasurer arming: the out-of-process host's --loopback flag sets
     * HARP_LOOPBACK_IN / HARP_LOOPBACK_OUT (mirroring HARP_DIAG_BUNDLE_OUT). Read
     * BEFORE the first sessionUp so audioStart declares the in-slot in key 3. UNSET
     * (the default) leaves loopbackIn_/Out_ = -1 => the byte-identical golden wire. */
    if (const char *ein = getenv("HARP_LOOPBACK_IN"))
        if (const char *eout = getenv("HARP_LOOPBACK_OUT"); eout && ein[0] && eout[0]) {
            int in = atoi(ein), out = atoi(eout);
            if (in >= 0 && in <= 33 && out >= 0 && out <= 33) setLoopbackSlots(in, out);
        }
    running_.store(true);
    /* One libusb context for the whole active life — every connect attempt
     * (incl. the device-less retry loop) borrows it, so we never churn
     * libusb_init/exit. Created before the first sessionUp() and the
     * supervisor spawn so both use it. */
    if (!usbCtx_) usbCtx_ = harp_usb_ctx_create();
    bool now = sessionUp(); /* fast path: report a present device immediately */
    if (!now) log_msg("no HARP device on the bus; supervising for hot-plug");
    supervisorThread_ = std::thread([this] { supervisor(); });
    return now;
}

void HarpRuntime::stop() {
    /* Flush in-flight events before teardown: the DAW's final note-offs
     * arrive in the last process() blocks, and killing the feeder with
     * them still queued is how notes get stuck. Bounded wait — across ALL
     * sources, so a sibling part's tail note-offs flush too (P5). */
    if (running_.load(std::memory_order_acquire) && connected()) {
        for (int i = 0; i < 100; i++) {
            bool allEmpty = true;
            {
                std::lock_guard<std::mutex> lk(sourcesMutex_);
                for (size_t s = 0; s < nSources_; s++)
                    if (!sources_[s]->ring.empty()) {
                        allEmpty = false;
                        break;
                    }
            }
            if (allEmpty) break;
            harp_sleep_ns(1000000ull); /* 1 ms */
        }
    }
    if (!running_.exchange(false)) return;
    if (supervisorThread_.joinable()) supervisorThread_.join(); /* final sessionDown */
    /* Supervisor is joined: no thread can touch the context now. Tear it down
     * here, while ~HarpRuntime still runs — i.e. before the DLL can unload, so
     * no libusb backend thread outlives our module. */
    harp_usb_ctx_destroy(usbCtx_);
    usbCtx_ = nullptr;
    log_msg("stopped (underruns: %llu, padded samples: %llu)",
            (unsigned long long)underruns_.load(std::memory_order_relaxed),
            (unsigned long long)padSamples_.load(std::memory_order_relaxed));
}

/* ---------------- audio thread side ---------------- */

/* Each queue* pushes to the CALLER'S source ring (its SPSC producer side) and
 * bumps the SHARED per-session fence (evtQueuedSeq_): the device must consume
 * the TOTAL across all sources before rendering a fenced range, so the fence
 * counts every source's events, not just one's.
 *
 * A null source means the instance is EVENT-DORMANT: registerSource() returned
 * nullptr because the device's 16 parts are all taken (a 17th alias). We MUST
 * NOT fall back to the owner source — that would make this instance a SECOND
 * producer on the owner's ring, breaking the SPSC invariant the whole merge
 * rests on. A 17th part legitimately contributes nothing to a 16-part device,
 * so the event is simply dropped (logged once via dormantSrcLogged_). */
void HarpRuntime::queueParamSet(EventSource *src, uint32_t id, float v, uint64_t ts) {
    if (!src) return noteDormant();
    if (src->ring.push({0, id, v, ts, 0}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueRamp(EventSource *src, uint32_t id, float target, uint64_t start,
                            uint64_t end) {
    if (!src) return noteDormant();
    if (src->ring.push({1, id, target, start, end}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueNote(EventSource *src, uint32_t word, uint64_t ts) {
    if (!src) return noteDormant();
    if (src->ring.push({2, word, 0.0f, ts, 0})) {
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    } else {
        evDrops_.fetch_add(1, std::memory_order_relaxed);
        /* A dropped note-ON is a missing note; a dropped note-OFF is a
         * note that never stops. If anything but a note-on is lost,
         * escalate to all-notes-off — silence beats a stuck drone. */
        uint32_t status = (word >> 20) & 0xf, vel = word & 0x7f;
        bool isNoteOn = status == 0x9 && vel > 0;
        if (!isNoteOn) panicPending_.store(true, std::memory_order_release);
    }
}

void HarpRuntime::queueMod(EventSource *src, uint32_t id, float offset,
                           uint32_t voice, uint64_t ts) {
    if (!src) return noteDormant();
    /* kind 4 = mod; the §9.5 voice key rides in `end` (it is a packed uint, not
     * a timestamp). A dropped mod is benign — it leaves the base value as-is, no
     * stuck state — so unlike a note we do not escalate to panic on overflow. */
    if (src->ring.push({4, id, offset, ts, voice}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}

void HarpRuntime::queueTransport(EventSource *src, uint32_t flags, double tempo,
                                 double ppq, uint64_t ts) {
    /* Transport is GLOBAL (no part): force it onto the OWNER source whatever
     * `src` is, so a multitimbral group emits ONE transport stream — the
     * owner's is canonical — instead of N identical copies racing on the wire.
     * (feedTransport's change-detection already runs only on the owner.) */
    (void)src;
    uint64_t ppqBits;
    memcpy(&ppqBits, &ppq, sizeof ppqBits);
    if (ownerSource_.ring.push({3, flags, (float)tempo, ts, ppqBits}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}

/* ---- source registry (P5; off the audio path) ---- */

/* Register an attached instance's source. Allocates an EventSource for `channel`
 * and adds it to the array under the lock; the eventPump's next pass drains it.
 * The owner source is slot 0 and never registered here. Returns nullptr if the
 * table is full (kMaxSources == the device's 16 parts): the caller (queue* via
 * a null source) then DROPS that instance's events — it is event-dormant. We do
 * NOT fall back to the owner source: a 17th instance pushing to the owner's ring
 * would make it multi-producer and break the SPSC invariant, and a 17th part
 * legitimately contributes nothing to a 16-part device anyway. */
EventSource *HarpRuntime::registerSource(uint8_t channel) {
    EventSource *src = new EventSource(channel);
    std::lock_guard<std::mutex> lk(sourcesMutex_);
    if (nSources_ >= kMaxSources) {
        delete src;
        return nullptr;
    }
    sources_[nSources_++] = src;
    return src;
}

/* Remove an attached source and free it, keeping the event fence CONSISTENT.
 *
 * SAFE-FREE: a source's ring is only ever READ (popped) by the eventPump, and
 * only while it holds sourcesMutex_ (the pump drains every ring into its batch
 * UNDER the lock, then writes the wire AFTER unlocking — see eventPump). We
 * take that SAME lock and remove the source from the array FIRST: from that
 * point the pump's next pass can no longer see it, so unregisterSource is its
 * SOLE accessor (the producer is quiescent — the host stops process() before
 * setActive(false)/release). It can then drain and free it safely.
 *
 * FENCE CONSISTENCY: evtQueuedSeq_ is the per-session high-water mark of events
 * QUEUED (every queue* fetch_add's it; the device must consume that many evt
 * messages before rendering a fenced range). Any events left UNWRITTEN in this
 * source's ring at release were counted into the fence but will never reach the
 * wire — so without correction the device would consume total-K < fence and
 * EVERY later fenced frame would hit the §8.3.1 bounded timeout (evt_late /
 * fence_timeouts climbing for ALL surviving parts). We drop those K in-flight
 * events ON PURPOSE — the part is gone — but fetch_sub(K) so the fence drops to
 * exactly what was written == what the device will consume, leaving SURVIVING
 * parts' timing tight.
 *
 * The owner source and nullptr are no-ops (the owner persists for the session,
 * and its ring is drained normally by the pump). */
void HarpRuntime::unregisterSource(EventSource *src) {
    if (!src || src == &ownerSource_) return;
    std::lock_guard<std::mutex> lk(sourcesMutex_);
    for (size_t i = 0; i < nSources_; i++) {
        if (sources_[i] == src) {
            sources_[i] = sources_[nSources_ - 1]; /* compact: last fills the hole */
            sources_[--nSources_] = nullptr;
            /* removed from the registry FIRST -> the pump can't touch it now;
             * we are its sole owner. Drain the leftover (queued-but-unwritten)
             * events, dropping them but decrementing the fence by exactly that
             * count so the device's consume target matches what was written. */
            uint32_t leftover = 0;
            TimedEv te;
            while (src->ring.pop(te)) leftover++;
            if (leftover) evtQueuedSeq_.fetch_sub(leftover, std::memory_order_release);
            delete src;
            return;
        }
    }
}

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

/* Remove + free a sink on release, keeping reader() safe. SAFE-FREE mirrors
 * unregisterSource: a sink's ring is only WRITTEN by reader(), and only while it
 * holds sinksMutex_ (it demuxes every frame into every registered sink under the
 * lock — see reader). We take that SAME lock and remove the sink from the array
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

void HarpRuntime::feedTransport(bool playing, bool tempoValid, double tempo,
                                bool posValid, double ppq, uint32_t blockSamples,
                                uint64_t base) {
    bool discont = false;
    if (playing && tpLastPlaying_ && posValid)
        /* half a MIDI tick of slack: anything bigger is a jump */
        discont = ppq + 1e-3 < tpLastEndPpq_ || ppq > tpLastEndPpq_ + 1e-3;
    tpSamplesSince_ += blockSamples;
    bool change =
        playing != tpLastPlaying_ || (tempoValid && tempo != tpLastTempo_) || discont;
    bool refresh = playing && tpSamplesSince_ >= rate_;
    if (change || refresh || !tpSent_) {
        uint32_t flags =
            (playing ? 1u : 0) | (tempoValid ? 1u << 3 : 0) | (posValid ? 1u << 5 : 0);
        /* feedTransport runs only on the OWNER (transport-change detection
         * state is owner-audio-thread-owned); transport is global, so push it
         * on the owner source — queueTransport pins it there regardless. */
        queueTransport(&ownerSource_, flags, tempo, ppq, base);
        tpSent_ = true;
        tpSamplesSince_ = 0;
    }
    tpLastPlaying_ = playing;
    tpLastTempo_ = tempo;
    tpLastEndPpq_ = playing && tempoValid && posValid
                        ? ppq + blockSamples * tempo / (60.0 * rate_)
                        : ppq;
}

/* transport event (§9.7): etype 7, body {0 flags, 1 tempo, 4 ppq} */
void HarpRuntime::encodeTransportEvent(harp_cbuf *m, uint32_t flags, double tempo,
                                       double ppq, uint64_t ts) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 7); /* etype: transport */
    harp_cbor_map(m, 3);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, flags);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, tempo);
    harp_cbor_uint(m, 4);
    harp_cbor_float(m, ppq);
}

/* UMP event (§9.10): etype 0, body = one packet, words big-endian. */
void HarpRuntime::encodeUmpEvent(harp_cbuf *m, uint32_t word, uint64_t ts) {
    uint8_t bytes[4] = {(uint8_t)(word >> 24), (uint8_t)(word >> 16),
                        (uint8_t)(word >> 8), (uint8_t)word};
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 0); /* etype: ump */
    harp_cbor_bytes(m, bytes, 4);
}

/* Mod event (§9.4 non-destructive modulation): etype 6, body
 * {0 param, 1 signed offset, 3 voice (§9.5 packed key), 5 channel/part}. The
 * offset is NOT clamped here — it is signed and clamped only after summing onto
 * the base, on the device (§9.4). The part (key 5) for a PER-VOICE mod is the
 * channel embedded in the voice key ((voice>>8)&0xf), so the mod lands on the
 * SAME part the note's voice_id was minted under; for a PART-WIDE mod (voice 0)
 * it is `srcChan` — the part the emitting source drives — so a zone-wide MPE
 * master bend/pressure reaches THIS instance's part, not always part 0. Key 5 is
 * omitted when the part is 0 (part-0 byte-economy; the device defaults absent to
 * part 0), so a part-0 source is byte-identical to before. */
void HarpRuntime::encodeModEvent(harp_cbuf *m, uint32_t id, float offset,
                                 uint64_t ts, uint32_t voice, uint8_t srcChan) {
    uint8_t channel = voice ? (uint8_t)((voice >> 8) & 0xf) : (uint8_t)(srcChan & 0xf);
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 6); /* etype: mod */
    int n = 2 + (voice ? 1 : 0) + (channel ? 1 : 0);
    harp_cbor_map(m, (uint64_t)n);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, offset);
    if (voice) { /* key 3 = §9.5 per-voice target; 0/absent = part-wide */
        harp_cbor_uint(m, 3);
        harp_cbor_uint(m, voice);
    }
    if (channel) { /* key 5 = multitimbral part; omitted for part 0 */
        harp_cbor_uint(m, 5);
        harp_cbor_uint(m, channel);
    }
}

/* Ramp event (§9.4): etype 5, msg tstamp = start, body {param, target, end}. */
void HarpRuntime::encodeRampEvent(harp_cbuf *m, uint32_t id, float target,
                                  uint64_t start, uint64_t end, uint8_t channel) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, start);
    harp_cbor_uint(m, 5); /* etype: ramp */
    harp_cbor_map(m, channel ? 4 : 3);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, target);
    harp_cbor_uint(m, 2);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, end);
    if (channel) { /* key 5 = multitimbral part (§9.4); omitted for part 0 */
        harp_cbor_uint(m, 5);
        harp_cbor_uint(m, channel);
    }
}

/* Padded stream positions are SPENT: ssiRead_ always advances by the full
 * request, and the late-arriving samples for those positions are dropped
 * (padDebtFloats_) when they show up. The wrong policy — playing late
 * arrivals anyway — grows latency by every pad and audibly "echoes" the
 * missing moment while the DAW grid drifts. */
void HarpRuntime::settlePadDebt() {
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
        sink.padDebt = 0;
        sink.ring.clear();
    }
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
    if (got < want) {
        memset(dst + got, 0, (want - got) * sizeof(float));
        padDebtFloats_ += want - got;
        if (connected_.load(std::memory_order_acquire)) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            padSamples_.fetch_add((want - got) / 2, std::memory_order_relaxed);
        }
        return (want - got) / 2;
    }
    return 0;
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
    if (got < want) {
        memset(dst + got, 0, (want - got) * sizeof(float));
        sink->padDebt += want - got;
        if (connected_.load(std::memory_order_acquire)) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            padSamples_.fetch_add((want - got) / 2, std::memory_order_relaxed);
        }
        return (want - got) / 2;
    }
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
            memset(dst + got, 0, (want - got) * sizeof(float));
            padDebtFloats_ += want - got;
            underruns_.fetch_add(1, std::memory_order_relaxed);
            padSamples_.fetch_add((want - got) / 2, std::memory_order_relaxed);
            return (want - got) / 2;
        }
        harp_sleep_ns(500000ull); /* 0.5 ms */
        waited++;
    }
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
            memset(dst + got, 0, (want - got) * sizeof(float));
            sink->padDebt += want - got;
            underruns_.fetch_add(1, std::memory_order_relaxed);
            padSamples_.fetch_add((want - got) / 2, std::memory_order_relaxed);
            return (want - got) / 2;
        }
        harp_sleep_ns(500000ull); /* 0.5 ms */
        waited++;
    }
    return 0;
}

/* ---------------- feeder thread ---------------- */

void HarpRuntime::feeder() {
    /* (QoS is set by the supervisor thread, which runs this.) Exits when
     * the plugin stops OR the session dies — the supervisor reconnects. */
#ifdef __APPLE__
    WgState wg;
#endif
    /* §8.7 bit-exact rate-control state (Ethernet only; see the freeRunning_
     * block below). Proportional-only on a smoothed FIFO fill, mirroring the
     * proven eth-bitexact-test loop. */
    double ethSmFill = 0;
    bool ethPrimed = false;
    uint64_t ethLastTrimNs = 0;
    while (running_.load(std::memory_order_relaxed) &&
           connected_.load(std::memory_order_relaxed)) {
        /* §8.3-over-§8.7 mid-stream toggle: a host flipped offline<->live on this live
         * session. Return to the supervisor (leaving connected_ true) so it re-dials in
         * the new mode — checked at the top so the flip is honored within ~one cycle. */
        if (modeFlipPending_.load(std::memory_order_acquire)) return;
#ifdef __APPLE__
        wgMaintain(wg);
#endif
        bool didWork = false;

        /* 0. P5b RE-NEGOTIATION at a SAFE boundary (between pacing cycles — we
         * are never mid-frame here). A sink registered/unregistered mid-session
         * and changed the required slot set, so re-stream the new union: take
         * ctlMutex_ (serialized against getState/setState and the eventPump's evt
         * writes), audio.stop -> new union -> audio.start + fence reset. We clear
         * the flag BEFORE acting and only act when STILL connected; if it fires
         * but the session dropped, the next sessionUp's audio.start picks up the
         * sink set from the registry anyway. The single-instance / no-sink path
         * never sets the flag, so it never reaches this block. */
        if (!freeRunning_ && audioRenegPending_.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lk(ctlMutex_);
            if (connected_.load(std::memory_order_relaxed)) audioRenegotiateLocked();
        }

        /* 0b. §14.3 LoopbackMeasurer at the same SAFE boundary (between pacing
         * cycles — never mid-frame). The host thread armed it (loopbackPending_) and
         * is blocked waiting for the result; we run the probe here so its H->D pacing
         * writes don't race our own pacing below (we ARE the pacer). Only the host-
         * paced path measures (the device requires a live host-paced stream); a free-
         * running session simply publishes a "wrong-mode" result so the caller unblocks. */
        if (loopbackPending_.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lk(ctlMutex_);
            runLoopbackProbeLocked();
            loopbackDone_.store(true, std::memory_order_release);
        }

        /* 1. inbound async traffic FIRST: keeping the IN direction drained
         * means the device is never blocked writing to us — which means OUR
         * writes below never stall (§4.2.1; learned the hard way under
         * automation flood) */
        pollEcho();

        /* (events + panic live on the eventPump thread: an event's wire
         * deadline is ~one DAW block, and the pacing writes below can
         * stall 8 ms in drain-on-stall — head-of-line blocking here is
         * exactly how block-256 sessions leaked evt_late) */

        /* 2a. ETHERNET bit-exact rate control (no host-paced writes). Every ~50 ms
         * read EthTransport's FIFO fill, smooth it, and stream an audio.trim rate
         * correction so the device emits at exactly our consumption rate — then
         * pullAudio plays 1:1 (bit-exact). Proportional-only on a smoothed fill =>
         * first-order stable (eth-bitexact-test: 127 dB, no oscillation). The send
         * takes ctlMutex_ so it serializes with the eventPump's evt writes. */
        if (freeRunning_ && bitExact_) { /* §8.7: trim only a rate-lockable device; ASRC needs none */
            uint64_t nowNs = harp_now_ns();
            if (nowNs - ethLastTrimNs >= 50000000ull) {
                ethLastTrimNs = nowNs;
                const double target = (double)ethTargetFrames();
                unsigned fill = (unsigned)(audioRing_.readAvailable() / 2); /* the reader fills this */
                /* EMA-smooth the fill measurement. alpha SCALES with the loop bandwidth
                 * so the smoother's pole stays a fixed ratio above the (target-dependent)
                 * crossover ω_c = rate·1e-9·K/target — keeping the PHASE MARGIN, hence the
                 * DAMPING, target-invariant too. (The gain normalization below makes only
                 * the clamp-rail band + steady-state offset invariant; a FIXED alpha would
                 * let the EMA lag erode phase margin as ω_c rises at small targets — the
                 * loop rings just below ~256 frames.) At target=2048 alpha=0.03 EXACTLY
                 * (byte-identical to the proven loop); clamped to [0.03, 0.5] so a large-
                 * block default stays at the proven 0.03 and the tiniest buffer never
                 * chases raw measurement jitter. */
                double alpha = 0.03 * 2048.0 / target;
                if (alpha < 0.03) alpha = 0.03;
                else if (alpha > 0.5) alpha = 0.5;
                if (!ethPrimed) { ethSmFill = fill; ethPrimed = true; }
                else ethSmFill += alpha * ((double)fill - ethSmFill);
                /* TARGET-INVARIANT proportional trim on the FRACTIONAL fill error.
                 * The buffer is a pure integrator, so a single gain on the error is
                 * first-order => unconditionally stable. The old loop used an ABSOLUTE
                 * error with a fixed Kp=2000 ppb/frame: its ±200000-ppb clamp railed at
                 * a fixed ±100 frames of error — 4.9% of a 2048 buffer (linear, proven
                 * 127 dB) but ±39% of a 256 buffer, so a small target spent its time on
                 * the rail and hunted. Normalizing by the live target makes the rail
                 * band a CONSTANT 4.9% of the buffer at EVERY setpoint: the loop stays
                 * linear (no retune) from 2048 down to a few hundred frames. The closed-
                 * loop time constant τ = target/(rate·1e-9·K) shrinks with target, so a
                 * small buffer self-corrects faster — exactly what a tight buffer needs.
                 * K = 2000·2048 = 4.096e6 reproduces Kp_eff = K/target = 2000 EXACTLY at
                 * target=2048, i.e. the default path is byte-for-byte the proven loop. */
                static constexpr double kEthNormGain = 2000.0 * 2048.0; /* ppb per unit fractional error */
                double trim = -kEthNormGain * ((ethSmFill - target) / target);
                if (trim > 200000.0) trim = 200000.0;
                else if (trim < -200000.0) trim = -200000.0;
                harp_cbuf tm;
                harp_cbuf_init(&tm);
                harp_client_req_head(&client_, &tm, "audio.trim", true);
                harp_cbor_map(&tm, 1);
                harp_cbor_uint(&tm, 0);
                harp_cbor_float(&tm, (float)trim);
                {
                    std::lock_guard<std::mutex> lk(ctlMutex_);
                    harp_client_send(&client_, &tm); /* fire-and-forget */
                }
                harp_cbuf_free(&tm);
                /* §14.4 host-context-C: snapshot the trim for clock-stats (key 11.6,
                 * ratelock-stats). Plain relaxed atomics, off the RT path. */
                lastTrimPpb_.store((int32_t)trim, std::memory_order_relaxed);
                trimCount_.fetch_add(1, std::memory_order_relaxed);
                didWork = true;
            }
        }

        /* 2. pace (USB host-paced only): ring to target depth, small fixed
         * pipeline on top. The reader thread keeps an audio-IN read permanently
         * pending, so the device's response writes land instantly and its pacing
         * turnaround is just render time. */
        if (!freeRunning_) {
        size_t ringFrames = audioRing_.readAvailable() / 2;
        uint64_t inFlight = framesSent_ - framesRecv_;
        /* The frontier cap is event-timing law, not flow control: event
         * timestamps carry target + one-pacing-block of headroom, so the
         * pacing frontier must never advance past target + kBlock beyond
         * the DAW's read position — or timestamps land in already-paced
         * ranges at queue time and apply late no matter how fast the wire
         * is (measured at DAW block 64: nearly every ramp END fell behind
         * the frontier's in-flight overshoot; fence_timeouts stayed 0 —
         * delivery was perfect, the math wasn't). */
        uint64_t frontierCap = ssiRead_.load(std::memory_order_relaxed) +
                               targetFrames_ + (eventHeadroom() - maxDawBlock_);
        /* the cap bounds the frame END: a frame starting under the cap but
         * extending past it would cover timestamps the current block can
         * still mint (measured: mid-frame note-ons applied a frame late) */
        while (ringFrames < (size_t)targetFrames_ && inFlight < ahead_ &&
               ssi_ + kBlock <= frontierCap) {
            /* every pacing frame carries the event fence (§8.3.1): the
             * count of events queued so far this session. Any event queued
             * before this instant is guaranteed consumed device-side
             * before this range renders — events and pacing travel on
             * different pipes, and without the fence they race (measured:
             * decoupling the event writes from this loop tripled evt_late
             * until the fence closed the order by construction). */
            harp_audio_hdr pace = {HARP_AUDIO_FVER,
                                   HARP_AUDIO_DIR_H2D | HARP_AUDIO_FENCE,
                                   0,
                                   0,
                                   ssi_,
                                   (uint16_t)kBlock,
                                   HARP_AUDIO_FMT_F32};
            uint8_t ph[HARP_AUDIO_HDR_LEN + HARP_AUDIO_FENCE_LEN];
            harp_audio_hdr_encode(&pace, ph);
            /* §8.3.1 fence = events queued SINCE this stream's audio.start =
             * monotonic high-water MINUS the current epoch baseline. At the initial
             * start base == 0, so this is byte-identical to the pre-P5b fence; after
             * a re-neg the device reset g_evt_consumed to 0 and base caught up to the
             * counter, so the count restarts at 0 here too — no over-count, no wedge.
             * SATURATE at 0: unregisterSource fetch_sub's a removed source's leftover
             * (queued-but-unwritten) events, which can pull the monotonic counter
             * BELOW a baseline that had counted them — a raw subtraction would wrap
             * to a huge OVER-count and wedge every later frame. An under-count is
             * always safe (the fence is a minimum -> at worst evt_late), so clamp. */
            uint32_t hw = evtQueuedSeq_.load(std::memory_order_acquire);
            uint32_t base = evtEpochBase_.load(std::memory_order_acquire);
            uint32_t seq = hw > base ? hw - base : 0;
            ph[HARP_AUDIO_HDR_LEN + 0] = (uint8_t)seq;
            ph[HARP_AUDIO_HDR_LEN + 1] = (uint8_t)(seq >> 8);
            ph[HARP_AUDIO_HDR_LEN + 2] = (uint8_t)(seq >> 16);
            ph[HARP_AUDIO_HDR_LEN + 3] = (uint8_t)(seq >> 24);
            if (!transport_->audioWrite(ph, sizeof ph, 8)) break;
            ssi_ += kBlock;
            framesSent_++;
            inFlight++;
            didWork = true;
        }
        } /* end if(!freeRunning_): host-paced pacing */

        /* 4. (audio draining lives on the reader thread; sync the count) */
        framesRecv_ = framesRecvAtomic_.load(std::memory_order_acquire);

        uint64_t drops = evDrops_.load(std::memory_order_relaxed);
        if (drops != evDropsLogged_) {
            log_msg("WARNING: %llu events dropped (ring overflow)",
                    (unsigned long long)(drops - evDropsLogged_));
            evDropsLogged_ = drops;
        }

        if (!didWork) {
            harp_sleep_ns(1000000ull); /* 1 ms */
        }
    }
}

/* Audio-IN reader: one blocking read always pending. This is what makes the
 * device's response writes complete immediately — the §4.2.1 always-pending
 * inbound rule applied to the stream pair. */
/* Dedicated event->wire thread (§9.2). The deadline for an event is its
 * own timestamp's pacing frame — roughly one DAW block of wall time
 * (5.3 ms at 256) — while the feeder's audio writes can stall 8 ms in
 * drain-on-stall. Sharing a loop with pacing spent the event budget on
 * someone else's stall: measured one late event per ~8 min of flood at
 * block 256, zero at >=512 (whose budget exceeds the stall). Events get
 * their own thread; the link endpoint is distinct from the audio
 * endpoint, so the two never contend on the wire — only on ctlMutex_,
 * whose link writes are short. */
/* Drain up to `budget` events from one source's ring, appending each as a
 * framed EVT message to `batch`. Returns the count drained. The eventPump is
 * the SOLE consumer of every source ring (SPSC), and calls this only while
 * holding sourcesMutex_ (the safe-free invariant — see unregisterSource).
 *
 * Param sets and ramps carry the SOURCE's channel (key 5) so each instance's
 * knob edits land on ITS part — this is what makes the merge multitimbral.
 * Notes already carry their channel in the UMP word (the shell baked it in).
 * Transport (kind 3) is global and only ever lives on the owner source. */
int HarpRuntime::drainSource(EventSource &src, harp_cbuf &batch, harp_cbuf &msgbuf,
                             int budget) {
    uint8_t chan = src.chan.load(std::memory_order_relaxed);
    TimedEv te;
    int sent = 0;
    for (; sent < budget && src.ring.pop(te); sent++) {
        harp_cbuf_reset(&msgbuf);
        if (te.kind == 0)
            encodeParamEvent(&msgbuf, te.a, te.v, te.ts, chan);
        else if (te.kind == 1)
            encodeRampEvent(&msgbuf, te.a, te.v, te.ts, te.end, chan);
        else if (te.kind == 3) {
            double ppq;
            memcpy(&ppq, &te.end, sizeof ppq);
            encodeTransportEvent(&msgbuf, te.a, te.v, ppq, te.ts);
        } else if (te.kind == 4)
            /* mod (§9.4): the voice key rides in `end`; a per-voice mod takes its
             * part from the voice key, a part-wide mod (voice 0) from this source's
             * channel — so a zone-wide MPE master bend reaches this part, not 0. */
            encodeModEvent(&msgbuf, te.a, te.v, te.ts, (uint32_t)te.end, chan);
        else
            encodeUmpEvent(&msgbuf, te.a, te.ts);
        harp_frame_hdr h = {HARP_FRAME_FVER, HARP_STREAM_EVT, HARP_FLAG_FIN,
                            (uint32_t)msgbuf.len};
        uint8_t hdr[HARP_FRAME_HDR_LEN];
        harp_frame_hdr_encode(&h, hdr);
        harp_cbuf_put(&batch, hdr, sizeof hdr);
        harp_cbuf_put(&batch, msgbuf.buf, msgbuf.len);
    }
    return sent;
}

void HarpRuntime::eventPump() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    harp_cbuf msgbuf, batch;
    harp_cbuf_init(&msgbuf);
    harp_cbuf_init(&batch);
#ifdef __APPLE__
    WgState wg;
#endif
    while (running_.load(std::memory_order_relaxed) &&
           connected_.load(std::memory_order_relaxed)) {
#ifdef __APPLE__
        wgMaintain(wg);
#endif
        bool didWork = false;

        /* escalated panic: a note-off was lost to overflow — all-notes-off
         * (CC 123) ahead of everything else */
        if (panicPending_.exchange(false, std::memory_order_acq_rel)) {
            harp_cbuf m;
            harp_cbuf_init(&m);
            encodeUmpEvent(&m, ump_all_notes_off(), 0); /* CC 123, now */
            std::lock_guard<std::mutex> lk(ctlMutex_);
            harp_link_send(transport_->ctlIo(), HARP_STREAM_EVT, m.buf, m.len);
            harp_cbuf_free(&m);
            log_msg("WARNING: note-off lost to overflow; sent all-notes-off");
        }

        /* timestamped events (params, ramps, notes — §9.2/§9.4/§9.10),
         * batched into ONE framed bulk write (per-event writes starve the
         * pipe); the cap only bounds the write size — the loop comes
         * straight back for the rest.
         *
         * P5 MERGE: drain EVERY registered source, stamping each event with
         * ITS source's channel (param/ramp), so a multitimbral group's parts
         * all land on the one wire. The owner source is drained FIRST and is
         * slot 0 — so with a SINGLE instance this is exactly the pre-P5 path:
         * one source, one channel, the same 64-event batch, BYTE-IDENTICAL.
         * We drain into `batch` UNDER sourcesMutex_ (pure memory work, no I/O)
         * and write the wire AFTER unlocking: that lock is the safe-free
         * invariant (unregisterSource deletes under it, so the pump never
         * reads a freed ring) and it never wraps the wire write. The audio
         * thread (queue*) never takes this lock — its source pointer is its
         * own SPSC producer side. */
        harp_cbuf_reset(&batch);
        int sent = 0;
        {
            std::lock_guard<std::mutex> slk(sourcesMutex_);
            for (size_t i = 0; i < nSources_; i++)
                sent += drainSource(*sources_[i], batch, msgbuf, 64 - sent);
        }
        if (sent) {
            std::lock_guard<std::mutex> lk(ctlMutex_);
            /* A P5b re-negotiation may run (under ctlMutex_, serialized against this
             * write) between our drain and here. With the MONOTONIC fence + epoch
             * baseline it is SAFE to write these events across a re-neg: the device's
             * audio.start reset its g_evt_consumed, and these straddling events were
             * counted into evtQueuedSeq_ BELOW the new evtEpochBase_, so they do not
             * count toward any post-reneg frame's (evtQueuedSeq_ - evtEpochBase_)
             * fence. The device simply consumes them (a fence is a §8.3.1 minimum —
             * consuming MORE than required never wedges), so no batch needs dropping
             * and no event is lost. */
            harp_io *cio = transport_->ctlIo();
            if (!cio->write_all(cio, batch.buf, batch.len)) {
                log_msg("event write failed; device gone?");
                connected_.store(false, std::memory_order_release);
            }
            didWork = true;
        }

        /* a 17th alias hit the full source table and is event-dormant; the
         * audio thread raised the flag, we log it once (off the RT path) */
        if (dormantSrcSeen_.load(std::memory_order_relaxed) &&
            !dormantSrcLogged_.exchange(true, std::memory_order_relaxed))
            log_msg("WARNING: source table full (%zu parts) — a further instance "
                    "is event-dormant; its events are dropped",
                    kMaxSources);

        if (!didWork) {
            harp_sleep_ns(500000ull); /* 0.5 ms — well inside the one-block budget */
        }
    }
    harp_cbuf_free(&msgbuf);
    harp_cbuf_free(&batch);
}

/* Demux ONE union audio frame (pl = ns frames of S slot-interleaved floats): the
 * owner's main-mix columns {0,1} -> audioRing_, and each per-part sink's columns ->
 * its ring. Shared by the USB reader (host-paced framed audio) and the §8.7 RTP
 * reader (bit-exact + ASRC), so BOTH bindings deliver per-part audio identically.
 * S==2 is the contiguous {0,1}-only fast path (a single instance / no per-part sink
 * — the byte-identical golden case). Caller is the sole writer of audioRing_ and the
 * sink rings (the reader thread, or — for ASRC — the owner's resampling pull). */
void HarpRuntime::demuxUnionFrame(const float *pl, size_t ns, uint16_t S) {
    if (S == 2) {
        audioRing_.write(pl, ns * 2);
    } else if (S > 2) {
        /* wider union: gather the owner's columns 0,1 (the main mix) out
         * of the slot-interleaved frame into an interleaved L/R chunk. */
        float tmp[1024 * 2];
        size_t i = 0;
        while (i < ns) {
            size_t chunk = ns - i < 1024 ? ns - i : 1024;
            for (size_t j = 0; j < chunk; j++) {
                tmp[2 * j] = pl[(i + j) * S + 0];
                tmp[2 * j + 1] = pl[(i + j) * S + 1];
            }
            audioRing_.write(tmp, chunk * 2);
            i += chunk;
        }
    } else if (S == 1) {
        /* mono union (a single-slot owner subscription): duplicate L=R
         * so the owner ring stays interleaved-stereo. The default path
         * is always a 2-slot {0,1} pair, so this is a robustness branch,
         * never the golden case. */
        float tmp[1024 * 2];
        size_t i = 0;
        while (i < ns) {
            size_t chunk = ns - i < 1024 ? ns - i : 1024;
            for (size_t j = 0; j < chunk; j++)
                tmp[2 * j] = tmp[2 * j + 1] = pl[i + j];
            audioRing_.write(tmp, chunk * 2);
            i += chunk;
        }
    }
    /* P5b DEMUX: split this frame's slot columns into every per-part sink's ring.
     * The caller is the SOLE writer of every sink (SPSC producer side); the lock is
     * the safe-free invariant against unregisterAudioSink (it removes a sink under
     * the same lock before freeing, so we never write a freed ring). Pure memory
     * work — no I/O under the lock. The relaxed haveSinks_ gate keeps the single-
     * instance / default-main-mix path LOCK-FREE: with no sink the lock is never
     * taken at all. */
    if (haveSinks_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(sinksMutex_);
        for (size_t si = 0; si < nSinks_; si++) {
            AudioSink *sk = sinks_[si];
            uint16_t cL = sk->cols[0], cR = sk->cols[1];
            if (cL >= S || cR >= S) continue; /* slot not in this union */
            float tmp[1024 * 2];
            size_t i = 0;
            while (i < ns) {
                size_t chunk = ns - i < 1024 ? ns - i : 1024;
                for (size_t j = 0; j < chunk; j++) {
                    tmp[2 * j] = pl[(i + j) * S + cL];
                    tmp[2 * j + 1] = pl[(i + j) * S + cR];
                }
                sk->ring.write(tmp, chunk * 2);
                i += chunk;
            }
        }
    }
}

unsigned HarpRuntime::minRingFillFrames() {
    size_t m = audioRing_.readAvailable() / 2; /* owner main mix (interleaved L/R) */
    if (haveSinks_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(sinksMutex_);
        for (size_t si = 0; si < nSinks_; si++) {
            size_t f = sinks_[si]->ring.readAvailable() / 2;
            if (f < m) m = f;
        }
    }
    return (unsigned)m;
}

void HarpRuntime::reader() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    if (freeRunning_) {
        /* §8.7 Ethernet: receive the device's RTP stream and write it 1:1 into
         * audioRing_ (bit-exact — no resampling). The DAW audio thread consumes
         * audioRing_ exactly as on USB; the feeder's audio.trim loop holds its
         * fill at setpoint. RTP has no EOF, so if the stream goes silent we flip
         * connected_ and let the supervisor reconnect. This thread is joined by
         * sessionDown before transport_ is freed, so the audio thread never
         * races the teardown. */
#ifdef __APPLE__
        WgState wgf;
#endif
        /* one RTP union packet = ns(=kBlock) frames of S(<=34) slot-interleaved
         * floats — sized to the widest union (main {0,1} + 16 stereo part pairs).
         * recvAudio returns the raw payload as floats; we split it by the union
         * width the host negotiated at audio.start (key 4) and demux exactly as the
         * USB reader does, so per-part audio lands in the same sinks. */
        static constexpr unsigned kRtpBufFloats = kBlock * 34;
        float buf[kRtpBufFloats];
        if (bitExact_) {
            /* rate-lock device: write the union 1:1 to audioRing_ + per-part sinks
             * (no resampling — the device's emit rate is slaved to us via audio.trim). */
            while (running_.load(std::memory_order_relaxed) &&
                   !readerStop_.load(std::memory_order_acquire)) {
#ifdef __APPLE__
                wgMaintain(wgf);
#endif
                unsigned floats = transport_->recvAudio(buf, kRtpBufFloats, 100, nullptr);
                unsigned width = unionWidth_.load(std::memory_order_relaxed);
                if (!width) width = 2;
                if (floats >= width)
                    demuxUnionFrame(buf, floats / width, (uint16_t)width);
                else if (floats == 0 && transport_->silentMs() > 1000) {
                    log_msg("RTP stream silent >1s; device gone?");
                    /* §12.1: implicit STREAMING -> DETACHED (transport-error). On
                     * the reader thread; the rings are reader-safe (history mutex,
                     * lock-free log) and off the render path. */
                    recordTransition(HARP_ST_STREAMING, HARP_ST_DETACHED,
                                     HARP_TR_TRANSPORT_ERROR, "RTP stream silent >1s");
                    recordLog(HARP_LOG_ERROR, "transport", "RTP stream silent >1s; device gone?");
                    connected_.store(false, std::memory_order_release);
                    break;
                }
            }
        } else {
            /* ASRC (device can't rate-lock): the device free-runs on its own crystal,
             * so recover its rate from the RTP timestamps and RESAMPLE the whole union
             * to the host clock — HERE on the reader thread, into the SAME stable
             * audioRing_ + per-part sinks the bit-exact path uses (demuxUnionFrame). So
             * pullAudio is unchanged and there is NO resampler on the audio thread to
             * race a reconnect: the freerun is reader-LOCAL, freed when this loop exits
             * (which sessionDown joins before reaping transport_). MULTICHANNEL — the
             * resampled union is demuxed exactly like bit-exact, so per-part audio works
             * over ASRC too. The steady audioRing_ drain (pullAudio) paces our pull, so
             * the freerun sees a steady ~host-rate consumer. */
            unsigned width = unionWidth_.load(std::memory_order_relaxed);
            if (!width) width = 2;
            harp_freerun_cfg cfg = {};
            cfg.channels = width;
            cfg.host_rate_hz = (double)rate_;
            cfg.dev_rate_hz = (double)rate_; /* nominal seed; recovered from RTP timestamps */
            cfg.target_frames = ethTargetFrames();
            cfg.capacity_frames = ethTargetFrames() * 4;
            cfg.quality = SRC_SINC_FASTEST; /* still exceeds the §8.3 stopband floor */
            harp_freerun *fr = harp_freerun_new(&cfg);
            if (!fr) {
                log_msg("ASRC: resampler init failed (libsamplerate?) — no audio");
                connected_.store(false, std::memory_order_release);
                return;
            }
            uint64_t prevTs = 0;
            float resamp[kRtpBufFloats];
            const unsigned target = ethTargetFrames(); /* keep audioRing_ near here */
            /* §14.4 host-context-C: this reader IS the ASRC clock-recovery, so it is
             * the authoritative source for clock-stats.5 (asrc-stats). Mark the
             * snapshot live; the cleanup below clears it when the reader exits (the
             * ASRC instance is reader-local and freed there). */
            asrcLive_.store(true, std::memory_order_relaxed);
            while (running_.load(std::memory_order_relaxed) &&
                   !readerStop_.load(std::memory_order_acquire)) {
#ifdef __APPLE__
                wgMaintain(wgf);
#endif
                unsigned ts32 = 0;
                unsigned floats = transport_->recvAudio(buf, kRtpBufFloats, 100, &ts32);
                if (floats >= width) {
                    unsigned ns = floats / width;
                    uint64_t dev = harp_rtp_unwrap_ts((uint32_t)ts32, prevTs);
                    prevTs = dev;
                    harp_freerun_observe(fr, dev, harp_now_ns());
                    harp_freerun_push(fr, buf, ns); /* raw union (channels = width) */
                    /* drain the resampler into audioRing_/sinks until EVERY consumer
                     * ring nears target or the resampler runs dry; the consumers'
                     * steady drains set the pace. Gating on the MIN across main + sinks
                     * (not just main) keeps the fastest-draining part fed when consumer
                     * rates skew — a slower over-full ring just drops the surplus. */
                    while (harp_freerun_warm(fr) && minRingFillFrames() < target) {
                        unsigned got = harp_freerun_pull(fr, resamp, kBlock);
                        if (got) demuxUnionFrame(resamp, got, (uint16_t)width);
                        if (got < kBlock) break; /* dry — don't spin on silence */
                    }
                    /* §14.4 host-context-C: publish the recovery state for clock-stats
                     * (key 11.5). Off the audio path — get_stats only reads atomics, so
                     * it adds nothing to the render and getDiagBundle reads the flat
                     * snapshot without ever touching the reader-local `fr`. */
                    harp_freerun_stats st;
                    harp_freerun_get_stats(fr, &st);
                    uint64_t rb; memcpy(&rb, &st.ratio, sizeof rb);
                    uint64_t jb; memcpy(&jb, &st.jitter_us, sizeof jb);
                    asrcRatioBits_.store(rb, std::memory_order_relaxed);
                    asrcJitterBits_.store(jb, std::memory_order_relaxed);
                    asrcEstPpm_.store(st.est_ppm, std::memory_order_relaxed);
                    asrcFill_.store(st.fill_frames, std::memory_order_relaxed);
                    asrcUnderflow_.store(st.underflow_frames, std::memory_order_relaxed);
                    asrcOverflow_.store(st.overflow_frames, std::memory_order_relaxed);
                } else if (floats == 0 && transport_->silentMs() > 1000) {
                    log_msg("RTP stream silent >1s; device gone?");
                    /* §12.1: implicit STREAMING -> DETACHED (transport-error), ASRC path. */
                    recordTransition(HARP_ST_STREAMING, HARP_ST_DETACHED,
                                     HARP_TR_TRANSPORT_ERROR, "RTP stream silent >1s (ASRC)");
                    recordLog(HARP_LOG_ERROR, "transport", "RTP stream silent >1s; device gone?");
                    connected_.store(false, std::memory_order_release);
                    break;
                }
            }
            asrcLive_.store(false, std::memory_order_relaxed); /* fr about to be freed */
            harp_freerun_free(fr);
        }
        return;
    }
    uint8_t acc[65536];
    size_t accLen = 0;
#ifdef __APPLE__
    WgState wg;
#endif
    /* readerStop_ lets a P5b re-negotiation reclaim the audio-IN endpoint WITHOUT
     * tearing the session down: the feeder sets it, we exit, it drains the stream
     * tail + does audio.stop/start, then respawns us. connected_ stays true, so
     * this is NOT the device-gone path (that flips connected_ and breaks below). */
    while (running_.load(std::memory_order_relaxed) &&
           !readerStop_.load(std::memory_order_acquire)) {
#ifdef __APPLE__
        wgMaintain(wg);
#endif
        int r = transport_->audioRead(acc + accLen, (int)(sizeof acc - accLen), 100);
        if (r < 0) {
            log_msg("audio stream read failed; device gone?");
            /* §12.1: implicit STREAMING -> DETACHED (transport-error), USB path. */
            recordTransition(HARP_ST_STREAMING, HARP_ST_DETACHED, HARP_TR_TRANSPORT_ERROR,
                             "audio stream read failed");
            recordLog(HARP_LOG_ERROR, "transport", "audio stream read failed; device gone?");
            connected_.store(false, std::memory_order_release);
            break;
        }
        size_t off = 0;
        accLen += (size_t)r;
        while (accLen - off >= HARP_AUDIO_HDR_LEN) {
            harp_audio_hdr h;
            if (!harp_audio_hdr_decode(acc + off, &h)) {
                log_msg("malformed stream frame; resyncing");
                accLen = 0;
                off = 0;
                break;
            }
            size_t need = HARP_AUDIO_HDR_LEN + harp_audio_payload_len(&h);
            if (accLen - off < need) break;
            const float *pl = (const float *)(acc + off + HARP_AUDIO_HDR_LEN);
            size_t ns = (size_t)h.nsamples;
            uint16_t S = h.slots; /* columns in this frame == |unionSlots_| */
            demuxUnionFrame(pl, ns, S); /* main mix -> audioRing_; per-part -> sinks */
            framesRecvAtomic_.fetch_add(1, std::memory_order_release);
            off += need;
        }
        memmove(acc, acc + off, accLen - off);
        accLen -= off;
    }
}

/* ---------------- state: pull / push / bundle ---------------- */

bool HarpRuntime::refsLocked(harp_ref *live) {
    return harp_client_find_ref(&client_, LIVE_REF, live) == 0;
}

bool HarpRuntime::snapshotLocked(harp_hash *out) {
    return harp_client_snapshot(&client_, LIVE_REF, "VST3 shell", out) == 0;
}

struct HashList {
    harp_hash h[512];
    size_t n = 0;
    bool contains(const harp_hash &x) const {
        for (size_t i = 0; i < n; i++)
            if (harp_hash_eq(&h[i], &x)) return true;
        return false;
    }
    bool add(const harp_hash &x) {
        if (contains(x) || n >= 512) return false;
        h[n++] = x;
        return true;
    }
};

static bool collect_cb(const harp_hash *h, void *ud) {
    ((HashList *)ud)->add(*h);
    return true;
}

bool HarpRuntime::fetchClosureLocked(const harp_hash &root) {
    return harp_client_fetch_closure(&client_, &root, nullptr) == 0;
}

/* Push staged target to the device: the §11.4 Push with archive-before-push.
 * Caller holds ctlMutex_. */
bool HarpRuntime::pushStateLocked(const harp_hash &target) {
    harp_ref live;
    if (!refsLocked(&live)) return false;
    if (!live.unborn && !live.dirty && harp_hash_eq(&live.hash, &target)) {
        log_msg("recall: hash match, clean -> SYNCED silently");
        recordLog(HARP_LOG_INFO, "reconcile", "hash match, clean -> SYNCED silently");
        return true;
    }
    harp_hash deviceHead = live.hash;
    if (live.dirty && !snapshotLocked(&deviceHead)) return false;

    /* §11.4: a real conflict (the device holds divergent state) is NOT auto-resolved
     * (spec §12.2) — post an offer to the front panel and let the user pick the action.
     * The window is HARP_RECONCILE_TIMEOUT_MS (default 30s, so a live DAW needs no env
     * var). With it 0 (headless/CI) we SKIP the offer ENTIRELY and Push straight away:
     * the per-part-audio stress fires setStateBundle ~5x/s, and an offer+poll round-trip
     * per push wedges the device — so headless takes the same archive-protected Push the
     * pre-recall baseline did. An unborn device is always a clean first push. */
    int timeout_ms = 30000;
    if (const char *env = getenv("HARP_RECONCILE_TIMEOUT_MS")) timeout_ms = atoi(env);
    int choice = 0; /* default Push — unborn, headless (timeout 0), and the no-pick path */
    if (!live.unborn && timeout_ms > 0) {
        char expect12[16], live12[16], hex[2 * HARP_HASH_LEN + 1];
        harp_hash_hex(&target, hex);
        snprintf(expect12, sizeof expect12, "%.12s", hex);
        harp_hash_hex(&deviceHead, hex);
        snprintf(live12, sizeof live12, "%.12s", hex);
        choice = -1;
        if (harp_client_reconcile_offer(&client_, expect12, live12, live.dirty ? 1 : 0) == 0) {
            log_msg("recall: mismatch%s -> reconcile offer posted (expect %s live %s)",
                    live.dirty ? " + dirty edits" : "", expect12, live12);
            const int POLL_MS = 200;
            for (int waited = 0; waited < timeout_ms; waited += POLL_MS) {
                bool pending = true;
                if (harp_client_reconcile_poll(&client_, &pending, &choice) != 0) break;
                if (!pending) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
            }
        }
        if (choice < 0) { /* nobody answered (no panel attached, or timed out) */
            log_msg("recall: no reconcile pick -> Push fallback (archive-protected)");
            choice = 0;
        }
    }

    if (choice == 1) { /* Pull to DAW: the host adopts the device state; device untouched */
        log_msg("recall: reconcile -> Pull (host adopts the device state)");
        /* §11.4 safe action — SYNCED -> SYNCED (state class unchanged; the
         * reconcile resolved how, not whether). */
        recordTransition(HARP_ST_SYNCED, HARP_ST_SYNCED, HARP_TR_RECONCILE_PULL,
                         "reconcile -> Pull (host adopts the device state)");
        recordLog(HARP_LOG_INFO, "reconcile", "Pull (host adopts the device state)");
        return fetchClosureLocked(deviceHead);
    }
    if (choice == 2) { /* Read-only: observe, no writes either way */
        log_msg("recall: reconcile -> Read-only (no writes)");
        recordTransition(HARP_ST_SYNCED, HARP_ST_SYNCED, HARP_TR_RECONCILE_OPEN_RO,
                         "reconcile -> Read-only (no writes)");
        recordLog(HARP_LOG_INFO, "reconcile", "Read-only (no writes)");
        return true;
    }

    /* Push (0) or Duplicate (3): archive the displaced device state (Duplicate names
     * it visibly as duplicate/<ts>, Push as archive/<ts>), push the target's closure,
     * CAS the live ref to the target. */
    const char *prefix = (choice == 3) ? "duplicate" : "archive";
    if (choice == 3) {
        log_msg("recall: reconcile -> Duplicate (displaced state kept as duplicate/<ts>)");
        recordTransition(HARP_ST_SYNCED, HARP_ST_SYNCED, HARP_TR_RECONCILE_DUPLICATE_PUSH,
                         "reconcile -> Duplicate (displaced state kept as duplicate/<ts>)");
        recordLog(HARP_LOG_INFO, "reconcile",
                  "Duplicate (displaced state kept as duplicate/<ts>)");
    } else {
        log_msg("recall: %s -> Push (archive-then-CAS)", live.unborn ? "first push" : "reconcile");
        recordTransition(HARP_ST_SYNCED, HARP_ST_SYNCED, HARP_TR_RECONCILE_PUSH,
                         live.unborn ? "first push (archive-then-CAS)"
                                     : "reconcile -> Push (archive-then-CAS)");
        recordLog(HARP_LOG_INFO, "reconcile",
                  live.unborn ? "first push (archive-then-CAS)" : "Push (archive-then-CAS)");
    }
    /* The archive is loss-prevention for the interactive Push/Duplicate. Headless
     * (timeout 0) skips it: under the per-part-audio stress a per-push archive/<ts>
     * ref (5x/s across the whole cluster) is pure churn that can wedge the gadget,
     * and a CI stress run has no displaced state worth keeping. */
    if (!live.unborn && timeout_ms > 0) {
        char archive[96];
        time_t now = time(nullptr);
        struct tm tm;
        harp_gmtime(now, &tm);
        snprintf(archive, sizeof archive, "%s/%04d-%02d-%02dT%02d:%02d:%02dZ", prefix,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (harp_client_refset(&client_, archive, nullptr, &deviceHead, true, false, nullptr) != 0)
            return false;
    }

    /* transfer missing objects: have -> send -> obj stream */
    if (harp_client_push_closure(&client_, &target, nullptr, nullptr) != 0) {
        log_msg("recall: state push failed (%s)", client_.err_code[0]
                                                      ? client_.err_code
                                                      : "local closure incomplete?");
        return false;
    }

    /* CAS the live ref */
    bool ok = harp_client_refset(&client_, LIVE_REF, live.unborn ? nullptr : &deviceHead,
                                 &target, live.unborn, false, nullptr) == 0;
    if (ok) log_msg("recall: live/project restored");
    /* The bundle reference is PERSISTENT, not consumed: the DAW project's notion of
     * state re-asserts on every reconnect ("Live wins") — the archive/duplicate step
     * keeps it loss-free. It only moves when a save pulls a new head (getStateBundle)
     * or a new set loads. */
    return ok;
}

/* ---- bundle codec (§15.3) ---- */

static void encode_bundle(harp_cbuf *out, uint32_t vendorId, const std::string &vendorName,
                          uint32_t productId, const std::string &productName,
                          const std::string &serial, const std::string &engineId,
                          const std::string &engineVer, const harp_hash &pmh,
                          const harp_ref &ref, harp_store *store, HashList &closure,
                          uint16_t usbVid, uint16_t usbPid,
                          const std::string &usbSerial) {
    harp_cbor_map(out, 6); /* +1: key 5 usb-identity */
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, BUNDLE_MAGIC);
    harp_cbor_uint(out, 1);
    harp_cbor_uint(out, 1); /* bundle version */
    harp_cbor_uint(out, 2); /* identity-expectation */
    harp_cbor_map(out, 4);
    harp_cbor_uint(out, 0);
    harp_cbor_map(out, 2);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, vendorId);
    harp_cbor_uint(out, 1);
    harp_cbor_text(out, vendorName.c_str());
    harp_cbor_uint(out, 1);
    harp_cbor_map(out, 2);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, productId);
    harp_cbor_uint(out, 1);
    harp_cbor_text(out, productName.c_str());
    harp_cbor_uint(out, 2);
    harp_cbor_text(out, serial.c_str());
    harp_cbor_uint(out, 3);
    harp_cbor_map(out, 3);
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, engineId.c_str());
    harp_cbor_uint(out, 1);
    harp_cbor_text(out, engineVer.c_str());
    harp_cbor_uint(out, 2);
    harp_cbor_bytes(out, pmh.b, HARP_HASH_LEN);
    harp_cbor_uint(out, 3); /* refs */
    harp_cbor_array(out, 1);
    harp_ref_encode(out, &ref);
    harp_cbor_uint(out, 4); /* embedded object closure */
    harp_cbor_array(out, closure.n);
    for (size_t i = 0; i < closure.n; i++) {
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        if (harp_store_get(store, &closure.h[i], &enc) == 0)
            harp_cbuf_put(out, enc.buf, enc.len); /* objects are CBOR items */
        harp_cbuf_free(&enc);
    }
    /* key 5: USB-descriptor identity of the device this bundle was saved
     * from — the multi-device selection key. Additive; decoders that
     * predate it skip it. */
    harp_cbor_uint(out, 5);
    harp_cbor_map(out, 3);
    harp_cbor_uint(out, 0);
    harp_cbor_uint(out, usbVid);
    harp_cbor_uint(out, 1);
    harp_cbor_uint(out, usbPid);
    harp_cbor_uint(out, 2);
    harp_cbor_text(out, usbSerial.c_str());
}

bool HarpRuntime::getStateBundle(std::vector<uint8_t> &out) {
    if (!storeOk_) return false;
    /* The host-paced session can briefly drop between render passes — the
     * supervisor reconnects within ~1s. A save (getState) right after a render,
     * as REAPER and other offline hosts do, can land in that window; wait
     * (bounded) for the device to come back rather than failing the save. */
    for (int i = 0; i < 60 && !connected(); i++) harp_sleep_ns(50000000ull); /* ≤ ~3 s */
    if (!connected()) return false; /* genuinely offline: nothing to save */
    std::lock_guard<std::mutex> lk(ctlMutex_);
    harp_ref live;
    if (!refsLocked(&live)) return false;
    harp_hash head = live.hash;
    if (live.unborn) return false;
    if (live.dirty && !snapshotLocked(&head)) return false;
    if (!fetchClosureLocked(head)) return false;

    HashList clo;
    clo.add(head);
    for (size_t cur = 0; cur < clo.n; cur++) {
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        if (harp_store_get(&store_, &clo.h[cur], &enc) == 0)
            harp_obj_foreach_child(enc.buf, enc.len, false, collect_cb, &clo);
        harp_cbuf_free(&enc);
    }

    harp_ref expected = {};
    snprintf(expected.name, sizeof expected.name, "%s", LIVE_REF);
    expected.unborn = false;
    expected.hash = head;
    expected.generation = live.generation;
    expected.dirty = false;

    harp_cbuf b;
    harp_cbuf_init(&b);
    encode_bundle(&b, vendorId_, vendorName_, productId_, productName_, serial_, engineId_,
                  engineVer_, paramMapHash_, expected, &store_, clo, usbVid_, usbPid_,
                  usbSerial_);
    out.assign(b.buf, b.buf + b.len);
    harp_cbuf_free(&b);
    { /* a save moves the project's reference point to what it captured */
        std::lock_guard<std::mutex> slk(bundleMutex_);
        hasBundle_ = true;
        bundleTarget_ = head;
    }
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&head, hex);
    hex[12] = 0;
    log_msg("recall bundle saved: live/project @ %s… (%zu bytes)", hex, out.size());
    return true;
}

/* §14.4 diag-bundle / §16 anonymization. DECODE the device-section map
 * { 0 => identity, 1 => counters } and RE-ENCODE it into `out`, clearing the
 * identity PII leaves to "" IN PLACE while preserving everything else byte-for-
 * meaning. This MIRRORS host/harp-probe.c:anonymize_device_section (the
 * authoritative §16 leaf list) so the host's two diag-bundle producers (the
 * out-of-process harp-probe and the in-shell runtime) clear the SAME leaves
 * identically. Cleared: identity key 2 (serial); vendor (key 0)/product (key 1)
 * sub-map key 1 (name); identity key 9 (build-id); channel-map (key 7) per-entry
 * keys 2/3/4 (name/group/path). PRESERVED verbatim: vid/pid, firmware, engine
 * (incl. engine-id + param-map-hash), protocol, latency-profile, boot count,
 * ump-group-map, part count, caps, per-entry slot/direction/host-paced flag, the
 * array length/order, and the whole counters map (no PII). Subtrees needing no
 * edit are copied via harp_cdec_span (byte-for-byte). Returns false on a
 * malformed section so the caller can fall back. */
static bool anon_device_section(harp_cbuf *out, const uint8_t *sec, size_t len) {
    harp_cdec d;
    harp_cdec_init(&d, sec, len);
    uint64_t nsec;
    if (!harp_cdec_map(&d, &nsec)) return false;
    harp_cbor_map(out, nsec);
    for (uint64_t i = 0; i < nsec; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        harp_cbor_uint(out, key);
        if (key != 0) { /* counters (key 1) + any future member: no PII, verbatim */
            const uint8_t *span;
            size_t sl;
            if (!harp_cdec_span(&d, &span, &sl)) return false;
            harp_cbuf_put(out, span, sl);
            continue;
        }
        /* key 0 => identity: re-encode, clearing serial + vendor/product names. */
        uint64_t nid;
        if (!harp_cdec_map(&d, &nid)) return false;
        harp_cbor_map(out, nid);
        for (uint64_t j = 0; j < nid; j++) {
            uint64_t ik;
            if (!harp_cdec_uint(&d, &ik)) return false;
            harp_cbor_uint(out, ik);
            if (ik == 0 || ik == 1) { /* vendor/product { 0 => id, 1 => name } */
                uint64_t nsub;
                if (!harp_cdec_map(&d, &nsub)) return false;
                harp_cbor_map(out, nsub);
                for (uint64_t s = 0; s < nsub; s++) {
                    uint64_t sk;
                    if (!harp_cdec_uint(&d, &sk)) return false;
                    harp_cbor_uint(out, sk);
                    if (sk == 1) {
                        if (!harp_cdec_skip(&d)) return false; /* drop the name */
                        harp_cbor_text(out, "");               /* "" in place (§16) */
                    } else {
                        const uint8_t *span;
                        size_t sl;
                        if (!harp_cdec_span(&d, &span, &sl)) return false;
                        harp_cbuf_put(out, span, sl);
                    }
                }
            } else if (ik == 2 || ik == 9) { /* serial / build-id -> "" (§16) */
                if (!harp_cdec_skip(&d)) return false;
                harp_cbor_text(out, "");
            } else if (ik == 7) { /* channel-map: clear per-entry name/group/path */
                uint64_t nent;
                if (!harp_cdec_array(&d, &nent)) return false;
                harp_cbor_array(out, nent);
                for (uint64_t en = 0; en < nent; en++) {
                    uint64_t nek;
                    if (!harp_cdec_map(&d, &nek)) return false;
                    harp_cbor_map(out, nek);
                    for (uint64_t ek = 0; ek < nek; ek++) {
                        uint64_t ekey;
                        if (!harp_cdec_uint(&d, &ekey)) return false;
                        harp_cbor_uint(out, ekey);
                        if (ekey == 2 || ekey == 3 || ekey == 4) {
                            if (!harp_cdec_skip(&d)) return false; /* name/group/path */
                            harp_cbor_text(out, "");               /* "" in place */
                        } else {
                            const uint8_t *span;
                            size_t sl;
                            if (!harp_cdec_span(&d, &span, &sl)) return false;
                            harp_cbuf_put(out, span, sl);
                        }
                    }
                }
            } else { /* fw/engine/protocol/latency/boot/ump-map/parts: verbatim */
                const uint8_t *span;
                size_t sl;
                if (!harp_cdec_span(&d, &span, &sl)) return false;
                harp_cbuf_put(out, span, sl);
            }
        }
    }
    return !d.err;
}

/* §14.4 host-context-C: clock-stats (top key 11). ALWAYS emitted. Deterministic
 * CBOR per the design CDDL: { 0 => clock_drift_ppb, 3 => clock-recovery, ?4 =>
 * reanchors, ?5 => asrc-stats (iff recovery==asrc), ?6 => ratelock-stats (iff
 * recovery==rate-lock) }. NO PII — the §16 pass does not touch it; it is a pure
 * numeric snapshot of the runtime's recovery state. The recovery enum (CDDL
 * clock-recovery) is decided exactly as bitExact_ is at sessionUp:
 *   host-paced (0): USB / not free-running — SSI-driven, no recovery.
 *   asrc (1):       free-running device with NO audio.rate-lock (host resamples).
 *   rate-lock (2):  free-running + rate-lock (bit-exact; the feeder trims).
 * Called under ctlMutex_ from getDiagBundle (reads the snapshot atomics only). */
void HarpRuntime::emitClockStats(harp_cbuf *out) {
    bool freeRun = freeRunning_.load(std::memory_order_relaxed);
    /* recovery: 0 host-paced, 1 asrc, 2 rate-lock. */
    int recovery = !freeRun ? 0 : (bitExact_ ? 2 : 1);
    bool haveAsrc = (recovery == 1) && asrcLive_.load(std::memory_order_relaxed);
    bool haveRatelock = (recovery == 2);
    /* host-measured drift gauge (key 0): the ASRC recovers it from the RTP
     * timestamps (est_ppm -> ppb); rate-lock/host-paced have no host estimate -> 0. */
    int64_t driftPpb = haveAsrc
        ? (int64_t)llround(asrcEstPpm_.load(std::memory_order_relaxed) * 1000.0)
        : 0;
    uint64_t nkeys = 2; /* key 0 (drift) + key 3 (recovery) always */
    if (haveAsrc || haveRatelock) nkeys++; /* key 5 or key 6 */
    harp_cbor_uint(out, 11);
    harp_cbor_map(out, nkeys);
    harp_cbor_uint(out, 0);
    harp_cbor_int(out, driftPpb);            /* clock_drift_ppb (host-measured gauge) */
    harp_cbor_uint(out, 3);
    harp_cbor_uint(out, (uint64_t)recovery); /* clock-recovery enum */
    if (haveAsrc) {
        /* asrc-stats (CDDL): { 0 => ratio, ?3 => phase/fill error vs setpoint, ?4 =>
         * converter quality }. The ratio is the recovered out/in; the fill error is
         * the signed frame deviation from the setpoint (the loop's phase). Quality is
         * the reader's SRC_SINC_FASTEST. (Keys 1/2 — input/output sample totals — are
         * not snapshotted by the freerun core, so a vN writer omits them.) */
        uint64_t rb = asrcRatioBits_.load(std::memory_order_relaxed);
        double ratio; memcpy(&ratio, &rb, sizeof ratio);
        double fillErr = (double)asrcFill_.load(std::memory_order_relaxed) -
                         (double)ethTargetFrames();
        harp_cbor_uint(out, 5);
        harp_cbor_map(out, 3); /* keys 0,3,4 */
        harp_cbor_uint(out, 0);
        harp_cbor_float(out, ratio);
        harp_cbor_uint(out, 3);
        harp_cbor_float(out, fillErr);
        harp_cbor_uint(out, 4);
        harp_cbor_uint(out, 2 /* SRC_SINC_FASTEST */);
    } else if (haveRatelock) {
        /* ratelock-stats: { 0 => last audio.trim ppb, 1 => fill, 2 => setpoint,
         * 3 => trim messages sent this session }. */
        harp_cbor_uint(out, 6);
        harp_cbor_map(out, 4);
        harp_cbor_uint(out, 0);
        harp_cbor_int(out, lastTrimPpb_.load(std::memory_order_relaxed));
        harp_cbor_uint(out, 1);
        harp_cbor_uint(out, (uint64_t)(audioRing_.readAvailable() / 2)); /* current fill, frames */
        harp_cbor_uint(out, 2);
        harp_cbor_uint(out, ethTargetFrames());                          /* setpoint, frames */
        harp_cbor_uint(out, 3);
        harp_cbor_uint(out, trimCount_.load(std::memory_order_relaxed));
    }
}

/* §14.4 host-context-C: usb-topology (top key 10), USB binding ONLY. Reads the
 * libusb topology of the bound device off the control path (transport_->usbTopology
 * -> host/usb_io.c). Returns false (emitting NOTHING) when the binding is not USB
 * or libusb could not resolve the device, so the assembler can size its map. §16:
 * with anonymize the controller-id (key 0) + serial (key 8) are cleared to "" IN
 * PLACE; bus/addr/port-chain/speed/VID/PID are RETAINED. CDDL usb-topology:
 * { 0 controller, 1 bus, 2 addr, 3 [port-chain], 4 speed, 6 VID, 7 PID, 8 serial }.
 * The caller pre-fetches the topology (so presence can size the bundle map upfront,
 * for deterministic definite-length CBOR) and only calls this when t.ok. */
void HarpRuntime::emitUsbTopology(harp_cbuf *out, const harp_usb_topology &t,
                                  bool anonymize) {
    harp_cbor_uint(out, 10);
    harp_cbor_map(out, 8); /* keys 0,1,2,3,4,6,7,8 */
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, anonymize ? "" : t.controller); /* §16: controller id */
    harp_cbor_uint(out, 1);
    harp_cbor_uint(out, t.bus);
    harp_cbor_uint(out, 2);
    harp_cbor_uint(out, t.addr);
    harp_cbor_uint(out, 3);
    harp_cbor_array(out, (uint64_t)(t.nports > 0 ? t.nports : 0));
    for (int i = 0; i < t.nports; i++) harp_cbor_uint(out, t.ports[i]);
    harp_cbor_uint(out, 4);
    harp_cbor_uint(out, (uint64_t)t.speed); /* usb-speed enum */
    harp_cbor_uint(out, 6);
    harp_cbor_uint(out, t.vendor_id);       /* VID RETAINED */
    harp_cbor_uint(out, 7);
    harp_cbor_uint(out, t.product_id);      /* PID RETAINED */
    harp_cbor_uint(out, 8);
    harp_cbor_text(out, anonymize ? "" : t.serial); /* §16: serial */
}

/* §14.4 host-context-C: net-topology (top key 13), Ethernet binding ONLY. CDDL
 * net-topology: { ?0 host:port, ?1 mDNS, ?2 jitter-frames, ?3 net.ptp, ?4
 * net.ptp.hw, ?5 net.offline }. The host learned the dial target (key 0) and the
 * live ring occupancy (key 2 jitter depth); mDNS/PTP are device-announced and not
 * yet learned host-side, so keys 1/3/4 are omitted (a vN writer omits what it
 * cannot fill). §16: with anonymize host:port (key 0) is cleared to "" IN PLACE;
 * the jitter depth + net.offline flag are RETAINED. Returns false (emitting
 * NOTHING) on a non-Ethernet binding — the caller only calls it on Ethernet. */
void HarpRuntime::emitNetTopology(harp_cbuf *out, bool anonymize) {
    const char *hostport = transport_ ? transport_->netEndpoint() : "";
    bool offline = wantHostPaced_.load(std::memory_order_relaxed);
    harp_cbor_uint(out, 13);
    harp_cbor_map(out, 3); /* keys 0,2,5 */
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, anonymize ? "" : (hostport ? hostport : "")); /* §16: host:port */
    harp_cbor_uint(out, 2);
    harp_cbor_uint(out, ethTargetFrames()); /* RTP jitter-buffer depth (setpoint), frames */
    harp_cbor_uint(out, 5);
    harp_cbor_bool(out, offline);           /* net.offline (host-paced TCP bounce) RETAINED */
}

/* §14.4 host-context-A — see the runtime.h doc. MIRRORS getStateBundle: a
 * deterministic CBOR map assembled under ctlMutex_, off the control path, with
 * no audio-path side effects (golden gate). */
std::vector<uint8_t> HarpRuntime::getDiagBundle(bool anonymize) {
    /* Serialize with feeder/control ops (the request() the device-section needs
     * is always issued under ctlMutex_, like every other control op). */
    std::lock_guard<std::mutex> lk(ctlMutex_);

    /* Fetch the device-section FIRST (its bytes are embedded verbatim at key 4):
     * issue `req diag.bundle` and keep the response body. A failure (device gone,
     * or no diag.bundle cap) leaves an empty section — the bundle stays valid. */
    harp_cbuf devReq, devRsp;
    harp_cbuf_init(&devReq);
    harp_cbuf_init(&devRsp);
    harp_client_req_head(&client_, &devReq, "diag.bundle", false); /* no body */
    harp_env de = {};
    bool haveDev = connected() && request(&devReq, &devRsp, &de) && de.has_body;

    /* §14.4 host-context-B: snapshot the instrumentation rings under ctlMutex_
     * (control-path read; both snapshots are non-destructive, so a later bundle
     * re-observes the same recent window). Done BEFORE the map header so the
     * key 6 / key 8 presence can size the definite-length map deterministically. */
    std::vector<StateTransition> history = sessionHistory_.snapshot(kDiagHistoryMax);
    std::vector<LogRecord> logs = runtimeLog_.snapshot(kDiagLogMax);

    harp_cbuf out;
    harp_cbuf_init(&out);

    /* §14.4 host-context-C: pre-resolve the binding-conditional sections so the
     * top map can size upfront (definite length, deterministic CBOR). key 11
     * (clock-stats) is ALWAYS present. key 10 (usb-topology) is present iff the
     * binding is USB AND libusb resolved the topology; key 13 (net-topology) iff
     * the binding is Ethernet. The two are mutually exclusive (a binding is one or
     * the other). On the §8.7 loopback the binding is Ethernet, so the test sees
     * key 13 + key 11 and NOT key 10. usbTopology() is fetched ONCE here and reused
     * by emitUsbTopology — a read-only libusb query, off the control path. */
    harp_usb_topology usbTopo;
    usbTopo.ok = false;
    bool isEth = transport_ && transport_->kind() == ShellTransport::Kind::Ethernet;
    bool haveUsbTopo = transport_ &&
                       transport_->kind() == ShellTransport::Kind::Usb &&
                       transport_->usbTopology(&usbTopo) && usbTopo.ok;

    /* Top-level diag-bundle map. v3 fills keys 0-6, 8, 9, 11 + (10 USB | 13 Eth);
     * key 7/12 (loopback-results) is a later sub-step (a vN writer omits unfilled
     * sections). key 9 is present only when a session is up (audio-config is
     * meaningless otherwise); keys 6 (session-history) + 8 (runtime logs) are
     * present only when their ring has records. The map size is declared upfront
     * (definite length, deterministic CBOR). */
    bool haveAudio = connected_.load(std::memory_order_acquire);
    uint64_t nkeys = 6; /* 0,1,2,3,4,5 */
    if (!history.empty()) nkeys++; /* key 6 */
    if (!logs.empty()) nkeys++;    /* key 8 */
    if (haveAudio) nkeys++;        /* key 9 */
    nkeys++;                       /* key 11 clock-stats (ALWAYS) */
    if (haveUsbTopo) nkeys++;      /* key 10 usb-topology (USB binding) */
    if (isEth) nkeys++;            /* key 13 net-topology (Ethernet binding) */
    harp_cbor_map(&out, nkeys);

    /* KEY 0: magic. */
    harp_cbor_uint(&out, 0);
    harp_cbor_text(&out, "harpd");

    /* KEY 1: version. v2 added session-history (key 6) + runtime logs (key 8); v3
     * adds host-context-C: clock-stats (key 11, always) + usb-topology (key 10,
     * USB) | net-topology (key 13, Ethernet) + transport enum (audio-config key 12). */
    harp_cbor_uint(&out, 1);
    harp_cbor_uint(&out, 3);

    /* KEY 2: bundle-meta { 0 => tstamp [epoch, msc], 1 => tool }. */
    harp_cbor_uint(&out, 2);
    harp_cbor_map(&out, 2);
    harp_cbor_uint(&out, 0);
    harp_cbor_array(&out, 2);
    harp_cbor_uint(&out, (uint64_t)time(nullptr)); /* epoch seconds */
    /* current MSC if streaming, else 0 — streamPos() is the stream-domain SSI. */
    harp_cbor_uint(&out, haveAudio ? streamPos() : 0);
    harp_cbor_uint(&out, 1);
    harp_cbor_text(&out, "harp-runtime (device-assembled device-section)");

    /* KEY 3: anonymized flag. */
    harp_cbor_uint(&out, 3);
    harp_cbor_bool(&out, anonymize);

    /* KEY 4: device-section. Non-anon: the device's response body VERBATIM (the
     * byte-identical conformance gate). Anon: decode-reencode with §16 leaves
     * cleared (the seam exception — NOT kept verbatim). harp_cbuf_put COPIES the
     * bytes, so de.body can be freed after this. */
    harp_cbor_uint(&out, 4);
    if (haveDev) {
        if (anonymize) {
            if (!anon_device_section(&out, de.body, de.body_len))
                harp_cbor_map(&out, 0); /* malformed -> empty (still valid CBOR) */
        } else {
            harp_cbuf_put(&out, de.body, de.body_len);
        }
    } else {
        harp_cbor_map(&out, 0); /* device not ready / no cap -> empty device-section */
    }
    harp_cbuf_free(&devReq);
    harp_cbuf_free(&devRsp);

    /* KEY 5: host-counters (keys 0-6). Numeric leaves — no PII, unchanged by the
     * anon pass. All read here under ctlMutex_. */
    harp_cbor_uint(&out, 5);
    harp_cbor_map(&out, 7);
    harp_cbor_uint(&out, 0);
    harp_cbor_uint(&out, underruns_.load(std::memory_order_relaxed)); /* host_underruns */
    harp_cbor_uint(&out, 1);
    harp_cbor_uint(&out, padSamples_.load(std::memory_order_relaxed)); /* pad_debt_samples */
    harp_cbor_uint(&out, 2);
    harp_cbor_uint(&out, evDrops_.load(std::memory_order_relaxed)); /* event_drops */
    harp_cbor_uint(&out, 3);
    harp_cbor_uint(&out, framesSent_); /* frames_sent (audio-thread member, read off-path) */
    harp_cbor_uint(&out, 4);
    harp_cbor_uint(&out, framesRecvAtomic_.load(std::memory_order_relaxed)); /* frames_recv */
    harp_cbor_uint(&out, 5);
    harp_cbor_uint(&out, sessionGen_.load(std::memory_order_relaxed)); /* session_generation */
    harp_cbor_uint(&out, 6);
    harp_cbor_uint(&out, renegCount_.load(std::memory_order_acquire)); /* audio_renegotiations */

    /* KEY 6: session-history — the §12.1 state-machine transition ring. Each
     * record is { 0 => [epoch, msc], 1 => from-state, 2 => to-state, 3 =>
     * reason, ?4 => detail }. §16: with anonymize the free-text detail (key 4)
     * is cleared to "" IN PLACE — the numeric tstamp/from/to/reason are RETAINED
     * (reveal whether, not what). Emitted only when the ring has records. */
    if (!history.empty()) {
        harp_cbor_uint(&out, 6);
        harp_cbor_array(&out, history.size());
        for (const StateTransition &t : history) {
            harp_cbor_map(&out, 5); /* keys 0,1,2,3,4 — detail always present (""=redacted) */
            harp_cbor_uint(&out, 0);
            harp_cbor_array(&out, 2);
            harp_cbor_uint(&out, t.tstamp_epoch);
            harp_cbor_uint(&out, t.tstamp_msc);
            harp_cbor_uint(&out, 1);
            harp_cbor_uint(&out, t.from_state);
            harp_cbor_uint(&out, 2);
            harp_cbor_uint(&out, t.to_state);
            harp_cbor_uint(&out, 3);
            harp_cbor_uint(&out, t.reason_code);
            harp_cbor_uint(&out, 4);
            harp_cbor_text(&out, anonymize ? "" : t.detail); /* §16: clear detail in place */
        }
    }

    /* KEY 8: runtime logs — the §14.4 RuntimeLog ring. Each record is { 0 =>
     * msc, 1 => level, 2 => tag, 3 => msg, ?4 => [epoch, msc] wall-stamp }. §16:
     * with anonymize the free-text msg (key 3) is cleared to "" IN PLACE; the
     * tag (key 2), level, msc and wall-stamp are RETAINED. Emitted only when the
     * ring has records. */
    if (!logs.empty()) {
        harp_cbor_uint(&out, 8);
        harp_cbor_array(&out, logs.size());
        for (const LogRecord &l : logs) {
            harp_cbor_map(&out, 5); /* keys 0,1,2,3,4 */
            harp_cbor_uint(&out, 0);
            harp_cbor_uint(&out, l.msc);
            harp_cbor_uint(&out, 1);
            harp_cbor_uint(&out, l.level);
            harp_cbor_uint(&out, 2);
            harp_cbor_text(&out, l.tag); /* §16: tag RETAINED (reveal whether, not what) */
            harp_cbor_uint(&out, 3);
            harp_cbor_text(&out, anonymize ? "" : l.msg); /* §16: clear msg in place */
            harp_cbor_uint(&out, 4);
            harp_cbor_array(&out, 2); /* tstamp [epoch, msc] (wall-clock correlation) */
            harp_cbor_uint(&out, l.tstamp_epoch);
            harp_cbor_uint(&out, l.msc);
        }
    }

    /* KEY 9: audio-config (host-owned DAW view; keys 0-12). Only when a session
     * is up (otherwise these read defaults that misdescribe a dead session). v3
     * adds key 12 (transport enum: 0 usb, 1 ethernet) — the EXPLICIT binding
     * selector so a decoder can tell a USB v3 bundle from an Ethernet one (the v2
     * ambiguity the design flagged); it pairs with key 10 (USB) vs key 13 (Eth). */
    if (haveAudio) {
        harp_cbor_uint(&out, 9);
        harp_cbor_map(&out, 13);
        harp_cbor_uint(&out, 0);
        harp_cbor_uint(&out, rate_); /* DAW sample rate */
        harp_cbor_uint(&out, 1);
        harp_cbor_uint(&out, kBlock); /* DAW pacing block (256) */
        harp_cbor_uint(&out, 2);
        harp_cbor_uint(&out, freeRunning_.load(std::memory_order_relaxed) ? 0 : 1); /* clock-mode */
        harp_cbor_uint(&out, 3); /* active out-slots (the audio.start union) */
        harp_cbor_array(&out, unionSlots_.size());
        for (uint32_t slot : unionSlots_) harp_cbor_uint(&out, slot);
        harp_cbor_uint(&out, 4); /* active in-slots: H->D only, empty */
        harp_cbor_array(&out, 0);
        harp_cbor_uint(&out, 5);
        harp_cbor_uint(&out, targetFrames_); /* target ring depth (frames) */
        harp_cbor_uint(&out, 6);
        harp_cbor_uint(&out, 0); /* DAW PDC latency (not reported in v1) */
        harp_cbor_uint(&out, 7);
        harp_cbor_bool(&out, bitExact_); /* 1:1 rate-lock vs ASRC */
        harp_cbor_uint(&out, 8);
        harp_cbor_bool(&out, freeRunning_.load(std::memory_order_relaxed)); /* free-running */
        harp_cbor_uint(&out, 9);
        harp_cbor_bool(&out, wantHostPaced_.load(std::memory_order_relaxed)); /* offline */
        harp_cbor_uint(&out, 10);
        harp_cbor_uint(&out, 0); /* sample format (0 = float32) */
        harp_cbor_uint(&out, 11);
        harp_cbor_uint(&out, unionWidth_.load(std::memory_order_relaxed)); /* RTP columns */
        harp_cbor_uint(&out, 12);
        harp_cbor_uint(&out, isEth ? 1u : 0u); /* transport enum: 0 usb, 1 ethernet (§4.4) */
    }

    /* KEY 10: usb-topology — USB binding ONLY (absent on the §8.7 Ethernet
     * loopback). Emitted from the topology fetched once above. §16 clears the
     * controller-id (0) + serial (8); bus/addr/port-chain/speed/VID/PID RETAINED. */
    if (haveUsbTopo) emitUsbTopology(&out, usbTopo, anonymize);

    /* KEY 11: clock-stats — ALWAYS. The host's recovery/correlation snapshot
     * (recovery mode + drift + asrc-stats|ratelock-stats). No PII (the §16 pass
     * leaves it untouched). MUST sit AFTER key 10 and BEFORE key 13 (integer keys
     * ascending — deterministic CBOR). */
    emitClockStats(&out);

    /* KEY 13: net-topology — Ethernet binding ONLY (the §8.7 loopback path). §16
     * clears host:port (key 0); the jitter depth + net.offline flag RETAINED. */
    if (isEth) emitNetTopology(&out, anonymize);

    std::vector<uint8_t> result(out.buf, out.buf + out.len);
    harp_cbuf_free(&out);
    return result;
}

/* Decode a flat { id => float } param map at the decoder's cursor into out. */
static void decode_param_map(harp_cdec *pd,
                             std::vector<std::pair<uint32_t, float>> *out) {
    uint64_t pn;
    if (!harp_cdec_map(pd, &pn)) return;
    for (uint64_t k = 0; k < pn; k++) {
        uint64_t id;
        double v;
        if (!harp_cdec_uint(pd, &id) || !harp_cdec_float(pd, &v)) break;
        out->push_back({(uint32_t)id, (float)v});
    }
}

/* Parse a "params" blob payload into part 0's (id,value) pairs, tolerating
 * both the NEW multitimbral format and the OLD flat one (SHARED CONTRACT):
 *   NEW: CBOR map { partIndex => { id => value } } — extract part 0 (a single
 *        instance shows its own part; multi-part UI is later).
 *   OLD: CBOR map { id => value } — read it wholesale as part 0 (back-compat).
 * The two are told apart by the first value's CBOR major type: a map (5) is
 * the per-part format, anything else (a float) is the flat one. */
static void decode_params_payload(const uint8_t *pl, size_t pll,
                                  std::vector<std::pair<uint32_t, float>> *out) {
    harp_cdec pd;
    harp_cdec_init(&pd, pl, pll);
    uint64_t pn;
    if (!harp_cdec_map(&pd, &pn) || pn == 0) return;
    uint64_t firstKey;
    if (!harp_cdec_uint(&pd, &firstKey)) return; /* both formats key on a uint */
    if (harp_cdec_peek(&pd) == 5) { /* NEW: { partIndex => { id => value } } */
        /* firstKey was a part index; decode its inner map iff it's part 0,
         * else skip it, then scan the remaining parts for part 0. */
        if (firstKey == 0)
            decode_param_map(&pd, out);
        else
            harp_cdec_skip(&pd); /* skip part firstKey's inner map */
        for (uint64_t k = 1; k < pn && !pd.err; k++) {
            uint64_t part;
            if (!harp_cdec_uint(&pd, &part)) break;
            if (part == 0)
                decode_param_map(&pd, out); /* part 0: the part we display */
            else if (!harp_cdec_skip(&pd)) /* any other part: skip its map */
                break;
        }
        return;
    }
    /* OLD flat { id => value }: firstKey was an id; its value is a float, and
     * the rest follow as id/value pairs. */
    double v;
    if (!harp_cdec_float(&pd, &v)) return;
    out->push_back({(uint32_t)firstKey, (float)v});
    for (uint64_t k = 1; k < pn; k++) {
        uint64_t id;
        if (!harp_cdec_uint(&pd, &id) || !harp_cdec_float(&pd, &v)) break;
        out->push_back({(uint32_t)id, (float)v});
    }
}

/* Walk a store: target snapshot -> tree -> "params" blob -> (id,value)
 * pairs. Shared by setStateBundle and the runtime-free controller path. */
void HarpRuntime::paramsFromStore(harp_store *store, const harp_hash &target,
                                  std::vector<std::pair<uint32_t, float>> &out) {
    harp_cbuf enc;
    harp_cbuf_init(&enc);
    harp_hash root;
    if (harp_store_get(store, &target, &enc) == 0 &&
        harp_obj_parse_snapshot_root(enc.buf, enc.len, &root)) {
        harp_cbuf tree;
        harp_cbuf_init(&tree);
        if (harp_store_get(store, &root, &tree) == 0) {
            struct Ctx {
                harp_store *store;
                std::vector<std::pair<uint32_t, float>> *out;
            } ctx{store, &out};
            harp_obj_tree_foreach(
                tree.buf, tree.len,
                [](const char *name, size_t nl, const harp_hash *h, uint32_t kind,
                   void *ud) -> bool {
                    if (nl != 6 || memcmp(name, "params", 6) != 0 || kind != 0)
                        return true;
                    auto *c = (Ctx *)ud;
                    harp_cbuf blob;
                    harp_cbuf_init(&blob);
                    if (harp_store_get(c->store, h, &blob) == 0) {
                        const uint8_t *pl;
                        size_t pll;
                        if (harp_obj_parse_blob(blob.buf, blob.len, nullptr, nullptr, &pl,
                                                &pll))
                            decode_params_payload(pl, pll, c->out);
                    }
                    harp_cbuf_free(&blob);
                    return true;
                },
                &ctx);
            harp_cbuf_free(&tree);
        }
    }
    harp_cbuf_free(&enc);
}

/* Runtime-free bundle param extraction for the VST3 controller (a separate
 * object that must NOT open USB or own a runtime). Ingests the bundle's
 * embedded object closure into `store` (idempotent, content-addressed) and
 * returns the live/project knob values. */
bool HarpRuntime::bundleParams(const uint8_t *data, size_t len, harp_store *store,
                               std::vector<std::pair<uint32_t, float>> &out) {
    out.clear();
    harp_cdec d;
    harp_cdec_init(&d, data, len);
    uint64_t n;
    if (!harp_cdec_map(&d, &n)) return false;
    harp_hash target{};
    bool haveTarget = false, magicOk = false;
    for (uint64_t i = 0; i < n && !d.err; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        if (key == 0) {
            const char *s;
            size_t sl;
            if (!harp_cdec_text(&d, &s, &sl)) return false;
            magicOk = sl == 5 && memcmp(s, BUNDLE_MAGIC, 5) == 0;
        } else if (key == 3) {
            uint64_t alen;
            if (!harp_cdec_array(&d, &alen)) return false;
            for (uint64_t j = 0; j < alen; j++) {
                harp_ref r;
                if (!harp_ref_decode(&d, &r)) return false;
                if (strcmp(r.name, LIVE_REF) == 0 && !r.unborn) {
                    target = r.hash;
                    haveTarget = true;
                }
            }
        } else if (key == 4) {
            if (harp_cdec_peek_null(&d)) {
                harp_cdec_null(&d);
                continue;
            }
            uint64_t alen;
            if (!harp_cdec_array(&d, &alen)) return false;
            for (uint64_t j = 0; j < alen; j++) {
                const uint8_t *span;
                size_t spanLen;
                if (!harp_cdec_span(&d, &span, &spanLen)) return false;
                harp_store_put(store, span, spanLen, nullptr);
            }
        } else if (!harp_cdec_skip(&d))
            return false;
    }
    if (!magicOk || !haveTarget) return false;
    paramsFromStore(store, target, out);
    return true;
}

/* Runtime-free read of the bundle's wanted usb serial (§15.3 key 5 ->
 * {0 vid, 1 pid, 2 serial}). Mirrors setStateBundle's key-5 decode but takes
 * nothing and touches no state — the registry calls it before a runtime
 * exists. Returns "" on any miss (no key 5, no serial, or a parse failure):
 * "" means "no explicit target", which the registry maps to a fresh unshared
 * runtime, exactly as an env-less / bundle-less instance gets today. */
std::string HarpRuntime::bundleWantedSerial(const uint8_t *data, size_t len) {
    if (!data || !len) return std::string();
    harp_cdec d;
    harp_cdec_init(&d, data, len);
    uint64_t n;
    if (!harp_cdec_map(&d, &n)) return std::string();
    for (uint64_t i = 0; i < n && !d.err; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return std::string();
        if (key == 5) { /* usb-identity: {0 vid, 1 pid, 2 serial} */
            uint64_t mn, mk;
            const char *s = nullptr;
            size_t sl = 0;
            if (!harp_cdec_map(&d, &mn)) return std::string();
            for (uint64_t j = 0; j < mn && !d.err; j++) {
                if (!harp_cdec_uint(&d, &mk)) return std::string();
                if (mk == 2) {
                    if (!harp_cdec_text(&d, &s, &sl)) return std::string();
                } else if (!harp_cdec_skip(&d))
                    return std::string();
            }
            return std::string(s ? s : "", s ? sl : 0);
        }
        if (!harp_cdec_skip(&d)) return std::string();
    }
    return std::string();
}

bool HarpRuntime::setStateBundle(const uint8_t *data, size_t len) {
    if (!storeOk_ || !len) return false;
    harp_cdec d;
    harp_cdec_init(&d, data, len);
    uint64_t n;
    if (!harp_cdec_map(&d, &n)) return false;
    bool magicOk = false;
    harp_hash target{};
    bool haveTarget = false;
    for (uint64_t i = 0; i < n && !d.err; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        switch (key) {
            case 0: {
                const char *s;
                size_t sl;
                if (!harp_cdec_text(&d, &s, &sl)) return false;
                magicOk = sl == 5 && memcmp(s, BUNDLE_MAGIC, 5) == 0;
                break;
            }
            case 2: { /* identity-expectation: warn on param-map drift
                       * (§9.3: shells MUST warn and map conservatively —
                       * the device maps by id, so old automation lands on
                       * matching ids and new params keep their values) */
                uint64_t in;
                if (!harp_cdec_map(&d, &in)) return false;
                for (uint64_t j = 0; j < in; j++) {
                    uint64_t ik;
                    if (!harp_cdec_uint(&d, &ik)) return false;
                    if (ik == 3) { /* engine map */
                        uint64_t en;
                        if (!harp_cdec_map(&d, &en)) return false;
                        for (uint64_t k2 = 0; k2 < en; k2++) {
                            uint64_t ek;
                            if (!harp_cdec_uint(&d, &ek)) return false;
                            if (ek == 2) {
                                const uint8_t *hp;
                                size_t hl;
                                if (!harp_cdec_bytes(&d, &hp, &hl)) return false;
                                if (hl == HARP_HASH_LEN && connected() &&
                                    memcmp(hp, paramMapHash_.b, HARP_HASH_LEN) != 0)
                                    log_msg("recall: project's param map differs "
                                            "from the device's (engine update?) — "
                                            "applying matching ids only");
                            } else if (!harp_cdec_skip(&d))
                                return false;
                        }
                    } else if (!harp_cdec_skip(&d))
                        return false;
                }
                break;
            }
            case 3: { /* refs */
                uint64_t alen;
                if (!harp_cdec_array(&d, &alen)) return false;
                for (uint64_t j = 0; j < alen; j++) {
                    harp_ref r;
                    if (!harp_ref_decode(&d, &r)) return false;
                    if (strcmp(r.name, LIVE_REF) == 0 && !r.unborn) {
                        target = r.hash;
                        haveTarget = true;
                    }
                }
                break;
            }
            case 5: { /* usb-identity (selection key): {0 vid, 1 pid, 2 serial} */
                uint64_t mn, mk, vid = 0, pid = 0;
                const char *s = nullptr;
                size_t sl = 0;
                if (!harp_cdec_map(&d, &mn)) return false;
                for (uint64_t j = 0; j < mn; j++) {
                    if (!harp_cdec_uint(&d, &mk)) return false;
                    if (mk == 0) harp_cdec_uint(&d, &vid);
                    else if (mk == 1) harp_cdec_uint(&d, &pid);
                    else if (mk == 2) harp_cdec_text(&d, &s, &sl);
                    else harp_cdec_skip(&d);
                }
                {
                    std::lock_guard<std::mutex> slk(bundleMutex_);
                    wantUsbVid_ = (uint16_t)vid;
                    wantUsbPid_ = (uint16_t)pid;
                    wantUsbSerial_.assign(s ? s : "", s ? sl : 0);
                    wantUsb_ = !wantUsbSerial_.empty();
                }
                break;
            }
            case 4: { /* embedded objects -> local store */
                if (harp_cdec_peek_null(&d)) {
                    harp_cdec_null(&d);
                    break;
                }
                uint64_t alen;
                if (!harp_cdec_array(&d, &alen)) return false;
                for (uint64_t j = 0; j < alen; j++) {
                    const uint8_t *span;
                    size_t spanLen;
                    if (!harp_cdec_span(&d, &span, &spanLen)) return false;
                    harp_store_put(&store_, span, spanLen, nullptr);
                }
                break;
            }
            default:
                if (!harp_cdec_skip(&d)) return false;
        }
    }
    if (!magicOk || !haveTarget) return false;

    /* surface knob values for the controller (find the params blob) */
    {
        std::lock_guard<std::mutex> slk(bundleMutex_);
        hasBundle_ = true;
        bundleTarget_ = target;
        bundleParams_.clear();
        paramsFromStore(&store_, target, bundleParams_);
    }

    if (connected()) {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        return pushStateLocked(target);
    }
    log_msg("recall bundle staged (device offline); will apply on connect");
    return true;
}

bool HarpRuntime::bundleParam(uint32_t id, float &value) {
    std::lock_guard<std::mutex> slk(bundleMutex_);
    for (auto &kv : bundleParams_)
        if (kv.first == id) {
            value = kv.second;
            return true;
        }
    return false;
}
