/* HarpRuntime — the embedded host runtime (§15.1, embedded-in-process;
 * the per-machine daemon split comes later). Shared by every shell
 * format (VST3, AU) — the shells are thin adapters over this.
 *
 * Owns the USB transport, the control link, the host-paced audio pump,
 * and the event plane. Threading contract:
 *   - DAW audio thread: pullAudio(), queueParamSet/Ramp/Note(),
 *     feedTransport(), streamPos() — lock-free, never blocks
 *   - supervisor thread: session lifecycle (connect/reconnect); runs
 *     feeder() while connected (pacing writes + inbound echo polling)
 *   - reader thread (per session): audio-IN always pending -> ring
 *   - event pump thread (per session): timed events -> wire, never
 *     behind a pacing stall (its deadline is ~one DAW block)
 *   - transport event thread (usb_io): async libusb completion reaping
 *   - main/UI thread: start(), stop(), getStateBundle(), setStateBundle()
 *     (control-plane requests serialize against the feeder via ctlMutex_)
 * On AU hosts all RT-adjacent threads join the host's CoreAudio
 * workgroup (setWorkgroup via RenderContextObserver).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#ifdef __APPLE__
#include <os/workgroup.h>
#endif
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ring.h"

extern "C" {
#include "harp/audio.h"
#include "harp/cbor.h"
#include "harp/envelope.h"
#include "harp/link.h"
#include "harp/object.h"
#include "harp/store.h"
#include "client.h"
#include "usb_io.h"
}

class HarpRuntime {
public:
    static HarpRuntime &instance();

    /* Called from setupProcessing: the ring cushion must scale with the
     * DAW's block size (a 1024-sample pull through a 1280-sample cushion
     * leaves 5 ms of headroom — stutter at large buffers). */
    void configure(uint32_t sampleRate, uint32_t maxDawBlock) {
        rate_ = sampleRate;
        maxDawBlock_ = maxDawBlock;
        uint32_t needed = 2 * maxDawBlock;
        uint32_t depth = kTargetDepthFrames;
        /* measurement/field override: HARP_CUSHION_BLOCKS=N (min 2). The
         * default is data-driven per transport generation — see the
         * kTargetDepthFrames comment. */
        if (const char *e = getenv("HARP_CUSHION_BLOCKS")) {
            int v = atoi(e);
            if (v >= 2 && v <= 64) depth = (uint32_t)v;
        }
        uint32_t floor_ = depth * kBlock;
        targetFrames_ = needed > floor_ ? needed : floor_;
    }

    /* Begin supervising: claim the device, hello, start the host-paced
     * stream — and keep a supervisor thread retrying/reconnecting for as
     * long as the plugin is active (unplug -> silence + retry; replug ->
     * session re-established and the project's bundle re-asserted).
     * Returns whether a device is connected RIGHT NOW. */
    bool start(uint32_t sampleRate);
    void stop();
    bool connected() const { return connected_.load(std::memory_order_acquire); }

    /* ---- audio thread (all lock-free) ---- */
    void queueParamSet(uint32_t id, float v, uint64_t ts);
    void queueRamp(uint32_t id, float target, uint64_t start, uint64_t end);
    void queueNote(uint32_t umpWord, uint64_t ts);
    /* §9.7 transport anchor: (ts, ppq, tempo) defines musical time on the
     * device until superseded. ppq travels as bit-cast u64 (the ring's
     * fields are fixed); flags per spec key 0. */
    void queueTransport(uint32_t flags, double tempo, double ppq, uint64_t ts);
    /* §9.7 synthesis shared by every shell (VST3, AU): feed the host's
     * per-block transport snapshot; emits an event on change (play/stop,
     * tempo, position discontinuity = locate/loop wrap) plus the >= 1 Hz
     * refresh. Audio-thread safe: detection state is audio-thread-owned,
     * emission is a lock-free ring push. `base` = this block's start in
     * the stream domain (streamPos + latency). */
    void feedTransport(bool playing, bool tempoValid, double tempo, bool posValid,
                       double ppq, uint32_t blockSamples, uint64_t base);
    /* SSI of the next sample pullAudio will deliver: the stream-domain "now"
     * for timestamping. Events for DAW offset s in the current block go to
     * streamPos() + s + latencySamples() — PDC makes that land on time. */
    uint64_t streamPos() const { return ssiRead_.load(std::memory_order_relaxed); }
    /* Fill n interleaved-stereo samples; pads with silence on underrun
     * (counted). Returns samples padded (0 = clean). */
    size_t pullAudio(float *interleavedLR, size_t nFrames);
    /* Offline-mode variant (§8.3 / VST3 kOffline): block until the device
     * has rendered the requested range — offline bounce through hardware
     * may legitimately wait for the wire. */
    size_t pullAudioBlocking(float *interleavedLR, size_t nFrames, unsigned timeoutMs);

    /* Reported latency = ring target + EVENT HEADROOM of one block — and
     * "block" means whichever is larger, the DAW's or the 256-sample
     * pacing block. Event timestamps are streamPos + offset + latency and
     * must stay ahead of the pacing frontier, which overshoots the ring
     * target by up to one PACING block; with only a 64-sample DAW block
     * of headroom, a fraction of timestamps land in already-paced
     * territory at queue time — no wire ordering can save an event whose
     * covering frame already went out (measured at block 64: evt_late
     * ~100 per 45 s flood; blocks >= 256 unaffected). */
    uint32_t latencySamples() const { return targetFrames_ + eventHeadroom(); }
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

    /* ---- state (main thread; blocking, not RT-safe) ---- */
    /* Pull (§11.4): snapshot-if-dirty, fetch closure, serialize a Recall
     * Bundle (§15.3). False if not connected and nothing staged. */
    bool getStateBundle(std::vector<uint8_t> &out);
    /* Parse a bundle; stage it; if connected, reconcile now (§12.2).
     * v0 policy deviation: mismatch auto-resolves by Push-with-archive
     * (spec wants a user choice; the shell has no UI yet — the archive
     * step keeps it loss-free). */
    bool setStateBundle(const uint8_t *data, size_t len);
    /* Knob values from the project's bundle, for controller display. */
    bool bundleParam(uint32_t id, float &value);

    std::string serial() const { return serial_; }

