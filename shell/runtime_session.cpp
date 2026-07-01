/* shell/runtime_session.cpp — the §5/§12 session + device-selection lifecycle.
 *
 * Extracted VERBATIM from runtime.cpp (a pure translation-unit split — no behavior
 * change; the connect/hello/identity flow, the read-only holds, and the wire bytes
 * stay identical). Holds the HarpRuntime session plane: the §5.4/§12 hello + identity
 * capture (helloAndIdentity), the §12.2/§13.4 read-only hold recompute
 * (recomputeReadOnlyHolds, shared with runtime_recall.cpp's setStateBundle), the
 * §6.1/§8.7 device selection + mDNS discovery (selectDevice + its file-local
 * wrapUsb / ethEngineIs / discoverEthDevice helpers), the §8.3-over-§8.7 live<->offline
 * toggle (setOffline), and the connection lifecycle proper — one-attempt bring-up
 * (sessionUp), orderly teardown (sessionDown), the reconnect supervisor (supervisor),
 * and start/stop. All member declarations already live in runtime.h, so this is
 * purely a move: the same code, compiled into its own object and linked into every
 * shell target.
 *
 * NOT moved (stays in runtime.cpp): the audio.start negotiation (audioStart) and its
 * §8.4 admission helpers, the RT feeder/reader/eventPump, and the event plane — this
 * cut deliberately touches NONE of the RT scheduling, the lock-free SPSC ring, or the
 * atomics ordering. sessionUp/Down only SPAWN and JOIN those threads (verbatim);
 * ledger_release / log_msg / log_param_map_drift are shared via the headers below.
 */
#include "runtime.h"
#include "runtime_registry.h" /* §8.4 ledger_release (sessionDown frees the reservation) */
#include "runtime_log.h"      /* log_msg / log_param_map_drift (shared w/ runtime.cpp) */
#include "shell_config.h"     /* HARP_SHELL_ENGINE_FILTER / HARP_SHELL_ETHERNET_ONLY */
#include "usb_transport.h"    /* UsbTransport — the concrete USB binding selectDevice() wraps */
#include "eth_transport.h"    /* EthTransport::dial + sock_io.h (ethEngineIs); winsock2 on _WIN32 */

#ifdef __APPLE__
#include <pthread/qos.h> /* supervisor QoS: QOS_CLASS_USER_INTERACTIVE */
#endif
#include <cstdio>  /* snprintf; ~/.config/harp/device fopen/fgets */
#include <cstdlib> /* getenv / atoi / strtoul / strtoull */
#include <cstring> /* strcmp / strcspn / memcmp / strlen */

#include "harp/plat.h" /* harp_plat_init / harp_sleep_ns / harp_thread_set_realtime */

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
