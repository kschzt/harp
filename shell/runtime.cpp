#include "runtime.h"

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

/* Send req, pump the link until its response arrives. Objects on the obj
 * stream land in the store; notifications are tolerated. Caller holds
 * ctlMutex_. */
bool HarpRuntime::request(harp_cbuf *req, harp_cbuf *rsp, harp_env *e) {
    uint64_t rid = nextRid_;
    if (harp_link_send(io_, HARP_STREAM_CTL, req->buf, req->len) != 0) return false;
    for (;;) {
        uint8_t stream;
        if (harp_link_recv(io_, &link_, &stream, &msg_) != 0) return false;
        if (stream == HARP_STREAM_OBJ) {
            if (storeOk_) harp_store_put(&store_, msg_.buf, msg_.len, nullptr);
            continue;
        }
        if (stream != HARP_STREAM_CTL) continue;
        harp_env pe;
        if (!harp_env_parse(msg_.buf, msg_.len, &pe)) return false;
        if (pe.msgtype == HARP_MSG_NOTIFICATION) continue;
        if (pe.rid != rid) continue;
        if (pe.msgtype == HARP_MSG_ERROR) {
            log_msg("device error on %s", pe.method);
            return false;
        }
        harp_cbuf_reset(rsp);
        harp_cbuf_put(rsp, msg_.buf, msg_.len);
        return harp_env_parse(rsp->buf, rsp->len, e);
    }
}

static void req_head(harp_cbuf *out, uint64_t rid, const char *method, bool body) {
    harp_cbuf_reset(out);
    harp_env_head(out, HARP_MSG_REQUEST, rid, method, body);
}

