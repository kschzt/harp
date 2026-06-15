#include "runtime.h"
#include "ump.h"

#ifdef __APPLE__
#include <pthread/qos.h>
#endif
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
    return true;
}

bool HarpRuntime::audioStart(uint32_t rate) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client_, &req, "audio.start", true);
    harp_cbor_map(&req, 6);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, rate);
    harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, kBlock);
    harp_cbor_uint(&req, 2);
    harp_cbor_uint(&req, kTargetDepthFrames);
    harp_cbor_uint(&req, 3);
    harp_cbor_array(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, 4);
    harp_cbor_array(&req, 0);
    harp_cbor_uint(&req, 5);
    harp_cbor_uint(&req, 1); /* host-paced */
    harp_env e;
    bool ok = request(&req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return ok;
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

/* Param set as a §9.4 event message: fire-and-forget, no response.
 * ts is an SSI (0 = "now"). Encode-only; the feeder frames and batches. */
void HarpRuntime::encodeParamEvent(harp_cbuf *m, uint32_t id, float v, uint64_t ts) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 1); /* etype: param */
    harp_cbor_map(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, v);
}

/* Drain any asynchronous inbound link traffic: evt echoes (-> echoRing_)
 * and notifications. Non-blocking-ish: one short-timeout fill, then
 * consume complete messages. */