#ifdef __APPLE__
    /* CoreAudio workgroup handoff (AU shells; VST3 has no API for it):
     * the host's render-context observer calls this with its workgroup;
     * the reader/pump/feeder threads join so the USB path is scheduled
     * WITH the audio graph. nullptr = leave. */
    void setWorkgroup(os_workgroup_t wg);
#endif

private:
    HarpRuntime();
    ~HarpRuntime();
    HarpRuntime(const HarpRuntime &) = delete;

    static constexpr uint32_t kBlock = 256; /* pacing block, samples */

    /* Event headroom: one block, whichever flavor is larger. Together with
     * the feeder's frontier cap (cap = read + target + headroom − dawBlock)
     * this gives the timing INVARIANT: the earliest event timestamp a
     * block can produce (offset 0 -> read + target + headroom) clears the
     * pacing frontier by at least one DAW block, for every block size —
     * so no event is ever born into already-paced territory. (At DAW
     * block 64 the soak's note period divides evenly into blocks: every
     * note hit offset 0 and raced the cap edge — ~30% applied late until
     * the strict margin closed it.) */
    uint32_t eventHeadroom() const {
        return maxDawBlock_ > kBlock ? maxDawBlock_ : kBlock;
    }
    /* Ring cushion vs host-side jitter. The sync-libusb transport needed
     * 5 blocks (26.7 ms) against its 20-25 ms completion tails; the async
     * transport (dedicated event thread, always-pending transfers) killed
     * the tails, and 2 blocks passed the full flood matrix at every DAW
     * block size 64-1024 with only the startup underrun. Reported latency
     * derives from this: 16 ms total at DAW blocks <= 256 (48 k).
     * HARP_CUSHION_BLOCKS overrides for measurement. */
    static constexpr uint32_t kTargetDepthFrames = 2;

#ifdef __APPLE__
    /* per-thread workgroup membership; loops call wgMaintain() each pass
     * (a relaxed generation check) and join/leave on their OWN stack —
     * os_workgroup_join must run on the joining thread */
    struct WgState {
        uint64_t gen = 0;
        os_workgroup_t joined = nullptr;
        os_workgroup_join_token_s token{};
    };
    void wgMaintain(WgState &st);
    std::mutex wgMutex_;
    os_workgroup_t wg_ = nullptr; /* retained; under wgMutex_ */
    std::atomic<uint64_t> wgGen_{0};
