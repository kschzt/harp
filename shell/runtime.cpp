#include "runtime.h"
#include "ump.h"
#include "usb_transport.h" /* the concrete USB binding selectDevice() wraps */
#include "eth_transport.h" /* the §8.7 Ethernet binding (bit-exact host-locked) */

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
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client_, &req, "audio.start", true);
    if (freeRunning_) {
        /* §8.7 Ethernet / bit-exact: free-running (key 5 = 0) with the RTP dest
         * port (key 6). The device emits its stereo main mix over RTP to us; the
         * host plays it 1:1 from EthTransport's FIFO — no per-part union, no
         * host-paced pacing. Mirrors the proven eth-bitexact-test request. */
        harp_cbor_map(&req, 4);
        harp_cbor_uint(&req, 0);
        harp_cbor_uint(&req, rate);
        harp_cbor_uint(&req, 1);
        harp_cbor_uint(&req, kBlock);
        harp_cbor_uint(&req, 5);
        harp_cbor_uint(&req, 0); /* free-running */
        harp_cbor_uint(&req, 6);
        harp_cbor_uint(&req, (uint64_t)transport_->audioPort()); /* RTP dest port */
    } else {
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
    } else {
        renegCount_.fetch_add(1, std::memory_order_release);
        log_msg("re-negotiated audio stream: %zu union slot(s) now streamed",
                unionSlots_.size());
    }
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
/* Wrap a freshly claimed USB transport (or a failed claim) as a ShellTransport.
 * Step 1 only ever builds UsbTransport here; the Ethernet binding is selected
 * elsewhere (next step). nullptr in => nullptr out (no device was claimed). */
static ShellTransport *wrapUsb(harp_io *io) { return io ? new UsbTransport(io) : nullptr; }

ShellTransport *HarpRuntime::selectDevice() {
    /* Ethernet binding (§8.7): HARP_ETH_DEVICE=HOST:PORT routes to the RTP/TCP
     * transport instead of USB. Unset (the default, and every golden run) falls
     * straight through to the USB path below — byte-identical. */
    if (const char *eth = getenv("HARP_ETH_DEVICE"))
        if (eth[0]) return EthTransport::dial(eth);

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
    /* fresh instance (or a bundle predating usb-identity): first unclaimed
     * HARP device of any model — it adopts whatever is there and records
     * it on first save. */
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
            delete transport_;
            transport_ = nullptr;
            return false;
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
    /* USB: the reader thread drains the host-paced audio-IN endpoint into
     * audioRing_. Ethernet free-running has no such endpoint — EthTransport owns
     * its own RTP rx thread feeding its FIFO (pullAudio pulls from it), so we
     * skip reader() entirely. The eventPump runs for both (events over TCP). */
    if (!freeRunning_) readerThread_ = std::thread([this] { reader(); });
    eventPumpThread_ = std::thread([this] { eventPump(); });
    return true;
}

/* Tear a session down: reap the reader, orderly audio.stop if the device
 * is still talking to us, release the claim. Safe on a dead transport. */
void HarpRuntime::sessionDown() {
    bool wasConnected = connected_.exchange(false, std::memory_order_acq_rel);
    if (readerThread_.joinable()) readerThread_.join();
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
    if (running_.load(std::memory_order_acquire)) return connected();
    harp_plat_init(); /* hi-res timers for the sub-ms pacing/idle waits (Windows) */
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
    if (freeRunning_) {
        /* §8.7 bit-exact: pull the device's RTP samples 1:1 from EthTransport's
         * FIFO — NO resampling. pullFree fills the whole stereo block (silence-
         * padding a transient underrun) and returns the real frames delivered. */
        unsigned real = transport_->pullFree(dst, (unsigned)nFrames);
        ssiRead_.fetch_add(nFrames, std::memory_order_relaxed);
        if (real < (unsigned)nFrames && connected_.load(std::memory_order_acquire)) {
            unsigned pad = (unsigned)nFrames - real;
            underruns_.fetch_add(1, std::memory_order_relaxed);
            padSamples_.fetch_add(pad, std::memory_order_relaxed);
            return pad;
        }
        return 0;
    }
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

/* Offline per-part pull: block until the sink's demuxed range has arrived. Like
 * the owner blocking pull but on the sink's ring/padDebt and WITHOUT advancing
 * ssiRead_ (the owner's clock). A null sink yields silence immediately. */
size_t HarpRuntime::pullAudioBlocking(AudioSink *sink, float *dst, size_t nFrames,
                                      unsigned timeoutMs) {
    if (!sink) {
        memset(dst, 0, nFrames * 2 * sizeof(float));
        return nFrames;
    }
    syncSinkEpoch(*sink); /* B3: drop pre-(re)negotiation pad debt + stale ring */
    settleSinkPadDebt(*sink);
    size_t want = nFrames * 2;
    size_t got = 0;
    unsigned waited = 0;
    while (got < want) {
        got += sink->ring.read(dst + got, want - got);
        if (got >= want) break;
        if (!connected_.load(std::memory_order_acquire) || waited >= timeoutMs) {
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
    const unsigned kEthTargetFrames = 2048; /* FIFO setpoint (~43 ms; tunable) */
    while (running_.load(std::memory_order_relaxed) &&
           connected_.load(std::memory_order_relaxed)) {
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
        if (freeRunning_) {
            uint64_t nowNs = harp_now_ns();
            if (nowNs - ethLastTrimNs >= 50000000ull) {
                ethLastTrimNs = nowNs;
                unsigned fill = transport_->fillFrames();
                if (!ethPrimed) { ethSmFill = fill; ethPrimed = true; }
                else ethSmFill += 0.03 * ((double)fill - ethSmFill);
                double trim = -2000.0 * (ethSmFill - (double)kEthTargetFrames);
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

void HarpRuntime::reader() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
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
            /* OWNER main mix: the owner subscribes to the contiguous union
             * PREFIX (computeUnionSlotsLocked puts outSlots_ first), so its
             * two columns are 0 and 1. When the frame carries EXACTLY 2 slots
             * (the default {0,1}-only union — a single instance, or any group
             * where no sibling requested a per-part slot), the prefix IS the
             * whole frame and this write is the SAME contiguous nsamples*2 copy
             * the pre-P5b reader did — BYTE-IDENTICAL (the golden gate). With a
             * wider union (per-part sinks present) we still write the owner's
             * columns 0,1, demuxed below; the {0,1} sub-case stays the fast
             * contiguous path. */
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
            /* P5b DEMUX: split this frame's slot columns into every per-part
             * sink's ring. reader() is the SOLE writer of every sink (SPSC
             * producer side); the lock is the safe-free invariant against
             * unregisterAudioSink (it removes a sink under the same lock before
             * freeing, so we never write a freed ring). Pure memory work — no
             * I/O under the lock. The relaxed haveSinks_ gate keeps the single-
             * instance / default-main-mix reader on its pre-P5b LOCK-FREE path:
             * with no sink registered the demux lock is never taken at all. */
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
        if (harp_client_refset(&client_, archive, nullptr, &deviceHead, true, false, nullptr) !=
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
                                 &target, live.unborn, false, nullptr) == 0;
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
