#include "runtime.h"
#include "runtime_registry.h" /* §8.4 admission ledger (ledger_reserve/release/reserved) */
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

#define LIVE_REF "live/project"
#define CREDIT_GRANT (16u << 20)
#define BUNDLE_MAGIC "harpb"

/* §8.8 never-silent guard: |sample| at or below this counts as silence. Above the
 * float denormal floor but far below any real audio, so the all-zeros wet of a broken
 * input path reads silent while a working reverb's wet reads live. */
static constexpr float kFxSilenceEps = 1e-6f;

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "harp-shell: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* §9.3/§13.4 param-map drift warning — one message, emitted whether the project
 * state arrived while connected or a bundle staged offline applied on connect. */
static void log_param_map_drift() {
    log_msg("recall: project's param map differs from the device's (engine update?) "
            "— applying matching ids only");
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

/* §12.2/§13.4: (re)compute the read-only holds against the LIVE identity vs the staged project's
 * expectations — engine-major mismatch, a different bound unit (serial), or a device refusal, minus a
 * user consent to the engine difference. Called on connect (helloAndIdentity) AND when a project is
 * staged while connected (setStateBundle), so the staged-while-connected path can't silently auto-push
 * onto a different/incompatible unit. Logs the hold once (on the clean->read-only transition). */
void HarpRuntime::recomputeReadOnlyHolds() {
    int curMajor = atoi(engineVer_.c_str());
    const char *force = getenv("HARP_FORCE_ENGINE_MAJOR");
    int expectMajor = (force && *force) ? atoi(force)
                      : wantEngineMajor_.load(std::memory_order_relaxed);
    bool mismatch = (expectMajor > 0 && curMajor != expectMajor);
    bool wasRO = readOnlyDefault_.load(std::memory_order_relaxed);
    if (mismatch && !wasRO) {
        char d[96];
        snprintf(d, sizeof d, "engine major %d (project) != %d (device): project state held read-only",
                 expectMajor, curMajor);
        recordTransition(HARP_ST_ATTACHED, HARP_ST_ATTACHED, HARP_TR_ENGINE_MAJOR_MISMATCH, d);
        log_msg("%s", d);
    }
    /* §12.2 (re-audit HIGH #4): hold read-only when a DIFFERENT physical unit was bound than the
     * project's — selectDevice's same-model fallback can bind another unit; the project must NOT
     * silently auto-push onto it. Compares the bound serial to the bundle's (§15.3 key 2). */
    std::string wantSer;
    { std::lock_guard<std::mutex> blk(bundleMutex_); wantSer = wantSerial_; }
    bool serialDiffers = !wantSer.empty() && !serial_.empty() && serial_ != wantSer;
    if (serialDiffers && !wasRO) {
        char d[160];
        snprintf(d, sizeof d, "serial %s (project) != %s (device): bound a different unit — project state held read-only",
                 wantSer.c_str(), serial_.c_str());
        recordTransition(HARP_ST_ATTACHED, HARP_ST_ATTACHED, HARP_TR_SERIAL_MISMATCH, d);
        log_msg("%s", d);
    }
    /* §13.4: HARP_CONSENT_ENGINE_MAJOR conformance seam — user pre-consents to an engine difference. */
    const char *cenv = getenv("HARP_CONSENT_ENGINE_MAJOR");
    if (cenv && *cenv && atoi(cenv)) consentEngineMajor_.store(true, std::memory_order_relaxed);
    bool consented = consentEngineMajor_.load(std::memory_order_relaxed);
    /* §12.2/§13.4: hold on a different unit (serial), OR an engine mismatch / device refusal — unless
     * consented to the engine difference. Consent does NOT lift the serial-differs hold. */
    readOnlyDefault_.store(serialDiffers || ((mismatch ||
                               engineRefused_.load(std::memory_order_relaxed)) && !consented),
                           std::memory_order_relaxed);
    engineMajorSeen_ = expectMajor;
}

bool HarpRuntime::helloAndIdentity() {
    harp_client_identity id;
    int rc = harp_client_hello(&client_, "harp-shell 0.1 (VST3)", &id);
    if (rc != 0) {
        if (rc == HARP_CLIENT_EINCOMPAT) {
            /* §5.4: surface a firmware/host-update prompt with specifics — never fail silently. */
            needsFirmwareUpdate_ = true;
            log_msg("device protocol INCOMPATIBLE: device supports major %u..%u — a firmware or "
                    "host update is required",
                    client_.incompat_major_min, client_.incompat_major_max);
        } else if (rc == HARP_CLIENT_EDEV)
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
    deviceEthFloor_ = id.eth_target_floor; /* §6.4 rt-profile (key 14): device-declared RTP
                                              jitter-buffer floor (frames); 0 = undeclared, so
                                              ethTargetFrames() keeps the conservative default. */
    deviceEthNsamples_ = id.eth_nsamples;  /* §6.4 rt-profile (key 14 sub-key 1): device-declared RTP
                                              packet size; 0 = undeclared -> ethNsamples() keeps 256. */
    /* §12.2: if the device's engine MAJOR changed across this (re)connect, the staged
     * project state may not fit the new engine — record it and hold the state read-only
     * (sessionUp then skips the auto-push). engineVer_ is "MAJOR.MINOR.PATCH"; atoi reads
     * the leading major. HARP_FORCE_ENGINE_MAJOR seeds the baseline so the conformance
     * test can force a single-connect mismatch. A matching reconnect self-clears the flag. */
    /* §12.2/§13.4: recompute the read-only holds (engine-major / serial-differs / device-refusal)
     * against the live identity vs the staged project. Shared with setStateBundle so a project staged
     * WHILE connected gets the same protection (else it auto-pushes onto a different/incompatible unit). */
    recomputeReadOnlyHolds();
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

/* ---------------- lifecycle ---------------- */

/* The device-selection policy (see header). The mutual exclusion is the
 * USB claim inside harp_usb_open_match: a device owned by another plugin
 * instance fails the claim and the scan advances, so two fresh instances
 * land on different units without any coordination here. */
/* Wrap a freshly claimed USB transport (or a failed claim) as a ShellTransport.
 * Step 1 only ever builds UsbTransport here; the Ethernet binding is selected
 * elsewhere (next step). nullptr in => nullptr out (no device was claimed). */
static ShellTransport *wrapUsb(harp_io *io) { return io ? new UsbTransport(io) : nullptr; }

/* §6.1/§4.4.3 shell auto-discovery: browse `_harp._tcp` and return the first resolved
 * "host:port" (empty if none, or where dns_sd is unavailable — then the shell just keeps
 * supervising for a USB device). A short, bounded browse; the supervisor retries ~1 s, so it
 * re-browses each cycle (this is how a network synth hot-plugs in). Opt out with HARP_NO_MDNS=1. */
#ifdef HARP_SHELL_ENGINE_FILTER
/* A product built with HARP_SHELL_ENGINE_FILTER binds ONLY a network device whose
 * §12 engine id matches — so it skips the other HARP devices on the bus without the
 * user picking. mDNS resolves host:port but not the engine, so briefly hello each
 * candidate and read its engine id: cheap (a few LAN devices), stateless (store=NULL). */
static bool ethEngineIs(const char *hostport, const char *want) {
    harp_sockhandle s = harp_sock_dial(hostport);
    if (s == HARP_SOCK_INVALID) return false;
    harp_sock_io t;
    harp_sock_io_init(&t, s);
    harp_link link;
    harp_link_init(&link);
    harp_client c;
    harp_client_init(&c, &t.io, &link, nullptr, nullptr, nullptr);
    harp_client_identity id;
    bool ok = harp_client_hello(&c, "harp-shell (engine probe)", &id) == 0 &&
              strcmp(id.engine_id, want) == 0;
    harp_client_free(&c);
    harp_link_free(&link);
    harp_sock_close(s);
    return ok;
}
#endif

static std::string discoverEthDevice() {
    if (const char *no = getenv("HARP_NO_MDNS"))
        if (no[0] && no[0] != '0') return std::string();
#ifdef HARP_SHELL_ENGINE_FILTER
    /* browse ALL `_harp._tcp`, keep the first that reports the wanted engine */
    harp_mdns_instance inst[16];
    int n = harp_mdns_discover(1200, inst, sizeof inst / sizeof inst[0]);
    for (int i = 0; i < n; i++) {
        char hp[300];
        snprintf(hp, sizeof hp, "%s:%u", inst[i].host, (unsigned)inst[i].port);
        if (ethEngineIs(hp, HARP_SHELL_ENGINE_FILTER)) return std::string(hp);
    }
    return std::string();
#else
    harp_mdns_instance inst;
    if (harp_mdns_discover(1200, &inst, 1) >= 1) {
        char hp[300];
        snprintf(hp, sizeof hp, "%s:%u", inst.host, (unsigned)inst.port);
        return std::string(hp);
    }
    return std::string();
#endif
}

/* §8.3-over-§8.7 mid-stream live<->offline toggle. The shell calls this from its
 * offline-render hook (a host/main thread — never the audio thread). On a genuine mode
 * change on a LIVE Ethernet session, arm a re-dial and BLOCK (bounded) until the new-mode
 * session is up, so the host's next process()->pull is deterministic host-paced (not the
 * stale free-running ring). Pre-start / no-session / USB are early no-ops. */
void HarpRuntime::setOffline(bool o) {
    bool prev = wantHostPaced_.exchange(o, std::memory_order_release);
    /* §8.8: an effect device is INHERENTLY host-paced (the host drives audio
     * THROUGH it; free-running RTP has no H→D input path). So an armed FX session
     * is ALWAYS host-paced and the DAW's live<->offline toggle must NEVER re-dial
     * it to free-running — wantHostPacedMode() stays true either way, so there is no
     * mode to flip. Gate the whole toggle on fxArmed(); the instrument (never armed)
     * keeps its exact free-running/host-paced live/offline flip behaviour below. */
    if (fxArmed()) return;
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
        if (eth[0]) {
            /* "mdns"/"discover" => browse `_harp._tcp` and dial the first synth found — the
             * EXPLICIT form of the no-USB auto-discovery below; anything else is a literal
             * host:port. host-paced (deterministic) when the DAW renders offline, else free-run RTP. */
            std::string target = eth;
            if (target == "mdns" || target == "discover") {
                /* skip the browse on the synchronous load-thread attempt; the supervisor browses async */
                target = allowDiscovery_.load(std::memory_order_relaxed) ? discoverEthDevice() : std::string();
                if (target.empty()) return nullptr; /* none resolved this cycle — supervisor retries */
                log_msg("mDNS: discovered network device %s — dialing", target.c_str());
            }
            /* §8.8: an armed effect (fxArmed) always dials host-paced — see wantHostPacedMode(). */
            return EthTransport::dial(target.c_str(), wantHostPacedMode());
        }

    /* reconnect: pinned to the exact unit this instance already owns — the
     * same-model fallback must NOT fire here, or a replug could let this
     * instance steal a sibling track's device. */
    if (!boundSerial_.empty()) {
        if (!boundEthHostport_.empty()) {
            /* §4.4.3/§12.3: this instance is pinned to a NETWORK synth — re-dial the same
             * address (a transient drop keeps it), and if the synth renumbered, re-browse for
             * one. Never fall into the USB-only lookup below, which could never resume it. */
            bool hp = wantHostPacedMode(); /* §8.8: an armed FX always re-dials host-paced */
            if (ShellTransport *t = EthTransport::dial(boundEthHostport_.c_str(), hp)) return t;
            std::string disc = allowDiscovery_.load(std::memory_order_relaxed) ? discoverEthDevice() : std::string();
            if (!disc.empty()) {
                log_msg("mDNS: re-discovered network device %s — dialing", disc.c_str());
                return EthTransport::dial(disc.c_str(), hp);
            }
            return nullptr;
        }
        return wrapUsb(harp_usb_open_match_ctx(usbCtx_, boundSerial_.c_str(), false, 0, 0));
    }

#ifndef HARP_SHELL_ETHERNET_ONLY /* a network-only product never claims a USB unit */
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
    if (harp_io *io = harp_usb_open_match_ctx(usbCtx_, nullptr, false, 0, 0))
        return wrapUsb(io);
#endif /* !HARP_SHELL_ETHERNET_ONLY */
    /* §6.1/§4.4.3: nothing on USB and no explicit HARP_ETH_DEVICE — browse the segment for a
     * network synth advertising `_harp._tcp` and dial the first one found. Keeps the shell's
     * device list "USB + network" without the DAW having to know an address. The synchronous
     * load-thread attempt skips it (allowDiscovery_=false); the supervisor browses async. */
    std::string disc = allowDiscovery_.load(std::memory_order_relaxed) ? discoverEthDevice() : std::string();
    if (!disc.empty()) {
        log_msg("mDNS: discovered network device %s — dialing", disc.c_str());
        /* §8.8: an armed effect (fxArmed) always dials host-paced — see wantHostPacedMode(). */
        return EthTransport::dial(disc.c_str(), wantHostPacedMode());
    }
    return nullptr;
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
    /* §12.1: the transport is up — DETACHED -> ATTACHED, before hello/identity. */
    recordTransition(HARP_ST_DETACHED, HARP_ST_ATTACHED, HARP_TR_ATTACH,
                     freeRunning_ ? "transport up (ethernet)" : "transport up (usb)");
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
            if (boundSerial_.empty()) {
                boundSerial_ = di.serial; /* pin for reconnect */
                /* §6.1: remember an Ethernet binding's dial target so reconnect re-dials the
                 * network (not the USB-only lookup); netEndpoint() is "" on USB -> USB path. */
                boundEthHostport_ = transport_->netEndpoint();
            }
        }
        /* fresh per session: rid space, credit, AND the link reassembly
         * state — a half-assembled frame from a dead session must not
         * poison the next one */
        harp_link_free(&link_);
        harp_link_init(&link_);
        harp_client_free(&client_);
        harp_client_init(&client_, transport_->ctlIo(), &link_, storeOk_ ? &store_ : nullptr,
                         nullptr, nullptr);
        /* Bound the hello/identity round-trip: a device that ACCEPTS the TCP connect but never
         * replies (a listening-but-wedged daemon) must not hang the dial — critical on a PINNED
         * device, whose dial runs on the synchronous load thread (setActive). Per-recv bound;
         * cleared to blocking on success so the live framed link is unaffected. */
        transport_->setCtlTimeout(2000);
        if (!helloAndIdentity()) {
            log_msg("hello failed");
            harp_client_free(&client_);
            delete transport_;
            transport_ = nullptr;
            return false;
        }
        /* hello ok. Bound the live-session ctl request-response recv (NOT infinite blocking):
         * a request whose response is LOST or delayed — the device parking/closing mid-teardown,
         * a dropped frame — must not wedge the recv forever. On Windows a blocking Winsock recv
         * cannot be interrupted, so an unbounded request recv hangs the supervisor thread and
         * stop()'s join never returns -> the host process hangs until a watchdog kills it (the
         * intermittent staged-connected "host HUNG"). sock_read_exact returns false on
         * SO_RCVTIMEO (WSAETIMEDOUT != WSAEINTR), so the request fails fast and the host makes
         * progress. 8s is generous (the hello round-trip above is bounded to 2s; healthy
         * responses are <100ms) and well under the 30s host watchdog, with margin for several
         * timed-out requests in one teardown. POLL-GATED reads (linkPoll-then-recv, e.g. the
         * §11.4 reconcile's panel-pick loop and any async core.changed) only recv when data is
         * already readable, so this never fires for them — only an actually-missing response. */
        transport_->setCtlTimeout(8000);
        log_msg("connected: %s %s (serial %s, engine %s %s)", vendorName_.c_str(),
                productName_.c_str(), serial_.c_str(), engineId_.c_str(),
                engineVer_.c_str());
        /* §12.1: hello-ok — the device identified + capabilities known: ATTACHED -> NEGOTIATED. */
        {
            char d[256];
            snprintf(d, sizeof d, "%s %s (serial %s, engine %s %s)", vendorName_.c_str(),
                     productName_.c_str(), serial_.c_str(), engineId_.c_str(),
                     engineVer_.c_str());
            recordTransition(HARP_ST_ATTACHED, HARP_ST_NEGOTIATED, HARP_TR_HELLO_OK, d);
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
        bool bundlePmhSet = false;
        harp_hash bundlePmh{};
        {
            std::lock_guard<std::mutex> blk(bundleMutex_);
            haveBundle = hasBundle_;
            target = bundleTarget_;
            bundlePmhSet = bundleParamMapHashSet_;
            bundlePmh = bundleParamMapHash_;
        }
        if (haveBundle && (readOnlyDefault_.load(std::memory_order_relaxed) ||
                           roExplicit_.load(std::memory_order_relaxed))) {
            /* §12.2/§13.4/§11.4: a read-only hold is in effect — the §12.2 engine/serial auto-hold,
             * a §13.4 device refusal, OR the user's explicit §11.4 Open-read-only pick. Do NOT
             * auto-apply the staged project; the user re-applies explicitly (or exits read-only).
             * Skipping the push here is also WHY roExplicit_ persists across reconnect — a held
             * session never reaches pushStateLocked's choice logic via a headless reconnect. */
            log_msg("project state held read-only — not auto-applied");
            recordLog(HARP_LOG_WARN, "recall",
                      "project state read-only — not auto-applied (§12.2/§13.4 mismatch or §11.4 explicit)");
        } else if (haveBundle) {
            /* §9.3/§13.4: a bundle that staged while offline applies now — warn if the
             * device's automatable param map drifted from what the project expects
             * (paramMapHash_ is valid only now that we're connected). */
            if (bundlePmhSet && memcmp(bundlePmh.b, paramMapHash_.b, HARP_HASH_LEN) != 0)
                log_param_map_drift();
            if (pushStateLocked(target)) {
                if (readOnlyDefault_.load(std::memory_order_relaxed) ||
                    roExplicit_.load(std::memory_order_relaxed)) {
                    /* §11.4/§13.4: the reconcile resolved to a READ-ONLY outcome — the user picked
                     * Open-read-only (choice 2) or the device refused the push (incompatible) — so
                     * pushStateLocked held WITHOUT writing. Don't claim a re-assert (it would mislead
                     * the user + the recall tests into thinking the project was pushed). */
                    log_msg("project state held read-only (reconcile: no write)");
                    recordLog(HARP_LOG_INFO, "recall", "project state held read-only (reconcile)");
                } else {
                    log_msg("project state re-asserted");
                    recordLog(HARP_LOG_INFO, "recall", "project state re-asserted");
                }
            } else {
                log_msg("project state apply failed (will retry on reconnect)");
                recordLog(HARP_LOG_WARN, "recall",
                          "project state apply failed (will retry on reconnect)");
            }
        }
        if (!audioStart(rate_)) {
            log_msg("audio.start failed");
            /* §12.1: NEGOTIATED but the stream never came up -> back to DETACHED. */
            recordTransition(HARP_ST_NEGOTIATED, HARP_ST_DETACHED, HARP_TR_AUDIO_START,
                             "audio.start failed");
            recordLog(HARP_LOG_ERROR, "audio.start", "audio.start failed");
            harp_client_free(&client_);
            delete transport_;
            transport_ = nullptr;
            return false;
        }
        /* §12.1: audio.start accepted -> SYNCED (stream synced, clock locked). */
        recordTransition(HARP_ST_NEGOTIATED, HARP_ST_SYNCED, HARP_TR_AUDIO_START,
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

    /* new session = new stream = new SSI time domain (§7.1). Events still queued
     * from the previous session carry STALE timestamps — drain the owner source
     * (no pump is running yet, so consuming here is safe), and the fence sequence
     * space restarts from zero on both sides. */
    {
        TimedEv stale;
        while (ownerSource_.ring.pop(stale)) {}
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
    /* §12.2/§11.4: report how many live param/automation writes the read-only hold suppressed
     * this session (logged here, on the supervisor thread — NOT from the RT queue writers). */
    if (uint64_t rod = roWrDrops_.exchange(0, std::memory_order_relaxed))
        log_msg("read-only: suppressed %llu live param/automation write(s) "
                "(§12.2 engine/serial mismatch or §11.4 explicit read-only)",
                (unsigned long long)rod);
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
        /* audio.stop's response recv is bounded by the session-wide ctl timeout set after
         * hello (8s, see sessionUp) — a missing ack during teardown can no longer wedge the
         * supervisor thread / stop()'s join (the Windows "host HUNG"). */
        audioStopLocked();
        /* drain the tail of the stream so the device thread can park */
        uint8_t junk[16384];
        int quiet = 0;
        while (quiet < 2) {
            int r = transport_->audioRead(junk, sizeof junk, 80);
            if (r < 0) break;
            quiet = (r == 0) ? quiet + 1 : 0;
        }
        /* §5.5: the shell does NOT auto-send core.bye on sessionDown. It looks clean, but on
         * the USB/FunctionFS path the device's session-close (d->closing) is exactly the
         * "host sees a real disconnect" behavior the daemon's UDC-unbind machinery is built
         * around — so a bye on EVERY teardown slows/disrupts the immediate re-claim the hw
         * suite does per test (connect -> 0.5s settle render -> teardown -> re-claim -> render),
         * producing wrong/slow renders. Over loopback it was instant + clean (eth-suite green),
         * which hid the difference; the USB rig caught it. The host caller exists
         * (harp_client_bye) and is exercised by harp-probe core-test; wiring it into the shell
         * teardown is deferred until it can be made USB-safe (eth-binding-only, or post-settle). */
    }
    /* §8.4: free this session's bandwidth reservation before the transport goes. Idempotent
     * (admittedBps_==0 = nothing held: never streamed, or already released on a wire failure). */
    if (admittedBps_) {
        ledger_release(admittedPath_, admittedKey_);
        admittedBps_ = 0;
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
    /* + hard real-time: this thread runs the host-paced PACING FEEDER (the
     * audioWrite loop). Under host CPU contention a time-share QoS thread is
     * preempted for tens of ms, so the FunctionFS OUT endpoint empties and the
     * device underruns (cliff-onset USB dropout). Measured: producer cadence
     * (usb_io out_gap) tailed to 50-85 ms at USER_INTERACTIVE; RT pins it. The
     * helper degrades gracefully where RT is unavailable. See host/usb_io.c. */
    harp_thread_set_realtime(0);
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
                if (end == p) break;        /* no digits: stop at the garbage */
                if (v > 0xFFFFFFFFUL) break; /* >32-bit would silently truncate to a wrong slot — reject */
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
    /* The synchronous first attempt runs on the DAW's load thread (setActive), so it must
     * NOT browse mDNS — a stale/unreachable advertiser would stall the dial and freeze the
     * DAW. Try only the fast USB / pinned-eth paths here; the supervisor (background) does
     * discovery, so a network synth still hot-plugs in a beat later. */
    allowDiscovery_.store(false, std::memory_order_relaxed);
    bool now = sessionUp(); /* fast path: report a present USB/pinned device immediately */
    allowDiscovery_.store(true, std::memory_order_relaxed);
    if (!now) log_msg("no HARP device on the bus; supervising for hot-plug");
    supervisorThread_ = std::thread([this] { supervisor(); });
    return now;
}

void HarpRuntime::stop() {
    /* Flush in-flight events before teardown: the DAW's final note-offs
     * arrive in the last process() blocks, and killing the feeder with
     * them still queued is how notes get stuck. Bounded wait. */
    if (running_.load(std::memory_order_acquire) && connected()) {
        for (int i = 0; i < 100; i++) {
            if (ownerSource_.ring.empty()) break;
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
    /* test-only fault injection: HARP_TEST_PUSH_FAIL simulates ONE transient ctl push failure
     * (the kind a loaded Windows runner produces when a request/response is delayed past the
     * live-session recv bound). One-shot so a later re-assert proceeds normally. The
     * staged-connected test uses this to PROVE that a failed staged-while-connected auto-push is
     * non-fatal — i.e. that the Windows `component setState failed` -> rc=1 flake is fixed. */
    if (getenv("HARP_TEST_PUSH_FAIL")) {
        static std::atomic<bool> injected{false};
        bool expected = false;
        if (injected.compare_exchange_strong(expected, true)) return false;
    }
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
    if (const char *env = getenv("HARP_RECONCILE_TIMEOUT_MS")) {
        int v = atoi(env);
        /* clamp to [0, 120000]: 0 = headless (skip the offer, push now); the upper bound keeps
         * the signed `waited` poll loop below (waited += POLL_MS) from overflowing to UB on a
         * pathologically large value. A negative/garbage env is treated as 0 (headless), not a hang. */
        timeout_ms = v < 0 ? 0 : (v > 120000 ? 120000 : v);
    }
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

    /* §11.4: set the explicit Open-read-only hold on the Read-only pick (choice 2), clear it on any
     * WRITE pick (Push/Pull/Duplicate/Force-consent). Safe to set unconditionally here: gate 1366
     * skips the auto-push for an roExplicit_ session, so this line is reached only via a real user
     * reconcile pick — a headless reconnect never gets here for such a session. */
    roExplicit_.store(choice == 2, std::memory_order_relaxed);

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
    if (choice == 4) { /* §11.4/§13.4 Force (consent): accept the engine-version difference, then Push */
        log_msg("recall: reconcile -> Force (engine-version consent)");
        consentEngineMajor_.store(true, std::memory_order_relaxed);
        engineRefused_.store(false, std::memory_order_relaxed);
        readOnlyDefault_.store(false, std::memory_order_relaxed);
        /* fall through to the Push path — the CAS below now carries consent (flags bit 2). */
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
    /* §11.4: archive the DISPLACED device head before the CAS swaps the live ref to the
     * target — a named archive/<ts> ref keeps the old state from §10.2 GC (the live CAS is
     * force=false, so a SUCCEEDING push still swaps deviceHead->target and, without the
     * archive, the displaced deviceHead becomes unreferenced and is lost). Gated on an ACTUAL
     * displacement (deviceHead != target), NOT on timeout_ms: a headless Push that displaces
     * MUST still archive (the §11.4 MUST), while the common re-assert / per-part-audio stress
     * re-pushes the SAME state (deviceHead == target) — a no-op here, so no archive churn. */
    if (!live.unborn && memcmp(deviceHead.b, target.b, HARP_HASH_LEN) != 0) {
        char tsname[96];
        time_t now = time(nullptr);
        struct tm tm;
        harp_gmtime(now, &tm);
        snprintf(tsname, sizeof tsname, "%s/%04d-%02d-%02dT%02d:%02d:%02dZ", prefix,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        /* §11.4: two DISTINCT displacing pushes within the same wall-clock second collide on this
         * second-granularity name — the second refset (expect=nullptr, create-if-absent) hits the
         * existing ref, conflicts, and the push aborts (silent loss — the §11.4 MUST). A same-second
         * repeat gets a per-session sequence suffix so each displaced state archives to a unique ref. */
        char archive[112];
        if (strcmp(tsname, lastArchiveName_) == 0) {
            snprintf(archive, sizeof archive, "%s.%u", tsname, ++archiveDupSeq_);
        } else {
            snprintf(archive, sizeof archive, "%s", tsname);
            snprintf(lastArchiveName_, sizeof lastArchiveName_, "%s", tsname);
            archiveDupSeq_ = 0;
        }
        if (harp_client_refset(&client_, archive, nullptr, &deviceHead, true, false, false, nullptr) != 0)
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
    bool ok = harp_client_refset(&client_, LIVE_REF, live.unborn ? nullptr : &deviceHead, &target,
                                 live.unborn, false,
                                 consentEngineMajor_.load(std::memory_order_relaxed), nullptr) == 0;
    if (ok) {
        log_msg("recall: live/project restored");
    } else if (strcmp(client_.err_code, "incompatible") == 0) {
        /* §13.4: the device refused the project (engine version differs) — typically a MINOR diff
         * the host's major-based hold did not catch, so the auto-push above was attempted. Hold
         * read-only STICKILY (engineRefused_ survives reconnect via helloAndIdentity) so we stop
         * retry-pushing; the user grants consent (§11.4 Force) to override. */
        engineRefused_.store(true, std::memory_order_relaxed);
        readOnlyDefault_.store(true, std::memory_order_relaxed);
        log_msg("recall: device refused project (incompatible engine version) — held read-only; grant consent to override");
        recordLog(HARP_LOG_WARN, "recall", "device refused incompatible engine version; held read-only");
    }
    /* The bundle reference is PERSISTENT, not consumed: the DAW project's notion of
     * state re-asserts on every reconnect ("Live wins") — the archive/duplicate step
     * keeps it loss-free. It only moves when a save pulls a new head (getStateBundle)
     * or a new set loads. */
    return ok;
}

void HarpRuntime::consentEngineMajorOverride() {
    /* §13.4: the user accepts the staged project's engine-version difference. Mark consent (so the
     * push carries flags bit 2), lift the engine read-only hold + the sticky refusal, and re-apply
     * the staged project (the held path never pushed). The serial-differs hold, if any, re-asserts
     * on the next hello — consent covers the engine difference only. */
    consentEngineMajor_.store(true, std::memory_order_relaxed);
    engineRefused_.store(false, std::memory_order_relaxed);
    readOnlyDefault_.store(false, std::memory_order_relaxed);
    roExplicit_.store(false, std::memory_order_relaxed); /* §11.4: consent is a write action — exit read-only */
    bool haveBundle = false;
    harp_hash target{};
    { std::lock_guard<std::mutex> blk(bundleMutex_); haveBundle = hasBundle_; target = bundleTarget_; }
    log_msg("recall: engine-version consent granted — re-applying project");
    if (haveBundle && connected()) {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        pushStateLocked(target);
    }
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
    /* The ctl round-trips below (refsLocked / snapshotLocked / fetchClosureLocked) are each bounded by
     * the §15.1 live-session SO_RCVTIMEO; on a loaded Windows runner a single response can land just past
     * it and fail a project SAVE that should have succeeded — the GET-path mirror of the SET/push path's
     * non-fatal posture (a transient push failure is already logged-and-tolerated, not fatal). Retry the
     * round-trips a few times on a TRANSPORT-level failure while still connected(), 100 ms apart. An
     * AUTHORITATIVE empty state (live.unborn) returns false immediately — a real "nothing to save" is
     * never retried — and a genuine failure still returns false after the attempts, so the save's
     * caller still sees the error and the engine-mismatch/recall assertions are unchanged. */
    harp_ref live;
    harp_hash head;
    bool got = false;
    for (int attempt = 0; attempt < 3 && connected(); attempt++) {
        if (!refsLocked(&live)) { harp_sleep_ns(100000000ull); continue; } /* transient ctl hiccup */
        if (live.unborn) return false;                  /* authoritative: nothing to save (not transient) */
        head = live.hash;
        if (live.dirty && !snapshotLocked(&head)) { harp_sleep_ns(100000000ull); continue; }
        if (!fetchClosureLocked(head)) { harp_sleep_ns(100000000ull); continue; }
        got = true;
        break;
    }
    if (!got) return false;

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
            std::string ser(s ? s : "", s ? sl : 0);
            /* §15.3: an EthTransport-synthesized "eth:<peer>:<port>" serial is NOT a USB selection
             * target — return empty so the registry maps an eth bundle to a fresh runtime (not a
             * phantom USB-serial shared instance), matching setStateBundle's wantUsb_ guard. */
            return ser.rfind("eth:", 0) == 0 ? std::string() : ser;
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
    bool haveBundlePmh = false; /* §9.3/§13.4: the project's expected param-map-hash */
    harp_hash bundlePmh{};
    int bundleEngineMajor = 0; /* §12.2: the project's engine major (from identity-expectation) */
    std::string bundleSerial;  /* §12.2: the device serial the project was saved on (HIGH #4) */
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
                                if (hl == HARP_HASH_LEN) { /* retain; compared at apply */
                                    memcpy(bundlePmh.b, hp, HARP_HASH_LEN);
                                    haveBundlePmh = true;
                                }
                            } else if (ek == 1) { /* §12.2: engine semver -> retain the leading
                                                   * major for the fresh-open read-only gate */
                                const char *es;
                                size_t esl;
                                if (!harp_cdec_text(&d, &es, &esl)) return false;
                                bundleEngineMajor = 0;
                                for (size_t c = 0; c < esl && es[c] >= '0' && es[c] <= '9'; c++)
                                    bundleEngineMajor = bundleEngineMajor * 10 + (es[c] - '0');
                            } else if (!harp_cdec_skip(&d))
                                return false;
                        }
                    } else if (ik == 2) { /* §12.2: the device serial the project was saved on */
                        const char *ss;
                        size_t ssl;
                        if (!harp_cdec_text(&d, &ss, &ssl)) return false;
                        bundleSerial.assign(ss, ssl);
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
                    /* §15.3: an "eth:<peer>:<port>" serial (EthTransport identity over the §8.7 link)
                     * is NOT a USB target — keep wantUsb_ false so selectDevice falls through to
                     * mDNS/eth discovery on reopen instead of looping on an impossible USB bind
                     * (med-bundle-key-reconnect). */
                    wantUsb_ = (!wantUsbSerial_.empty() && wantUsbSerial_.rfind("eth:", 0) != 0);
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
        bundleParamMapHashSet_ = haveBundlePmh; /* retained for the connect-apply re-check */
        if (haveBundlePmh) bundleParamMapHash_ = bundlePmh;
        wantEngineMajor_.store(bundleEngineMajor, std::memory_order_relaxed); /* §12.2 read-only baseline */
        wantSerial_ = bundleSerial; /* §12.2 serial-differs read-only baseline (HIGH #4) */
        consentEngineMajor_.store(false, std::memory_order_relaxed); /* §13.4 consent is per-project */
        engineRefused_.store(false, std::memory_order_relaxed);      /* clear any prior refusal hold */
        roExplicit_.store(false, std::memory_order_relaxed);         /* §11.4: an explicit Open-read-only
            hold is per-project — a freshly staged bundle is a NEW project and must not inherit the prior
            session's read-only choice (which would leave it held read-only, "not auto-applied", on connect).
            Reconnect to the SAME project does not pass through here, so the intended persistence survives. */
        bundleParams_.clear();
        paramsFromStore(&store_, target, bundleParams_);
    }

    if (connected()) {
        /* §9.3/§13.4: state arrived while connected — warn now if the device's
         * automatable param map drifted from what the project expects. A bundle that
         * staged offline gets the same check when it applies in the connect handler. */
        if (haveBundlePmh && memcmp(bundlePmh.b, paramMapHash_.b, HARP_HASH_LEN) != 0)
            log_param_map_drift();
        /* §12.2/§13.4: a project staged WHILE connected must get the SAME read-only holds the connect
         * handler computes — recompute them against the LIVE identity before any auto-push. Without
         * this, staging a unit-A project while bound to a same-engine DIFFERENT unit B silently
         * auto-pushed A's state onto B (the device can't self-protect: the engine matches, so the §13.4
         * device gate never fires). Held read-only here -> the user pushes explicitly to bind it. */
        recomputeReadOnlyHolds();
        if (readOnlyDefault_.load(std::memory_order_relaxed) || roExplicit_.load(std::memory_order_relaxed)) {
            log_msg("recall bundle staged while connected, but held read-only (different unit / engine) — not auto-applied");
            recordLog(HARP_LOG_INFO, "recall", "staged-while-connected held read-only — not auto-applied");
            return true;
        }
        std::lock_guard<std::mutex> lk(ctlMutex_);
        if (!pushStateLocked(target)) {
            /* The bundle is STAGED (recorded above) — only the IMMEDIATE auto-push failed, and a
             * failed push here is recoverable, not data loss: a transient ctl error (a request/
             * response delayed past the live-session recv bound while the just-started audio stream
             * loads the link — intermittent on a loaded Windows runner) leaves the staged project
             * intact, and "Live wins" re-asserts it on the next reconnect/operation. The connect-time
             * re-assert already treats the SAME pushStateLocked failure as recoverable (helloAndIdentity:
             * "project state apply failed (will retry on reconnect)") and carries on; this path must
             * match it. Propagating false made the VST3 host's component->setState fail FATALLY for a
             * recoverable hiccup — the intermittent `staged-connected` Windows flake. A genuinely
             * malformed bundle still returns false EARLY (above), so setState's error contract holds.
             *
             * Distinguish WHY it failed (the connect-time re-assert at helloAndIdentity does the same):
             * pushStateLocked sets readOnlyDefault_/engineRefused_ when the device REFUSED the project
             * (§13.4 incompatible engine) — a PERMANENT hold, not a transient hiccup. Log it as a
             * read-only hold so the session log isn't misleading; a real transport timeout (no flag
             * set) is the transient/deferred case. Either way the bundle is staged and setState is
             * non-fatal (a held project must not crash the host any more than a deferred one). */
            if (readOnlyDefault_.load(std::memory_order_relaxed) ||
                roExplicit_.load(std::memory_order_relaxed) ||
                engineRefused_.load(std::memory_order_relaxed)) {
                log_msg("recall: staged-while-connected held read-only (device refused / mismatch) — not auto-applied");
                recordLog(HARP_LOG_WARN, "recall",
                          "staged-while-connected held read-only (device refused / mismatch) — not auto-applied");
            } else {
                log_msg("recall: staged-while-connected auto-push deferred (transient ctl error) — staged; re-asserts on reconnect");
                recordLog(HARP_LOG_WARN, "recall",
                          "staged-while-connected auto-push deferred (transient); staged, will re-assert on reconnect");
            }
        }
        return true;
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