bool HarpRuntime::helloAndIdentity() {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(&req, ++nextRid_, "core.hello", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_array(&req, 2);
    harp_cbor_uint(&req, 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, 1);
    harp_cbor_text(&req, "harp-shell 0.1 (VST3)");
    harp_env e;
    bool ok = request(&req, &rsp, &e);
    if (ok && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n;
        ok = false;
        if (harp_cdec_map(&b, &n)) {
            for (uint64_t i = 0; i < n && !b.err; i++) {
                uint64_t key;
                if (!harp_cdec_uint(&b, &key)) break;
                if (key != 1) {
                    harp_cdec_skip(&b);
                    continue;
                }
                /* identity map */
                uint64_t in;
                if (!harp_cdec_map(&b, &in)) break;
                ok = true;
                for (uint64_t j = 0; j < in && !b.err; j++) {
                    uint64_t ik;
                    if (!harp_cdec_uint(&b, &ik)) break;
                    const char *s;
                    size_t sl;
                    if (ik == 0 || ik == 1) { /* vendor / product */
                        uint64_t mn, mk, idnum = 0;
                        std::string name;
                        if (!harp_cdec_map(&b, &mn)) break;
                        for (uint64_t k = 0; k < mn; k++) {
                            if (!harp_cdec_uint(&b, &mk)) break;
                            if (mk == 0)
                                harp_cdec_uint(&b, &idnum);
                            else if (mk == 1 && harp_cdec_text(&b, &s, &sl))
                                name.assign(s, sl);
                            else if (mk > 1)
                                harp_cdec_skip(&b);
                        }
                        if (ik == 0) {
                            vendorId_ = (uint32_t)idnum;
                            vendorName_ = name;
                        } else {
                            productId_ = (uint32_t)idnum;
                            productName_ = name;
                        }
                    } else if (ik == 2 && harp_cdec_text(&b, &s, &sl)) {
                        serial_.assign(s, sl);
                    } else if (ik == 4) { /* engine */
                        uint64_t mn, mk;
                        if (!harp_cdec_map(&b, &mn)) break;
                        for (uint64_t k = 0; k < mn; k++) {
                            if (!harp_cdec_uint(&b, &mk)) break;
                            if (mk == 0 && harp_cdec_text(&b, &s, &sl))
                                engineId_.assign(s, sl);
                            else if (mk == 1 && harp_cdec_text(&b, &s, &sl))
                                engineVer_.assign(s, sl);
                            else if (mk == 2) {
                                const uint8_t *hp;
                                size_t hl;
                                if (harp_cdec_bytes(&b, &hp, &hl) && hl == HARP_HASH_LEN)
                                    memcpy(paramMapHash_.b, hp, HARP_HASH_LEN);
                            } else if (mk > 2)
                                harp_cdec_skip(&b);
                        }
                    } else {
                        harp_cdec_skip(&b);
                    }
                }
            }
        }
    }
    if (ok) {
        /* grant obj credit for pulls */
        harp_cbuf m;
        harp_cbuf_init(&m);
        harp_env_head(&m, HARP_MSG_NOTIFICATION, 0, "core.credit", true);
        harp_cbor_map(&m, 1);
        harp_cbor_uint(&m, 0);
        harp_cbor_uint(&m, CREDIT_GRANT);
        harp_link_send(io_, HARP_STREAM_CTL, m.buf, m.len);
        harp_cbuf_free(&m);
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return ok;
}

bool HarpRuntime::audioStart(uint32_t rate) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(&req, ++nextRid_, "audio.start", true);
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
    req_head(&req, ++nextRid_, "audio.stop", false);
    harp_env e;
    request(&req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
}

bool HarpRuntime::pushKnob(uint32_t id, float v) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(&req, ++nextRid_, "x.harp-refdev.knob", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_uint(&req, id);
    harp_cbor_uint(&req, 1);
    harp_cbor_float(&req, v);
    harp_env e;
    bool ok = request(&req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return ok;
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
    ahead_ = 4;
    audioRing_.clear();
    running_.store(true);
    connected_.store(true, std::memory_order_release);
    feederThread_ = std::thread([this] { feeder(); });
    return true;
}

void HarpRuntime::stop() {
    if (!running_.exchange(false)) {
        if (io_) {
            harp_usb_close(io_);
            io_ = nullptr;
        }
        return;
    }
    if (feederThread_.joinable()) feederThread_.join();
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
    harp_usb_close(io_);
    io_ = nullptr;
    log_msg("stopped (underruns: %llu)", (unsigned long long)underruns_.load());
}

/* ---------------- audio thread side ---------------- */

void HarpRuntime::setParam(uint32_t id, float v) { paramRing_.push({id, v}); }

size_t HarpRuntime::pullAudio(float *dst, size_t nFrames) {
    size_t want = nFrames * 2;
    size_t got = audioRing_.read(dst, want);
    if (got < want) {
        memset(dst + got, 0, (want - got) * sizeof(float));
        if (connected_.load(std::memory_order_acquire))
            underruns_.fetch_add(1, std::memory_order_relaxed);
        return (want - got) / 2;
    }
    return 0;
}

size_t HarpRuntime::pullAudioBlocking(float *dst, size_t nFrames, unsigned timeoutMs) {
    size_t want = nFrames * 2;
    size_t got = 0;
    unsigned waited = 0;
    while (got < want) {
        got += audioRing_.read(dst + got, want - got);
        if (got >= want) break;
        if (!connected_.load(std::memory_order_acquire) || waited >= timeoutMs) {
            memset(dst + got, 0, (want - got) * sizeof(float));
            underruns_.fetch_add(1, std::memory_order_relaxed);
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
    uint8_t acc[65536];
    size_t accLen = 0;
    /* Pending knob values, coalesced to the last value per param: a slider
     * drag floods one id; only the newest value matters. Audio outranks
     * knobs (§9.2: event overload must never glitch audio). */
    struct {
        uint32_t id;
        float value;
    } pending[16];
    size_t npending = 0;
    while (running_.load(std::memory_order_relaxed)) {
        bool didWork = false;
        /* 1. drain + coalesce param changes (no I/O yet) */
        ParamChange pc;
        while (paramRing_.pop(pc)) {
            size_t i = 0;
            while (i < npending && pending[i].id != pc.id) i++;
            if (i < npending)
                pending[i].value = pc.value;
            else if (npending < 16)
                pending[npending++] = {pc.id, pc.value};
        }

        /* 2. pace: keep ring occupancy + in-flight under the target depth */
        size_t ringFrames = audioRing_.readAvailable() / 2;
        uint64_t inFlight = framesSent_ - framesRecv_;
        while (ringFrames + (inFlight + 1) * kBlock <= (size_t)kTargetDepthFrames * kBlock &&
               inFlight < ahead_) {
            harp_audio_hdr pace = {HARP_AUDIO_FVER, HARP_AUDIO_DIR_H2D, 0,    0,
                                   ssi_,            (uint16_t)kBlock,   HARP_AUDIO_FMT_F32};
            uint8_t ph[HARP_AUDIO_HDR_LEN];
            harp_audio_hdr_encode(&pace, ph);
            if (!harp_usb_audio_write(io_, ph, sizeof ph, 50)) {
                if (inFlight >= 1 && inFlight < ahead_) ahead_ = inFlight;
                break;
            }
            ssi_ += kBlock;
            framesSent_++;
            inFlight++;
            didWork = true;
        }

        /* 3. drain rendered frames into the ring */
        if (framesSent_ > framesRecv_) {
            int r = harp_usb_audio_read(io_, acc + accLen, (int)(sizeof acc - accLen), 20);
            if (r < 0) {
                log_msg("audio stream read failed; device gone?");
                connected_.store(false, std::memory_order_release);
                break;
            }
            accLen += (size_t)r;
            size_t off = 0;
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
                framesRecv_++;
                off += need;
                didWork = true;
            }
            memmove(acc, acc + off, accLen - off);
            accLen -= off;
        }

        /* 4. push at most ONE knob per iteration, and only with audio
         * headroom — each push is a blocking control-plane round trip */
        if (npending && audioRing_.readAvailable() / 2 >= kBlock) {
            std::lock_guard<std::mutex> lk(ctlMutex_);
            pushKnob(pending[0].id, pending[0].value);
            memmove(&pending[0], &pending[1], (--npending) * sizeof pending[0]);
            didWork = true;
        }

        if (!didWork) {
            struct timespec ts = {0, 1000000}; /* 1 ms */
            nanosleep(&ts, nullptr);
        }
    }
}

/* ---------------- state: pull / push / bundle ---------------- */

bool HarpRuntime::refsLocked(harp_ref *live) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(&req, ++nextRid_, "state.refs", false);
    harp_env e;
    bool found = false;
    if (request(&req, &rsp, &e) && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key, alen;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_array(&b, &alen)) {
            for (uint64_t i = 0; i < alen; i++) {
                harp_ref r;
                if (!harp_ref_decode(&b, &r)) break;
                if (strcmp(r.name, LIVE_REF) == 0) {
                    *live = r;
                    found = true;
                }
            }
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return found;
}

bool HarpRuntime::snapshotLocked(harp_hash *out) {
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(&req, ++nextRid_, "state.snapshot", true);
    harp_cbor_map(&req, 2);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, LIVE_REF);
    harp_cbor_uint(&req, 1);
    harp_cbor_text(&req, "VST3 shell");
    harp_env e;
    bool ok = false;
    if (request(&req, &rsp, &e) && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key;
        const uint8_t *hp;
        size_t hl;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_bytes(&b, &hp, &hl) && hl == HARP_HASH_LEN) {
            memcpy(out->b, hp, HARP_HASH_LEN);
            ok = true;
        }
    }
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    return ok;
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
    HashList pending;
    pending.add(root);
    size_t cursor = 0;
    while (cursor < pending.n) {
        /* want what we don't hold (in batches) */
        harp_hash want[64];
        size_t nwant = 0;
        for (size_t i = cursor; i < pending.n && nwant < 64; i++)
            if (!harp_store_have(&store_, &pending.h[i])) want[nwant++] = pending.h[i];
        if (nwant) {
            harp_cbuf req, rsp;
            harp_cbuf_init(&req);
            harp_cbuf_init(&rsp);
            req_head(&req, ++nextRid_, "state.want", true);
            harp_cbor_map(&req, 1);
            harp_cbor_uint(&req, 0);
            harp_cbor_array(&req, nwant);
            for (size_t i = 0; i < nwant; i++)
                harp_cbor_bytes(&req, want[i].b, HARP_HASH_LEN);
            harp_env e;
            bool ok = request(&req, &rsp, &e);
            harp_cbuf_free(&req);
            harp_cbuf_free(&rsp);
            if (!ok) return false;
            /* pump until all wanted objects landed (request() stores them;
             * here we keep receiving until present) */
            int spins = 0;
            for (;;) {
                bool all = true;
                for (size_t i = 0; i < nwant; i++)
                    if (!harp_store_have(&store_, &want[i])) all = false;
                if (all) break;
                if (++spins > 4096) return false;
                uint8_t stream;
                if (harp_link_recv(io_, &link_, &stream, &msg_) != 0) return false;
                if (stream == HARP_STREAM_OBJ)
                    harp_store_put(&store_, msg_.buf, msg_.len, nullptr);
            }
        }
        /* expand children */
        size_t end = pending.n;
        for (; cursor < end; cursor++) {
            harp_cbuf enc;
            harp_cbuf_init(&enc);
            if (harp_store_get(&store_, &pending.h[cursor], &enc) == 0)
                harp_obj_foreach_child(enc.buf, enc.len, false, collect_cb, &pending);
            harp_cbuf_free(&enc);
        }
    }
    return true;
}

/* Push staged target to the device: the §11.4 Push with archive-before-push.
 * Caller holds ctlMutex_. */
bool HarpRuntime::pushStateLocked(const harp_hash &target) {
    harp_ref live;
    if (!refsLocked(&live)) return false;
    if (!live.unborn && !live.dirty && harp_hash_eq(&live.hash, &target)) {
        log_msg("recall: hash match, clean -> SYNCED silently");
        std::lock_guard<std::mutex> slk2(stagedMutex_);
        hasStaged_ = false;
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
        harp_cbuf req, rsp;
        harp_cbuf_init(&req);
        harp_cbuf_init(&rsp);
        req_head(&req, ++nextRid_, "state.refset", true);
        harp_cbor_map(&req, 4);
        harp_cbor_uint(&req, 0);
        harp_cbor_text(&req, archive);
        harp_cbor_uint(&req, 1);
        harp_cbor_null(&req);
        harp_cbor_uint(&req, 2);
        harp_cbor_bytes(&req, deviceHead.b, HARP_HASH_LEN);
        harp_cbor_uint(&req, 3);
        harp_cbor_uint(&req, 1); /* create-if-unborn */
        harp_env e;
        bool ok = request(&req, &rsp, &e);
        harp_cbuf_free(&req);
        harp_cbuf_free(&rsp);
        if (!ok) return false;
    }

    /* transfer missing objects: have -> send -> obj stream */
    HashList clo;
    clo.add(target);
    for (size_t cur = 0; cur < clo.n; cur++) {
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        if (harp_store_get(&store_, &clo.h[cur], &enc) != 0) {
            harp_cbuf_free(&enc);
            log_msg("recall: local closure incomplete");
            return false;
        }
        harp_obj_foreach_child(enc.buf, enc.len, false, collect_cb, &clo);
        harp_cbuf_free(&enc);
    }
    harp_cbuf req, rsp;
    harp_cbuf_init(&req);
    harp_cbuf_init(&rsp);
    req_head(&req, ++nextRid_, "state.have", true);
    harp_cbor_map(&req, 1);
    harp_cbor_uint(&req, 0);
    harp_cbor_array(&req, clo.n);
    for (size_t i = 0; i < clo.n; i++) harp_cbor_bytes(&req, clo.h[i].b, HARP_HASH_LEN);
    harp_env e;
    bool ok = request(&req, &rsp, &e);
    bool missing[512] = {false};
    size_t nmissing = 0;
    if (ok && e.has_body) {
        harp_cdec b;
        harp_cdec_init(&b, e.body, e.body_len);
        uint64_t n, key, alen;
        if (harp_cdec_map(&b, &n) && n >= 1 && harp_cdec_uint(&b, &key) && key == 0 &&
            harp_cdec_array(&b, &alen) && alen == clo.n) {
            for (size_t i = 0; i < clo.n; i++) {
                bool have;
                if (!harp_cdec_bool(&b, &have)) break;
                if (!have) {
                    missing[i] = true;
                    nmissing++;
                }
            }
        }
    }
    if (!ok) {
        harp_cbuf_free(&req);
        harp_cbuf_free(&rsp);
        return false;
    }
    if (nmissing) {
        uint64_t total = 0;
        harp_cbuf enc;
        harp_cbuf_init(&enc);
        for (size_t i = 0; i < clo.n; i++)
            if (missing[i]) {
                harp_cbuf_reset(&enc);
                if (harp_store_get(&store_, &clo.h[i], &enc) == 0) total += enc.len;
            }
        req_head(&req, ++nextRid_, "state.send", true);
        harp_cbor_map(&req, 2);
        harp_cbor_uint(&req, 0);
        harp_cbor_array(&req, nmissing);
        for (size_t i = 0; i < clo.n; i++)
            if (missing[i]) harp_cbor_bytes(&req, clo.h[i].b, HARP_HASH_LEN);
        harp_cbor_uint(&req, 1);
        harp_cbor_uint(&req, total);
        ok = request(&req, &rsp, &e);
        for (size_t i = 0; ok && i < clo.n; i++) {
            if (!missing[i]) continue;
            harp_cbuf_reset(&enc);
            if (harp_store_get(&store_, &clo.h[i], &enc) != 0 ||
                harp_link_send(io_, HARP_STREAM_OBJ, enc.buf, enc.len) != 0)
                ok = false;
        }
        harp_cbuf_free(&enc);
        if (!ok) {
            harp_cbuf_free(&req);
            harp_cbuf_free(&rsp);
            return false;
        }
    }

    /* CAS the live ref */
    req_head(&req, ++nextRid_, "state.refset", true);
    harp_cbor_map(&req, live.unborn ? 4 : 3);
    harp_cbor_uint(&req, 0);
    harp_cbor_text(&req, LIVE_REF);
    harp_cbor_uint(&req, 1);
    if (live.unborn)
        harp_cbor_null(&req);
    else
        harp_cbor_bytes(&req, deviceHead.b, HARP_HASH_LEN);
    harp_cbor_uint(&req, 2);
    harp_cbor_bytes(&req, target.b, HARP_HASH_LEN);
    if (live.unborn) {
        harp_cbor_uint(&req, 3);
        harp_cbor_uint(&req, 1);
    }
    ok = request(&req, &rsp, &e);
    harp_cbuf_free(&req);
    harp_cbuf_free(&rsp);
    if (ok) {
        log_msg("recall: live/project restored");
        std::lock_guard<std::mutex> slk(stagedMutex_);
        hasStaged_ = false;
    }
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