#endif

    void supervisor(); /* owns session lifecycle: connect, run, reconnect */
    bool sessionUp();  /* one attempt: open, hello, re-push bundle, stream */
    void sessionDown(); /* reap reader+pump, orderly stop if alive, close usb */
    void feeder();      /* runs on the supervisor thread while connected */
    void reader();
    void eventPump();   /* dedicated event->wire thread: an event's deadline
                           budget is ~one DAW block (5.3 ms at 256), while a
                           pacing write can stall 8 ms in drain-on-stall —
                           events must never wait behind audio head-of-line */
    void settlePadDebt(); /* drop late arrivals for already-padded SSIs */
    bool helloAndIdentity();
    bool audioStart(uint32_t rate);
    void audioStopLocked();
    /* encode one event message into `out` (no I/O) — the feeder batches
     * many messages into a single framed bulk write per cycle */
    static void encodeParamEvent(harp_cbuf *out, uint32_t id, float v, uint64_t ts);
    static void encodeRampEvent(harp_cbuf *out, uint32_t id, float target,
                                uint64_t start, uint64_t end);
    static void encodeUmpEvent(harp_cbuf *out, uint32_t word, uint64_t ts);
    static void encodeTransportEvent(harp_cbuf *out, uint32_t flags, double tempo,
                                     double ppq, uint64_t ts);
    void pollEcho(); /* drain incoming evt stream */

    /* control-plane ops under ctlMutex_ (protocol work lives in the shared
     * host client — host/client.h; these wrap it with shell policy/logging) */
    bool request(harp_cbuf *req, harp_cbuf *rsp, harp_env *e);
    bool refsLocked(harp_ref *live);
    bool snapshotLocked(harp_hash *out);
    bool fetchClosureLocked(const harp_hash &root);
    bool pushStateLocked(const harp_hash &target);

    harp_io *io_ = nullptr;
    harp_link link_;   /* rx reassembly: shared by client_ and pollEcho */
    harp_cbuf msg_;    /* pollEcho rx scratch */
    harp_client client_{};
    std::mutex ctlMutex_;

    harp_store store_;
    bool storeOk_ = false;

    std::thread supervisorThread_; /* runs supervisor(): feeder + reconnect */
    std::thread readerThread_; /* always-pending audio-IN read: the device's
                                  response writes must never wait for us;
                                  spawned per session by sessionUp() */
    std::thread eventPumpThread_; /* per session, like the reader */
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> padSamples_{0}; /* total silence padded — severity, not count */
    std::atomic<uint64_t> evDrops_{0};    /* events lost to ring overflow — never silent */
    uint64_t evDropsLogged_ = 0;
    std::atomic<bool> panicPending_{false}; /* a note-off was lost: all-off NOW */

    /* §9.7 transport change detection (audio-thread-owned) */
    bool tpLastPlaying_ = false;
    bool tpSent_ = false;
    double tpLastTempo_ = 0, tpLastEndPpq_ = 0;
    uint64_t tpSamplesSince_ = 0;

    /* event fence sequence (§8.3.1): count of events QUEUED this session
     * (not yet written — queue time is what a racing pacing frame must
     * respect). Audio thread increments; feeder stamps it into fenced
     * pacing frames; the device renders a range only after consuming that
     * many evt messages. Resets with the session, like the SSI domain. */
    std::atomic<uint32_t> evtQueuedSeq_{0};

    FloatRing audioRing_{1 << 15}; /* 32768 floats = 16384 stereo frames */
    std::atomic<uint64_t> framesRecvAtomic_{0}; /* written by reader, read by feeder */
    TimedRing timedRing_; /* outbound: params, ramps, notes — order preserved */
    ParamRing echoRing_;  /* device front-panel echoes -> outputParameterChanges */
    std::atomic<uint64_t> ssiRead_{0};

public:
    /* audio thread: drain echoed device-side edits (§9.4 echo) */
    bool popEcho(uint32_t &id, float &v) {
        ParamChange c;
        if (!echoRing_.pop(c)) return false;
        id = c.id;
        v = c.value;
        return true;
    }

private:
    uint64_t ssi_ = 0;
    uint64_t framesSent_ = 0, framesRecv_ = 0;
    uint64_t ahead_ = 2; /* fixed small pipeline; reader thread keeps RTT short */
    uint32_t targetFrames_ = kTargetDepthFrames * kBlock; /* see configure() */
    uint32_t maxDawBlock_ = 1024;                          /* event headroom */
    size_t padDebtFloats_ = 0; /* late arrivals owed to already-padded SSIs;
                                  audio thread only */

    /* identity (for the bundle's identity-expectation) */
    std::string serial_, vendorName_, productName_, engineId_, engineVer_;
    uint32_t vendorId_ = 0, productId_ = 0;

    /* USB-descriptor identity of the BOUND device (vid:pid = "same model",
     * serial = this unit). Captured at claim, recorded in the bundle,
     * used for selection — distinct from the hello/CBOR identity above,
     * which lives in a different id namespace. */
    uint16_t usbVid_ = 0, usbPid_ = 0;
    std::string usbSerial_;
    std::string boundSerial_; /* once bound, reconnect targets this exactly (Step 5) */
    harp_hash paramMapHash_{};

    /* the project's recall bundle: PERSISTENT, not consumed — the DAW
     * project's notion of state re-asserts on every (re)connect ("Live
     * wins", archive-before-push keeps it loss-free) */
    std::mutex bundleMutex_;
    bool hasBundle_ = false;
    harp_hash bundleTarget_{};
    std::vector<std::pair<uint32_t, float>> bundleParams_;
    /* the device the loaded bundle wants (USB identity, key 5). Empty
     * serial = no usb-identity in the bundle (fresh or pre-schema). */
    bool wantUsb_ = false;
    uint16_t wantUsbVid_ = 0, wantUsbPid_ = 0;
    std::string wantUsbSerial_;

    uint32_t rate_ = 48000;
};
