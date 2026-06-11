#include "runtime.h"

#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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

HarpRuntime &HarpRuntime::instance() {
    static HarpRuntime rt;
    return rt;
}

HarpRuntime::HarpRuntime() {
    harp_link_init(&link_);
    harp_cbuf_init(&msg_);
    const char *home = getenv("HOME");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/Library/Application Support/HARP/store",
             home ? home : "/tmp");
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
        if (harp_link_recv(io_, &link_, &stream, &msg_) != 0) return;
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

/* ---------------- lifecycle ---------------- */

bool HarpRuntime::start(uint32_t sampleRate) {
    if (running_.load()) return connected();
    rate_ = sampleRate;
    io_ = harp_usb_open();
    if (!io_) {
        log_msg("no HARP device on the bus; rendering silence");
        return false;
    }
    if (!harp_usb_has_audio(io_)) {
        log_msg("device has no HARP stream endpoints");
        harp_usb_close(io_);
        io_ = nullptr;
        return false;
    }
    /* fresh client per session: rid space and credit restart with hello */
    harp_client_free(&client_);
    harp_client_init(&client_, io_, &link_, storeOk_ ? &store_ : nullptr, nullptr,
                     nullptr);
    {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        if (!helloAndIdentity()) {
            log_msg("hello failed");
            harp_usb_close(io_);
            io_ = nullptr;
            return false;
        }
        log_msg("connected: %s %s (serial %s, engine %s %s)", vendorName_.c_str(),
                productName_.c_str(), serial_.c_str(), engineId_.c_str(),
                engineVer_.c_str());
        /* apply staged recall from a setState that arrived pre-connect.
         * Copy the target out first: pushStateLocked clears the staged
         * flag under stagedMutex_ itself. */
        bool staged = false;
        harp_hash target{};
        {
            std::lock_guard<std::mutex> slk(stagedMutex_);
            staged = hasStaged_;
            target = stagedTarget_;
        }
        if (staged) {
            if (pushStateLocked(target))
                log_msg("staged project state applied");
            else
                log_msg("staged state apply failed (left staged)");
        }
        if (!audioStart(rate_)) {
            log_msg("audio.start failed");
            harp_usb_close(io_);
            io_ = nullptr;
            return false;
        }
    }
    /* drain any stale stream bytes before pacing */
    uint8_t junk[16384];
    while (harp_usb_audio_read(io_, junk, sizeof junk, 30) > 0) {}

    ssi_ = framesSent_ = framesRecv_ = 0;
    framesRecvAtomic_.store(0, std::memory_order_relaxed);
    ssiRead_.store(0, std::memory_order_relaxed);
    padDebtFloats_ = 0;
    ahead_ = 2; /* small fixed pipeline; the reader thread keeps RTT short */
    audioRing_.clear();
    running_.store(true);
    connected_.store(true, std::memory_order_release);
    feederThread_ = std::thread([this] { feeder(); });
    readerThread_ = std::thread([this] { reader(); });
    return true;
}

void HarpRuntime::stop() {
    /* Flush in-flight events before teardown: the DAW's final note-offs
     * arrive in the last process() blocks, and killing the feeder with
     * them still queued is how notes get stuck. Bounded wait. */
    if (running_.load(std::memory_order_acquire)) {
        for (int i = 0; i < 100 && !timedRing_.empty(); i++) {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, nullptr);
        }
    }
    if (!running_.exchange(false)) {
        if (io_) {
            harp_usb_close(io_);
            io_ = nullptr;
        }
        return;
    }
    if (feederThread_.joinable()) feederThread_.join();
    if (readerThread_.joinable()) readerThread_.join();
    {
        std::lock_guard<std::mutex> lk(ctlMutex_);
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
    connected_.store(false, std::memory_order_release);
    harp_client_free(&client_); /* idempotent; start() re-inits per session */
    harp_usb_close(io_);
    io_ = nullptr;
    log_msg("stopped (underruns: %llu, padded samples: %llu)",
            (unsigned long long)underruns_.load(),
            (unsigned long long)padSamples_.load());
}