void HarpRuntime::pollEcho() {
    std::unique_lock<std::mutex> lk(ctlMutex_, std::try_to_lock);
    if (!lk.owns_lock()) return; /* a state op owns the link; echoes wait */
    if (!harp_usb_link_poll(io_, 1)) return;
    while (harp_usb_link_pending(io_) > 0) {
        uint8_t stream;
        if (harp_link_recv(io_, &link_, &stream, &msg_) != 0) {
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
            uint64_t n, key, id = 0;
            double v = 0;
            bool ok = harp_cdec_map(&dec, &n);
            for (uint64_t i = 0; ok && i < n; i++) {
                if (!harp_cdec_uint(&dec, &key)) break;
                if (key == 0)
                    ok = harp_cdec_uint(&dec, &id);
                else if (key == 1)
                    ok = harp_cdec_float(&dec, &v);
                else
                    ok = harp_cdec_skip(&dec);
            }
            if (ok) echoRing_.push({(uint32_t)id, (float)v});
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
harp_io *HarpRuntime::selectDevice() {
    /* reconnect: pinned to the exact unit this instance already owns — the
     * same-model fallback must NOT fire here, or a replug could let this
     * instance steal a sibling track's device. */
    if (!boundSerial_.empty())
        return harp_usb_open_match_ctx(usbCtx_, boundSerial_.c_str(), false, 0, 0);

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
        return io; /* a known model is never satisfied by a different model */
    }
    /* fresh instance (or a bundle predating usb-identity): first unclaimed
     * HARP device of any model — it adopts whatever is there and records
     * it on first save. */
    return harp_usb_open_match_ctx(usbCtx_, nullptr, false, 0, 0);
}

/* One connection attempt: claim, hello, re-assert the project bundle,
 * start the stream, spawn the reader. Caller: start() or supervisor(). */
bool HarpRuntime::sessionUp() {
    io_ = selectDevice();
    if (!io_) return false;
    if (!harp_usb_has_audio(io_)) {
        harp_usb_close(io_);
        io_ = nullptr;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        /* capture the bound device's USB identity (vid:pid:serial) UNDER
         * ctlMutex_ — getStateBundle reads it there on the main thread, so
         * an unlocked write here would be a cross-thread race on the
         * std::string (the project holds itself to zero benign races). */
        harp_usb_devinfo di;
        if (harp_usb_devident(io_, &di)) {
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
        harp_client_init(&client_, io_, &link_, storeOk_ ? &store_ : nullptr, nullptr,
                         nullptr);
        if (!helloAndIdentity()) {
            log_msg("hello failed");
            harp_client_free(&client_);
            harp_usb_close(io_);
            io_ = nullptr;
            return false;
        }
        log_msg("connected: %s %s (serial %s, engine %s %s)", vendorName_.c_str(),
                productName_.c_str(), serial_.c_str(), engineId_.c_str(),
                engineVer_.c_str());
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
            if (pushStateLocked(target))
                log_msg("project state re-asserted");
            else
                log_msg("project state apply failed (will retry on reconnect)");
        }
        if (!audioStart(rate_)) {
            log_msg("audio.start failed");
            harp_client_free(&client_);
            harp_usb_close(io_);
            io_ = nullptr;
            return false;
        }
    }
    /* drain any stale stream bytes before pacing */
    uint8_t junk[16384];
    while (harp_usb_audio_read(io_, junk, sizeof junk, 30) > 0) {}

    /* new session = new stream = new SSI time domain (§7.1). Events still
     * queued from the previous session carry STALE timestamps — drain
     * them (no pump is running yet, so consuming here is safe), and the
     * fence sequence space restarts from zero on both sides. */
    {
        TimedEv stale;
        while (timedRing_.pop(stale)) {}
    }
    evtQueuedSeq_.store(0, std::memory_order_release);
    ssi_ = framesSent_ = framesRecv_ = 0;
    framesRecvAtomic_.store(0, std::memory_order_relaxed);
    ssiRead_.store(0, std::memory_order_relaxed);
    padDebtFloats_ = 0;
    ahead_ = 2; /* small fixed pipeline; the reader thread keeps RTT short */
    audioRing_.clear();
    /* connected_ goes true BEFORE the pump spawns: its run loop gates on
     * it, and spawning first would race a clean instant exit */
    connected_.store(true, std::memory_order_release);
    readerThread_ = std::thread([this] { reader(); });
    eventPumpThread_ = std::thread([this] { eventPump(); });
    return true;
}

/* Tear a session down: reap the reader, orderly audio.stop if the device
 * is still talking to us, release the claim. Safe on a dead transport. */
void HarpRuntime::sessionDown() {
    bool wasConnected = connected_.exchange(false, std::memory_order_acq_rel);
    if (readerThread_.joinable()) readerThread_.join();
    if (eventPumpThread_.joinable()) eventPumpThread_.join();
    if (!io_) return;
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (wasConnected) {
        audioStopLocked();
        /* drain the tail of the stream so the device thread can park */
        uint8_t junk[16384];
        int quiet = 0;
        while (quiet < 2) {
            int r = harp_usb_audio_read(io_, junk, sizeof junk, 80);
            if (r < 0) break;
            quiet = (r == 0) ? quiet + 1 : 0;
        }
    }
    harp_client_free(&client_);
    harp_usb_close(io_);
    io_ = nullptr;
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
        if (connected_.load(std::memory_order_acquire)) {
            everConnected = true;
            feeder(); /* returns when !running_ or the transport died */
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
    if (running_.load()) return connected();
    harp_plat_init(); /* hi-res timers for the sub-ms pacing/idle waits (Windows) */
    rate_ = sampleRate;
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
     * them still queued is how notes get stuck. Bounded wait. */
    if (running_.load(std::memory_order_acquire) && connected()) {
        for (int i = 0; i < 100 && !timedRing_.empty(); i++) {
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
            (unsigned long long)underruns_.load(),
            (unsigned long long)padSamples_.load());
}

/* ---------------- audio thread side ---------------- */

void HarpRuntime::queueParamSet(uint32_t id, float v, uint64_t ts) {
    if (timedRing_.push({0, id, v, ts, 0}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueRamp(uint32_t id, float target, uint64_t start, uint64_t end) {
    if (timedRing_.push({1, id, target, start, end}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueNote(uint32_t word, uint64_t ts) {
    if (timedRing_.push({2, word, 0.0f, ts, 0})) {
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

void HarpRuntime::queueTransport(uint32_t flags, double tempo, double ppq,
                                 uint64_t ts) {
    uint64_t ppqBits;
    memcpy(&ppqBits, &ppq, sizeof ppqBits);
    if (timedRing_.push({3, flags, (float)tempo, ts, ppqBits}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
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
        queueTransport(flags, tempo, ppq, base);
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

/* Ramp event (§9.4): etype 5, msg tstamp = start, body {param, target, end}. */
void HarpRuntime::encodeRampEvent(harp_cbuf *m, uint32_t id, float target,
                                  uint64_t start, uint64_t end) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, start);
    harp_cbor_uint(m, 5); /* etype: ramp */
    harp_cbor_map(m, 3);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, target);
    harp_cbor_uint(m, 2);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, end);
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

size_t HarpRuntime::pullAudio(float *dst, size_t nFrames) {
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

size_t HarpRuntime::pullAudioBlocking(float *dst, size_t nFrames, unsigned timeoutMs) {
    settlePadDebt();
    size_t want = nFrames * 2;
    size_t got = 0;
    unsigned waited = 0;
    ssiRead_.fetch_add(nFrames, std::memory_order_relaxed);
    while (got < want) {
        got += audioRing_.read(dst + got, want - got);
        if (got >= want) break;
        if (!connected_.load(std::memory_order_acquire) || waited >= timeoutMs) {
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

/* ---------------- feeder thread ---------------- */

void HarpRuntime::feeder() {
    /* (QoS is set by the supervisor thread, which runs this.) Exits when
     * the plugin stops OR the session dies — the supervisor reconnects. */
#ifdef __APPLE__
    WgState wg;
#endif
    while (running_.load(std::memory_order_relaxed) &&
           connected_.load(std::memory_order_relaxed)) {
#ifdef __APPLE__
        wgMaintain(wg);
#endif
        bool didWork = false;

        /* 1. inbound async traffic FIRST: keeping the IN direction drained
         * means the device is never blocked writing to us — which means OUR
         * writes below never stall (§4.2.1; learned the hard way under
         * automation flood) */
        pollEcho();

        /* (events + panic live on the eventPump thread: an event's wire
         * deadline is ~one DAW block, and the pacing writes below can
         * stall 8 ms in drain-on-stall — head-of-line blocking here is
         * exactly how block-256 sessions leaked evt_late) */

        /* 2. pace: ring to target depth, small fixed pipeline on top. The
         * reader thread keeps an audio-IN read permanently pending, so the
         * device's response writes land instantly and its pacing turnaround
         * is just render time. */
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
            uint32_t seq = evtQueuedSeq_.load(std::memory_order_acquire);
            ph[HARP_AUDIO_HDR_LEN + 0] = (uint8_t)seq;
            ph[HARP_AUDIO_HDR_LEN + 1] = (uint8_t)(seq >> 8);
            ph[HARP_AUDIO_HDR_LEN + 2] = (uint8_t)(seq >> 16);
            ph[HARP_AUDIO_HDR_LEN + 3] = (uint8_t)(seq >> 24);
            if (!harp_usb_audio_write(io_, ph, sizeof ph, 8)) break;
            ssi_ += kBlock;
            framesSent_++;
            inFlight++;
            didWork = true;
        }

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
            harp_link_send(io_, HARP_STREAM_EVT, m.buf, m.len);
            harp_cbuf_free(&m);
            log_msg("WARNING: note-off lost to overflow; sent all-notes-off");
        }

        /* timestamped events (params, ramps, notes — §9.2/§9.4/§9.10),
         * batched into ONE framed bulk write (per-event writes starve the
         * pipe); the cap only bounds the write size — the loop comes
         * straight back for the rest */
        harp_cbuf_reset(&batch);
        TimedEv te;
        int sent = 0;
        for (; sent < 64 && timedRing_.pop(te); sent++) {
            harp_cbuf_reset(&msgbuf);
            if (te.kind == 0)
                encodeParamEvent(&msgbuf, te.a, te.v, te.ts);
            else if (te.kind == 1)
                encodeRampEvent(&msgbuf, te.a, te.v, te.ts, te.end);
            else if (te.kind == 3) {
                double ppq;
                memcpy(&ppq, &te.end, sizeof ppq);
                encodeTransportEvent(&msgbuf, te.a, te.v, ppq, te.ts);
            } else
                encodeUmpEvent(&msgbuf, te.a, te.ts);
            harp_frame_hdr h = {HARP_FRAME_FVER, HARP_STREAM_EVT, HARP_FLAG_FIN,
                                (uint32_t)msgbuf.len};
            uint8_t hdr[HARP_FRAME_HDR_LEN];
            harp_frame_hdr_encode(&h, hdr);
            harp_cbuf_put(&batch, hdr, sizeof hdr);
            harp_cbuf_put(&batch, msgbuf.buf, msgbuf.len);
        }
        if (sent) {
            std::lock_guard<std::mutex> lk(ctlMutex_);
            if (!io_->write_all(io_, batch.buf, batch.len)) {
                log_msg("event write failed; device gone?");
                connected_.store(false, std::memory_order_release);
            }
            didWork = true;
        }

        if (!didWork) {
            harp_sleep_ns(500000ull); /* 0.5 ms — well inside the one-block budget */
        }
    }
    harp_cbuf_free(&msgbuf);
    harp_cbuf_free(&batch);
}

void HarpRuntime::reader() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    uint8_t acc[65536];
    size_t accLen = 0;
#ifdef __APPLE__
    WgState wg;
#endif
    while (running_.load(std::memory_order_relaxed)) {
#ifdef __APPLE__
        wgMaintain(wg);
#endif
        int r = harp_usb_audio_read(io_, acc + accLen, (int)(sizeof acc - accLen), 100);
        if (r < 0) {
            log_msg("audio stream read failed; device gone?");
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
            audioRing_.write((const float *)(acc + off + HARP_AUDIO_HDR_LEN),
                             (size_t)h.nsamples * 2);
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
        return true;
    }
    log_msg("recall: mismatch%s -> Push with archive (v0 auto-resolve)",
            live.dirty ? " + dirty edits" : "");
    harp_hash deviceHead = live.hash;
    if (live.dirty && !snapshotLocked(&deviceHead)) return false;

    if (!live.unborn) {
        char archive[96];
        time_t now = time(nullptr);
        struct tm tm;
        harp_gmtime(now, &tm);
        snprintf(archive, sizeof archive, "archive/%04d-%02d-%02dT%02d:%02d:%02dZ",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                 tm.tm_sec);
        if (harp_client_refset(&client_, archive, nullptr, &deviceHead, true, nullptr) !=
            0)
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
                                 &target, live.unborn, nullptr) == 0;
    if (ok) log_msg("recall: live/project restored");
    /* The bundle reference is PERSISTENT, not consumed: the DAW project's
     * notion of state re-asserts on every reconnect ("Live wins") — with
     * the archive step keeping it loss-free. It only moves when a save
     * pulls a new head (getStateBundle) or a new set loads. */
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
    if (!connected()) {
        /* offline: re-emit staged bundle if we have one */
        std::lock_guard<std::mutex> slk(bundleMutex_);
        return false; /* v0: nothing meaningful to save offline */
    }
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
                                                &pll)) {
                            harp_cdec pd;
                            harp_cdec_init(&pd, pl, pll);
                            uint64_t pn;
                            if (harp_cdec_map(&pd, &pn))
                                for (uint64_t k = 0; k < pn; k++) {
                                    uint64_t id;
                                    double v;
                                    if (!harp_cdec_uint(&pd, &id) || !harp_cdec_float(&pd, &v))
                                        break;
                                    c->out->push_back({(uint32_t)id, (float)v});
                                }
                        }
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
