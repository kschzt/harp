/* shell/runtime_recall.cpp — §11.4 recall/reconcile + §15.3 state-bundle codec.
 *
 * Extracted VERBATIM from runtime.cpp (a pure translation-unit split — no behavior
 * change; the state-bundle bytes and the reconcile flow stay identical). Holds the
 * HarpRuntime state plane: the pull/push/snapshot primitives (refsLocked /
 * snapshotLocked / fetchClosureLocked), the §11.4 push-with-archive + reconcile
 * (pushStateLocked / consentEngineMajorOverride), and the §15.3 bundle codec
 * (encode_bundle / getStateBundle / decode_param_map / decode_params_payload /
 * paramsFromStore / bundleParams / bundleWantedSerial / setStateBundle /
 * bundleParam), plus the file-local HashList / collect_cb closure walker they share.
 * All member declarations already live in runtime.h, so this is purely a move: the
 * same code, compiled into its own object and linked into every shell target.
 *
 * LIVE_REF + BUNDLE_MAGIC (used only by this state plane) move here with it;
 * log_msg / log_param_map_drift are shared via runtime_log.h.
 */
#include "runtime.h"
#include "runtime_log.h" /* log_msg / log_param_map_drift (shared w/ runtime.cpp) */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "harp/plat.h" /* harp_gmtime / harp_sleep_ns */

#define LIVE_REF "live/project"
#define BUNDLE_MAGIC "harpb"

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