/* ---------------- audio thread side ---------------- */

void HarpRuntime::queueParamSet(uint32_t id, float v, uint64_t ts) {
    if (!timedRing_.push({0, id, v, ts, 0}))
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueRamp(uint32_t id, float target, uint64_t start, uint64_t end) {
    if (!timedRing_.push({1, id, target, start, end}))
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void HarpRuntime::queueNote(uint32_t word, uint64_t ts) {
    if (!timedRing_.push({2, word, 0.0f, ts, 0})) {
        evDrops_.fetch_add(1, std::memory_order_relaxed);
        /* A dropped note-ON is a missing note; a dropped note-OFF is a
         * note that never stops. If anything but a note-on is lost,
         * escalate to all-notes-off — silence beats a stuck drone. */
        uint32_t status = (word >> 20) & 0xf, vel = word & 0x7f;
        bool isNoteOn = status == 0x9 && vel > 0;
        if (!isNoteOn) panicPending_.store(true, std::memory_order_release);
    }
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
        struct timespec ts = {0, 500000}; /* 0.5 ms */
        nanosleep(&ts, nullptr);
        waited++;
    }
    return 0;
}

/* ---------------- feeder thread ---------------- */

void HarpRuntime::feeder() {
#ifdef __APPLE__
    /* The feeder does the time-critical pacing for the whole plugin; at
     * default QoS it loses races to everything the DAW runs elevated. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (running_.load(std::memory_order_relaxed)) {
        bool didWork = false;

        /* 1. inbound async traffic FIRST: keeping the IN direction drained
         * means the device is never blocked writing to us — which means OUR
         * writes below never stall (§4.2.1; learned the hard way under
         * automation flood) */
        pollEcho();

        /* 1b. escalated panic: a note-off was lost to overflow — send
         * all-notes-off (CC 123) ahead of everything else */
        if (panicPending_.exchange(false, std::memory_order_acq_rel)) {
            harp_cbuf m;
            harp_cbuf_init(&m);
            encodeUmpEvent(&m, 0x20B07B00u, 0); /* CC 123, now */
            std::lock_guard<std::mutex> lk(ctlMutex_);
            harp_link_send(io_, HARP_STREAM_EVT, m.buf, m.len);
            harp_cbuf_free(&m);
            log_msg("WARNING: note-off lost to overflow; sent all-notes-off");
        }

        /* 2. timestamped events (params, ramps, notes — §9.2/§9.4/§9.10),
         * sent BEFORE pacing: a pacing frame triggers the render of its
         * SSIs, so events timestamped within them must already be on the
         * wire — events after pacing intermittently lost the race and
         * applied a block late (audible 5-15 ms slop on some notes).
         * Batched into ONE framed bulk write (per-event writes starved
         * pacing); bounded per iteration: audio outranks events. */
        {
            harp_cbuf msgbuf, batch;
            harp_cbuf_init(&msgbuf);
            harp_cbuf_init(&batch);
            TimedEv te;
            int sent = 0;
            for (; sent < 32 && timedRing_.pop(te); sent++) {
                harp_cbuf_reset(&msgbuf);
                if (te.kind == 0)
                    encodeParamEvent(&msgbuf, te.a, te.v, te.ts);
                else if (te.kind == 1)
                    encodeRampEvent(&msgbuf, te.a, te.v, te.ts, te.end);
                else
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
                io_->write_all(io_, batch.buf, batch.len);
                didWork = true;
            }
            harp_cbuf_free(&msgbuf);
            harp_cbuf_free(&batch);
        }

        /* 3. pace: ring to target depth, small fixed pipeline on top. The
         * reader thread keeps an audio-IN read permanently pending, so the
         * device's response writes land instantly and its pacing turnaround
         * is just render time. */
        size_t ringFrames = audioRing_.readAvailable() / 2;
        uint64_t inFlight = framesSent_ - framesRecv_;
        while (ringFrames < (size_t)targetFrames_ && inFlight < ahead_) {
            harp_audio_hdr pace = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D, 0,    0,
                                   ssi_,            (uint16_t)kBlock,   HARP_AUDIO_FMT_F32};
            uint8_t ph[HARP_AUDIO_HDR_LEN];
            harp_audio_hdr_encode(&pace, ph);
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
            struct timespec ts = {0, 1000000}; /* 1 ms */
            nanosleep(&ts, nullptr);
        }
    }
}

