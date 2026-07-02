/* shell/runtime_audio.cpp — the §8.2/§8.3/§8.7 audio engine (feeder/reader/pump).
 *
 * Extracted VERBATIM from runtime.cpp (a pure translation-unit split — no behavior
 * change; the wire bytes, the pacing/rate-control math, the ASRC path, and every
 * atomic ordering stay identical). Holds the HarpRuntime AUDIO plane — the
 * real-time streaming engine and the §8.4 admission that gates it:
 *   - audioStart: the audio.start negotiation (host-paced USB / §8.3-over-§8.7
 *     offline TCP / free-running RTP) + its §8.4 admission-control helpers
 *     (the file-local usbSpeedBps / pathCapacityBps in an anon namespace);
 *   - audioRenegotiateLocked: the P5b stop -> new-union -> start re-neg (fence
 *     epoch advance, reader quiesce/respawn) on a continuous SSI domain;
 *   - feeder(): the supervisor-thread pacing loop — host-paced writes + the §8.7
 *     bit-exact audio.trim rate-control loop + the re-neg / §14.3-probe boundaries;
 *   - reader(): the audio-IN drain — USB framed audio, §8.7 bit-exact RTP, and the
 *     free-running ASRC (harp_freerun clock recovery + resample) glue;
 *   - eventPump(): the dedicated event->wire thread (drains the owner source via
 *     drainSource — now in runtime_events.cpp — and services the panic path);
 *   - demuxUnionFrame() / minRingFillFrames(): the per-part union demux + the
 *     multi-consumer fill gate shared by the USB and RTP readers.
 * All member declarations already live in runtime.h, so this is purely a move: the
 * same code, compiled into its own object and linked into every shell target.
 *
 * CRITICAL — this cut moves the lock-free SPSC PRODUCER, CONSUMER, and RING as ONE
 * UNIT: demuxUnionFrame writes audioRing_/sink rings, feeder writes fxInRing_/paces,
 * reader is the sole audioRing_ producer, eventPump is the sole source-ring consumer.
 * NOTHING about the atomics, memory_order, the THREAD_TIME_CONSTRAINT / SCHED_FIFO
 * scheduling (harp_thread_set_realtime), or the CoreAudio workgroup handoff is
 * altered — the rings themselves live in ring.h and their declarations in runtime.h;
 * this TU only holds the functions that drive them, byte-for-byte as before.
 */
#include "runtime.h"
#include "runtime_registry.h" /* §8.4 admission ledger (ledger_reserve/release — audioStart) */
#include "runtime_log.h"      /* log_msg (shared w/ runtime.cpp) */
#include "ump.h"              /* ump_all_notes_off (eventPump panic path) */
#include "eth_transport.h"    /* §8.7 EthTransport binding + rtp.h (harp_rtp_unwrap_ts) */
extern "C" {
#include "freerun.h" /* §8.7 ASRC: host-side clock recovery + resample (libsamplerate) */
}
#include <samplerate.h> /* SRC_* converter-quality enum for the freerun cfg */

#ifdef __APPLE__
#include <pthread/qos.h> /* reader/eventPump QoS (QOS_CLASS_USER_INTERACTIVE) */
#endif
#include <cstdio>  /* snprintf (audioRenegotiateLocked record-detail) */
#include <cstdlib> /* getenv / atoi / strtoull (§8.4 admission env + budget) */
#include <cstring> /* memset / memcpy (feeder pacing + reader ASRC snapshot) */
#include <mutex>   /* std::lock_guard over ctlMutex_ / sinksMutex_ */
#include <string>
#include <thread>
#include <vector>

#include "harp/plat.h" /* harp_now_ns / harp_sleep_ns / harp_thread_set_realtime */

#define CREDIT_GRANT (16u << 20)

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
    events_.advanceEpoch();

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
             * (the fence is a minimum -> at worst evt_late). The (hw-base) saturating
             * math is owned by EventManager::fenceStamp() — one owner for both this
             * feeder stamp and the §14.3 loopback probe's identical stamp. */
            uint32_t seq = events_.fenceStamp();
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

        events_.pollDropLog();

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
        if (events_.takePanic()) {
            harp_cbuf m;
            harp_cbuf_init(&m);
            EventManager::encodeUmpEvent(&m, ump_all_notes_off(), 0); /* CC 123, now */
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
        int sent = events_.drainOwner(batch, msgbuf, 64);
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
