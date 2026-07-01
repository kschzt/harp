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

#define CREDIT_GRANT (16u << 20)

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

namespace {
/* §8.4 practical-bulk capacity for a usb-speed enum (bytes/sec). Only High-Speed=30 MB/s
 * is spec-anchored (§8.4 topology note); the rest are informative engineering estimates. */
uint64_t usbSpeedBps(int speed) {
    switch (speed) {
    case 5: return 700ull * 1024 * 1024; /* SuperSpeed+ */
    case 4: return 350ull * 1024 * 1024; /* SuperSpeed */
    case 3: return 30ull * 1024 * 1024;  /* High-Speed (the one spec-anchored figure) */
    case 2: return 1ull * 1024 * 1024;   /* Full-Speed */
    default: return 1ull * 1024 * 1024;  /* Low / unknown: conservative floor */
    }
}
/* §8.4 capacity of the transport PATH (bytes/sec). Priority: the HARP_ADMISSION_BUDGET env
 * seam (test/field override) wins; else a real USB controller's practical-bulk speed; else
 * a nominal segment budget for eth/loopback — no real controller is host-observable from a
 * TCP socket, so that figure is a DECLARED policy number, not a measurement. */
uint64_t pathCapacityBps(ShellTransport *t) {
    if (const char *e = getenv("HARP_ADMISSION_BUDGET")) {
        unsigned long long v = strtoull(e, nullptr, 10); /* 64-bit: a >2GB budget overflows long on MSVC */
        if (v > 0) return (uint64_t)v;
    }
    harp_usb_topology topo;
    if (t && t->kind() == ShellTransport::Kind::Usb && t->usbTopology(&topo) && topo.ok)
        return usbSpeedBps(topo.speed);
    return 110ull * 1024 * 1024; /* eth/loopback nominal (~1 Gbit practical): declared policy */
}
} // namespace

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

    /* §8.4 admission control: before audio.start reaches the wire, reserve this session's
     * audio bandwidth in the process-global per-path ledger, and refuse explicitly with the
     * computed budget if the transport path is full — never degrade silently. needBps is
     * exactly what this branch puts on the wire: the OUT union plus the IN columns declared
     * (0 free-running RTP, 1 loopback, else 2 host-paced), float32. */
    {
        uint32_t outCh = (uint32_t)unionSlots_.size();
        /* IN columns declared on the wire: 0 free-running RTP (no H→D audio); else
         * §8.8 FX in-slots when armed, the single loopback column when probing, or the
         * historical 2 host-paced pacing columns (slots=0 frames, but [0,1] declared). */
        uint32_t inCh = freeRunning_.load(std::memory_order_relaxed)
                            ? 0u
                            : (fxArmed() ? (uint32_t)fxInSlots_.size()
                                         : (loopbackArmed() ? 1u : 2u));
        uint64_t needBps = (uint64_t)(outCh + inCh) * 4ull * rate;
        std::string pathKey;
        if (transport_ && transport_->kind() == ShellTransport::Kind::Usb) {
            harp_usb_topology topo;
            pathKey = std::string("usb:") +
                      ((transport_->usbTopology(&topo) && topo.ok) ? topo.controller : "unknown");
        } else {
            pathKey = "eth:global"; /* loopback/eth: one shared local segment (honest for the refdev) */
        }
        /* resKey identifies THIS runtime's one session uniquely + stably. The serial is a
         * grouping HINT, not a uniqueness guarantee (two serial-less USB gadgets both report
         * "?"), and shared instances ride one runtime — so the runtime pointer is the right
         * key: unique per session, identical for every instance attached to it. */
        std::string resKey = "rt:" + std::to_string((uintptr_t)this);
        uint64_t cap = pathCapacityBps(transport_);
        if (admittedBps_ && admittedPath_ != pathKey) {
            /* rebound to a DIFFERENT path (controller/segment): free the old path's row. A
             * SAME-path re-negotiation keeps our row — ledger_reserve overwrites it on admit
             * (excludes our own key when summing) or leaves it intact on refusal, so a refused
             * widen never throws away the still-valid prior reservation. */
            ledger_release(admittedPath_, admittedKey_);
            admittedBps_ = 0;
        }
        uint64_t reserved = 0, capOut = 0, avail = 0;
        if (!ledger_reserve(pathKey, resKey, needBps, cap, &reserved, &capOut, &avail)) {
            log_msg("§8.4 admission: REFUSED audio.start on %s — requested %llu B/s (%u out + %u in "
                    "ch x 4 B x %u Hz float32), available %llu B/s, capacity %llu B/s",
                    pathKey.c_str(), (unsigned long long)needBps, outCh, inCh, rate,
                    (unsigned long long)avail, (unsigned long long)capOut);
            recordLog(HARP_LOG_ERROR, "admission", "audio.start refused: path bandwidth budget exceeded");
            recordTransition(HARP_ST_NEGOTIATED, HARP_ST_DETACHED, HARP_TR_AUDIO_START,
                             "§8.4 admission refused: requested bandwidth exceeds path budget");
            return false; /* audio.start NEVER reaches the wire (state matches the wire-failure edge) */
        }
        admittedPath_ = pathKey;
        admittedKey_ = resKey;
        admittedBps_ = needBps;
    }

    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    harp_client_req_head(&client_, &req, "audio.start", true);
    if (freeRunning_) {
        log_msg("eth audio.start: packet=%u prefill/target=%u frames (deviceEthFloor=%u)",
                ethNsamples(), ethTargetFrames(), deviceEthFloor_);
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
        if (fxArmed()) {
            /* §8.8 audio.fx: declare the device's input columns; the feeder fills
             * them with the track audio process() pushes (writeFxInput). The device
             * (engine_is_fx()) demuxes these columns into a->fx_in and returns WET. */
            harp_cbor_array(&req, fxInSlots_.size());
            for (uint32_t s : fxInSlots_) harp_cbor_uint(&req, s);
        } else if (loopbackArmed()) {
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
    if (!ok && admittedBps_) { /* §8.4: audio.start failed ON THE WIRE — release here, because the
                                * sessionUp false-branch deletes transport_ WITHOUT a sessionDown */
        ledger_release(admittedPath_, admittedKey_);
        admittedBps_ = 0;
    }
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
        /* §8.4: a re-neg audio.start that FAILS (incl. an admission refusal — the device is
         * already stopped, so the host-paced reader only times out r==0, never r<0) would
         * leave connected_=true and the supervisor parked in feeder() forever: a silent wedge.
         * Drop connected_ so the supervisor reaps + reconnects, re-running audio.start (and
         * admission) from a clean sessionUp — it self-heals once the path frees up. */
        connected_.store(false, std::memory_order_release);
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

/* Each queue* pushes to the owner source ring (its SPSC producer side) and bumps
 * the per-session fence (evtQueuedSeq_): the device must consume that many events
 * before rendering a fenced range. A null source (unacquired instance) is a
 * defensive no-op — the shells only queue once they hold the owner source. */
void HarpRuntime::queueParamSet(EventSource *src, uint32_t id, float v, uint64_t ts,
                                uint8_t channel) {
    if (!src) return;
    if (readOnlyDefault_.load(std::memory_order_relaxed) || roExplicit_.load(std::memory_order_relaxed)) {
        /* §12.2/§11.4: read-only hold — a live param edit MUST NOT reach a mismatched-engine
         * device; drop + count (the user exits read-only to make changes). The fresh-open default
         * held the STORED state read-only; this closes the live-edit leak (re-audit HIGH #1). */
        roWrDrops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    /* M2 per-event part: an explicit channel (a satellite's MIDI channel N) targets part N;
     * the default resolves to the source's own channel (byte-identical for every prior caller). */
    uint8_t ch = (channel == kChanFromSource)
                     ? (uint8_t)(src->chan.load(std::memory_order_relaxed) & 0xf)
                     : (uint8_t)(channel & 0xf);
    if (src->ring.push({0, id, v, ts, 0, ch}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueRamp(EventSource *src, uint32_t id, float target, uint64_t start,
                            uint64_t end, uint8_t channel) {
    if (!src) return;
    if (readOnlyDefault_.load(std::memory_order_relaxed) || roExplicit_.load(std::memory_order_relaxed)) {
        /* §12.2/§11.4: read-only hold — drop the automation write so it can't reach the
         * mismatched engine (re-audit HIGH #1). */
        roWrDrops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint8_t ch = (channel == kChanFromSource)
                     ? (uint8_t)(src->chan.load(std::memory_order_relaxed) & 0xf)
                     : (uint8_t)(channel & 0xf);
    if (src->ring.push({1, id, target, start, end, ch}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueNote(EventSource *src, uint32_t word, uint64_t ts) {
    if (!src) return;
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
    if (!src) return;
    /* kind 4 = mod; the §9.5 voice key rides in `end` (it is a packed uint, not
     * a timestamp). A dropped mod is benign — it leaves the base value as-is, no
     * stuck state — so unlike a note we do not escalate to panic on overflow.
     * M2: carry the source's channel as the part FALLBACK — a per-voice mod still
     * derives its part from the voice key (encodeModEvent), a part-wide mod (voice 0)
     * uses this channel, byte-identical to the prior src.chan path. */
    uint8_t ch = (uint8_t)(src->chan.load(std::memory_order_relaxed) & 0xf);
    if (src->ring.push({4, id, offset, ts, voice, ch}))
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

/* (The attached-source registry — registerSource/unregisterSource — is retired:
 * one private runtime per instance, so the only event source is ownerSource_.) */

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
        memset(dst + got, 0, (want - got) * sizeof(float));
        /* §8.8: only the 1:1 synth path owes droppable pad debt. For an armed FX
         * (fxArmed) this short read is PRIMING silence while the fixed-latency wet
         * pipeline fills — the wet is PDC-late, not spent — so DON'T schedule a drop
         * (settlePadDebt early-returns for the FX too). We still COUNT the underrun:
         * for the FX these are the expected handful of priming blocks, after which the
         * ring stays full and there are none. fxInSlots_ is fixed before start(). */
        if (!fxArmed()) padDebtFloats_ += want - got;
        if (connected_.load(std::memory_order_acquire)) {
            underruns_.fetch_add(1, std::memory_order_relaxed);
            padSamples_.fetch_add((want - got) / 2, std::memory_order_relaxed);
        }
        shortBy = (want - got) / 2;
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
            if (fxArmed()) observeFxWet(dst, nFrames); /* §8.8 never-silent guard (offline) */
            return (want - got) / 2;
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
        /* MULTI-OUT: pace to keep the SLOWEST consumer fed — the main-mix ring OR any
         * per-part sink. A wide-union multi-out main demuxes every paced frame into both
         * audioRing_ and the 16 sinks; gating on audioRing_ ALONE let the feeder stop once
         * the main mix hit target while a per-part sink was still below it, so that bus's
         * blocking pull stalled (offline/USB multi-out hung). minRingFillFrames() is the min
         * across main + all sinks — exactly what the free-running reader's drain gate uses.
         * With no sinks it == audioRing_ fill, so the single-out path is byte-identical. */
        size_t ringFrames = minRingFillFrames();
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
        /* §8.8 audio.fx: when the shell armed an effect input, each pacing frame
         * CARRIES the track audio (process() pushed it via writeFxInput) on
         * `fxCols` interleaved columns; the device demuxes them into a->fx_in and
         * returns WET. Not armed (the instrument) => fxCols==0 => the byte-identical
         * slots=0 pacing frame, no payload, no input gate. */
        const size_t fxCols = fxArmed() ? fxInSlots_.size() : 0;
        while (ringFrames < (size_t)targetFrames_ && inFlight < ahead_ &&
               ssi_ + kBlock <= frontierCap) {
            /* §8.8: only pace once the track input for this range is in the SPSC
             * ring — this couples the H→D input 1:1 to the D→H wet the reader fills,
             * so dry and wet stay sample-aligned (the lockstep host-paced effect). */
            if (fxCols && fxInRing_.readAvailable() < kBlock * fxCols) break;
            /* every pacing frame carries the event fence (§8.3.1): the
             * count of events queued so far this session. Any event queued
             * before this instant is guaranteed consumed device-side
             * before this range renders — events and pacing travel on
             * different pipes, and without the fence they race (measured:
             * decoupling the event writes from this loop tripled evt_late
             * until the fence closed the order by construction). */
            harp_audio_hdr pace = {HARP_AUDIO_FVER,
                                   HARP_AUDIO_DIR_H2D | HARP_AUDIO_FENCE,
                                   (uint16_t)fxCols, /* slots: 0 instrument, N §8.8 FX in-cols */
                                   0,
                                   ssi_,
                                   (uint16_t)kBlock,
                                   HARP_AUDIO_FMT_F32};
            /* header + 4 fence bytes (+ §8.8 FX input payload: kBlock × fxCols f32) */
            uint8_t ph[HARP_AUDIO_HDR_LEN + HARP_AUDIO_FENCE_LEN + kBlock * kMaxFxInCols * 4];
            harp_audio_hdr_encode(&pace, ph);
            /* §8.3.1 fence = events queued SINCE this stream's audio.start =
             * monotonic high-water MINUS the current epoch baseline. At the initial
             * start base == 0, so this is byte-identical to the pre-P5b fence; after
             * a re-neg the device reset g_evt_consumed to 0 and base caught up to the
             * counter, so the count restarts at 0 here too — no over-count, no wedge.
             * SATURATE at 0 (now purely defensive): the counter is monotonic — the
             * only decrementer, a released source's leftover-event fetch_sub, is
             * retired with the source registry, so hw >= base always holds. The
             * clamp stays as a cheap guard; an under-count is always safe anyway
             * (the fence is a minimum -> at worst evt_late). */
            uint32_t hw = evtQueuedSeq_.load(std::memory_order_acquire);
            uint32_t base = evtEpochBase_.load(std::memory_order_acquire);
            uint32_t seq = hw > base ? hw - base : 0;
            ph[HARP_AUDIO_HDR_LEN + 0] = (uint8_t)seq;
            ph[HARP_AUDIO_HDR_LEN + 1] = (uint8_t)(seq >> 8);
            ph[HARP_AUDIO_HDR_LEN + 2] = (uint8_t)(seq >> 16);
            ph[HARP_AUDIO_HDR_LEN + 3] = (uint8_t)(seq >> 24);
            size_t frameLen = HARP_AUDIO_HDR_LEN + HARP_AUDIO_FENCE_LEN;
            if (fxCols) {
                /* the device demuxes payload column c (in_slots order) into
                 * a->fx_in[c]; writeFxInput already interleaved process()'s input
                 * by fxCols, so a straight ring read fills the payload in column
                 * order. The availability gate above guarantees a full block. */
                fxInRing_.read((float *)(ph + frameLen), (size_t)kBlock * fxCols);
                frameLen += (size_t)kBlock * fxCols * 4;
            }
            if (!transport_->audioWrite(ph, (int)frameLen, 8)) break;
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
/* Drain up to `budget` events from the owner source's ring, appending each as a
 * framed EVT message to `batch`. Returns the count drained. The eventPump is the
 * SOLE consumer of the source ring (SPSC) — there is one fixed session-long
 * source, so no lock and no safe-free dance.
 *
 * Param sets and ramps carry the SOURCE's channel (key 5) so a multi-out main's
 * per-channel knob edits land on the right part (channel N -> part N).
 * Notes already carry their channel in the UMP word (the shell baked it in).
 * Transport (kind 3) is global and only ever lives on the owner source. */
int HarpRuntime::drainSource(EventSource &src, harp_cbuf &batch, harp_cbuf &msgbuf,
                             int budget) {
    /* M2: the target part (§9.4 key 5) is PER EVENT — te.channel, resolved at queue time
     * from the caller's explicit channel (a satellite's MIDI channel N) or this source's own
     * channel. So one main instance drives every part; an attached single-channel source is
     * byte-identical (its events all carry src.chan). Notes carry their channel in the UMP word. */
    (void)src; /* channel now travels in each TimedEv, not read from the source here */
    TimedEv te;
    int sent = 0;
    for (; sent < budget && src.ring.pop(te); sent++) {
        harp_cbuf_reset(&msgbuf);        if (te.kind == 0)
            encodeParamEvent(&msgbuf, te.a, te.v, te.ts, te.channel);
        else if (te.kind == 1)
            encodeRampEvent(&msgbuf, te.a, te.v, te.ts, te.end, te.channel);
        else if (te.kind == 3) {
            double ppq;
            memcpy(&ppq, &te.end, sizeof ppq);
            encodeTransportEvent(&msgbuf, te.a, te.v, ppq, te.ts);
        } else if (te.kind == 4)
            /* mod (§9.4): the voice key rides in `end`; a per-voice mod takes its
             * part from the voice key, a part-wide mod (voice 0) from te.channel. */
            encodeModEvent(&msgbuf, te.a, te.v, te.ts, (uint32_t)te.end, te.channel);
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
    /* + hard real-time: events ride the link endpoint while pacing rides the
     * audio endpoint; if this thread stalls under load the §8.3.1 fence the
     * feeder stamps outraces the event stream and the device counts
     * fence_timeouts (the late-apply that shows as the dropout's edge). Keep it
     * on the same RT class as the feeder so the two pipes stay in lockstep. */
    harp_thread_set_realtime(0);
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
         * Drain the owner source (the one and only source — the per-instance
         * attached-source merge is retired). The audio thread (queue*) is its sole
         * SPSC producer; the pump is the sole consumer, so no lock is needed. */
        harp_cbuf_reset(&batch);
        int sent = drainSource(ownerSource_, batch, msgbuf, 64);
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
            /* the loop above fills tmp[0 .. 2*chunk), exactly the range written here; cppcheck
             * can't prove the variable-length loop covers it (false positive). */
            /* cppcheck-suppress uninitvar */
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
    /* + hard real-time: this thread drains the D->H render frames (USB audio_fifo
     * or RTP) into audioRing_ that the DAW audio thread pulls. If it is preempted
     * past the small (low-latency) ring depth, the DAW sees a gap even when the
     * bytes already landed in the FIFO — so it must be RT too. See host/usb_io.c. */
    harp_thread_set_realtime(0);
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
                rtpLostSnap_.store(transport_->rtpPacketsLost(), std::memory_order_relaxed); /* §8.7 clock-stats key 8 (rtp_loss) */
                unsigned width = unionWidth_.load(std::memory_order_relaxed);
                if (!width) width = 2;
                if (floats >= width)
                    demuxUnionFrame(buf, floats / width, (uint16_t)width);
                else if (rtpStallTrip(floats, transport_->silentMs())) {
                    /* §8.7 never-silent guard tripped (loud log + x.harp.rtp_silent counter
                     * + sticky flag, inside rtpStallTrip). §12.1: implicit STREAMING ->
                     * DETACHED (transport-error). On the reader thread; the rings are
                     * reader-safe (history mutex, lock-free log) and off the render path. */
                    recordTransition(HARP_ST_STREAMING, HARP_ST_DETACHED,
                                     HARP_TR_TRANSPORT_ERROR, "RTP stream silent >1s");
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
            cfg.quality = HARP_ASRC_QUALITY; /* §8.3: >=120 dB stopband + <=0.01 dB ripple
                                              * (freerun.h; == SRC_SINC_BEST_QUALITY). MEDIUM (~96 dB
                                              * @23 kHz) and FASTEST (~97 dB) are BELOW the floor. */
            harp_freerun *fr = harp_freerun_new(&cfg);
            if (!fr) {
                log_msg("ASRC: resampler init failed (libsamplerate?) — no audio");
                connected_.store(false, std::memory_order_release);
                return;
            }
            uint64_t prevTs = 0;
            bool tsPrimed = false; /* gate the clock observe on the FIRST forward packet */
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
                    /* §8.7 reorder-residual (re-audit HIGH #10): a REORDERED or DUPLICATE
                     * packet (the same non-advancing seq harp_rtp_loss_gap flags advance=false
                     * for) carries a dev timestamp that does NOT advance past the high-water
                     * (dev <= prevTs). Feeding its (dev_ts, host_ns) into the regression
                     * perturbs the drift fit — a backward dev with a forward host_ns drags the
                     * slope. The loss-COUNT fix (round 5, eth_transport recvAudio) already keeps
                     * lastSeq_ from rewinding; this is the CLOCK-fit analogue: only an ADVANCING
                     * packet (dev > prevTs, the in-order/forward case) updates the fit AND the
                     * high-water. A reordered/dup packet is still PUSHED below (its audio is real
                     * data, late but valid for the elastic buffer) — only its timestamp is
                     * withheld from the clock recovery. The first packet always primes. */
                    bool advance = (!tsPrimed) || (dev > prevTs);
                    if (advance) {
                        prevTs = dev;
                        tsPrimed = true;
                        harp_freerun_observe(fr, dev, harp_now_ns());
                    }
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
                    asrcReanchors_.store(st.reanchors, std::memory_order_relaxed);
                    rtpLostSnap_.store(transport_->rtpPacketsLost(), std::memory_order_relaxed); /* §8.7 clock-stats key 8 (rtp_loss) */
                } else if (rtpStallTrip(floats, transport_->silentMs())) {
                    /* §8.7 never-silent guard tripped (loud log + x.harp.rtp_silent counter
                     * + sticky flag, inside rtpStallTrip), ASRC path: the resampler is
                     * starved of input. §12.1: implicit STREAMING -> DETACHED. */
                    recordTransition(HARP_ST_STREAMING, HARP_ST_DETACHED,
                                     HARP_TR_TRANSPORT_ERROR, "RTP stream silent >1s (ASRC)");
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