/* Audio-IN reader: one blocking read always pending. This is what makes the
 * device's response writes complete immediately — the §4.2.1 always-pending
 * inbound rule applied to the stream pair. */
void HarpRuntime::reader() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    uint8_t acc[65536];
    size_t accLen = 0;
    while (running_.load(std::memory_order_relaxed)) {
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
        gmtime_r(&now, &tm);
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
                          const harp_ref &ref, harp_store *store, HashList &closure) {
    harp_cbor_map(out, 5);
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
}

bool HarpRuntime::getStateBundle(std::vector<uint8_t> &out) {
    if (!storeOk_) return false;
    if (!connected()) {
        /* offline: re-emit staged bundle if we have one */
        std::lock_guard<std::mutex> slk(stagedMutex_);
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
                  engineVer_, paramMapHash_, expected, &store_, clo);
    out.assign(b.buf, b.buf + b.len);
    harp_cbuf_free(&b);
    { /* a save moves the project's reference point to what it captured */
        std::lock_guard<std::mutex> slk(stagedMutex_);
        hasStaged_ = true;
        stagedTarget_ = head;
    }
    char hex[2 * HARP_HASH_LEN + 1];
    harp_hash_hex(&head, hex);
    hex[12] = 0;
    log_msg("recall bundle saved: live/project @ %s… (%zu bytes)", hex, out.size());
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
        std::lock_guard<std::mutex> slk(stagedMutex_);
        hasStaged_ = true;
        stagedTarget_ = target;
        stagedParams_.clear();
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        harp_hash root;
        if (harp_store_get(&store_, &target, &enc) == 0 &&
            harp_obj_parse_snapshot_root(enc.buf, enc.len, &root)) {
            harp_cbuf tree;
            harp_cbuf_init(&tree);
            if (harp_store_get(&store_, &root, &tree) == 0) {
                struct Ctx {
                    HarpRuntime *rt;
                } ctx{this};
                harp_obj_tree_foreach(
                    tree.buf, tree.len,
                    [](const char *name, size_t nl, const harp_hash *h, uint32_t kind,
                       void *ud) -> bool {
                        if (nl != 6 || memcmp(name, "params", 6) != 0 || kind != 0)
                            return true;
                        auto *rt = ((Ctx *)ud)->rt;
                        harp_cbuf blob;
                        harp_cbuf_init(&blob);
                        if (harp_store_get(&rt->store_, h, &blob) == 0) {
                            const uint8_t *pl;
                            size_t pll;
                            if (harp_obj_parse_blob(blob.buf, blob.len, nullptr, nullptr,
                                                    &pl, &pll)) {
                                harp_cdec pd;
                                harp_cdec_init(&pd, pl, pll);
                                uint64_t pn;
                                if (harp_cdec_map(&pd, &pn)) {
                                    for (uint64_t k = 0; k < pn; k++) {
                                        uint64_t id;
                                        double v;
                                        if (!harp_cdec_uint(&pd, &id) ||
                                            !harp_cdec_float(&pd, &v))
                                            break;
                                        rt->stagedParams_.push_back(
                                            {(uint32_t)id, (float)v});
                                    }
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

    if (connected()) {
        std::lock_guard<std::mutex> lk(ctlMutex_);
        return pushStateLocked(target);
    }
    log_msg("recall bundle staged (device offline); will apply on connect");
    return true;
}

bool HarpRuntime::stagedParam(uint32_t id, float &value) {
    std::lock_guard<std::mutex> slk(stagedMutex_);
    for (auto &kv : stagedParams_)
        if (kv.first == id) {
            value = kv.second;
            return true;
        }
    return false;
}
