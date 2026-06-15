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

/* §9.4 multitimbral event MERGE (P5). One physical HARP unit is one session
 * with one event plane; several shells can share that session (P4 registry),
 * each OWNING a part (channel). Each such shell registers its OWN event SOURCE
 * — a TimedRing + a channel — and the eventPump drains ALL registered sources
 * onto the one wire, stamping every event with its source's channel. So a
 * multitimbral group of aliases actually PLAYS: shell A injects notes/params on
 * part 0, shell B on part 1, …, all merged onto the device.
 *
 * Each source is strictly SPSC: ONE producer (the owning instance's audio /
 * control thread calling queue*) and ONE consumer (the eventPump). The merge
 * adds no multi-producer ring — the pump just iterates a SET of SPSC rings.
 * The only shared mutable structure is the source REGISTRY (sources added on
 * acquire, removed on release); it is synchronised against the pump's iteration
 * by a short mutex that the audio thread never touches (acquire/release are off
 * the audio path).
 *
 * SINGLE-INSTANCE is unchanged: the owner's source is the runtime's built-in
 * source (ownerSource_), always present, always slot 0. With one instance the
 * pump drains exactly that one ring with that one channel — byte-identical to
 * the pre-P5 timedRing_ + chan_ path (the golden gate).
 *
 * SCOPE: P5 merges EVENTS only. Per-part AUDIO demux (each alias hearing only
 * its own part) is OUT OF SCOPE — the owner still pulls the MAIN MIX (which
 * sums all parts), and attached/sibling instances stay audio-SILENT. That is
 * tracked as the follow-up P5b. */
struct EventSource {
    TimedRing ring;            /* this instance's outbound events (SPSC) */
    std::atomic<uint8_t> chan; /* the part (§9.4 key 5) every event carries */
    explicit EventSource(uint8_t channel = 0) : chan(channel & 0xf) {}
};

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
    /* Called from setupProcessing: the ring cushion must scale with the
     * DAW's block size (a 1024-sample pull through a 1280-sample cushion
     * leaves 5 ms of headroom — stutter at large buffers). */
    void configure(uint32_t sampleRate, uint32_t maxDawBlock) {
        rate_ = sampleRate;
        maxDawBlock_ = maxDawBlock;
        targetFrames_ = targetFramesFor(maxDawBlock);
    }

    /* The reported latency (= ring target + event headroom) is a PURE function
     * of the DAW block size — independent of any session. A shell can report it
     * before a runtime is configured (P4: the runtime now comes from the
     * registry at activate-time, so a host querying latency in the inactive
     * window has no live runtime to ask). Same constants/env as configure() +
     * latencySamples(), so the value is byte-identical to a configured
     * runtime's. */
    static uint32_t latencyFor(uint32_t maxDawBlock) {
        uint32_t headroom = maxDawBlock > kBlock ? maxDawBlock : kBlock;
        return targetFramesFor(maxDawBlock) + headroom;
    }

    /* Begin supervising: claim the device, hello, start the host-paced
     * stream — and keep a supervisor thread retrying/reconnecting for as
     * long as the plugin is active (unplug -> silence + retry; replug ->
     * session re-established and the project's bundle re-asserted).
     * Returns whether a device is connected RIGHT NOW. */
    bool start(uint32_t sampleRate);
    void stop();
    bool connected() const { return connected_.load(std::memory_order_acquire); }

    /* Which device output slots audio.start subscribes to (§9.x active-slots-out,
     * key 4). DEFAULT {0,1} = the stereo MAIN MIX — the exact render the device
     * has always produced, so the default wire is byte-identical to before this
     * setter existed (golden gate). A test/host harness can request a single
     * PART's stereo pair instead — part N occupies slots {2+2N, 3+2N} — to pull
     * that part in isolation (P2.2). Must be called before start()/audioStart();
     * a no-op once a session is up (the subscription is fixed at audio.start). */
    void setOutSlots(const std::vector<uint32_t> &slots) {
        if (!slots.empty()) outSlots_ = slots;
    }

    /* The multitimbral part (§9.4, key 5) the OWNER instance drives: notes
     * already carry their channel in the UMP word (the shell stamps it per-
     * event), but parameter sets/ramps had no channel — so on a multi-part
     * device a host's knob edits all hit part 0. setChannel() pins the part
     * the OWNER source drives, so its param events carry the SAME channel as
     * its notes and one shell fully owns its part. DEFAULT 0 => encode omits
     * the key => byte-identical wire (golden gate). Set before start() (the
     * event pump reads it per event); a no-op mid-session is harmless since
     * the pump re-reads it per event. ATTACHED instances set their part on
     * their OWN source via the channel passed to registerSource(). */
    void setChannel(uint8_t channel) {
        ownerSource_.chan.store(channel & 0xf, std::memory_order_relaxed);
    }
    uint8_t channel() const { return ownerSource_.chan.load(std::memory_order_relaxed); }

    /* ---- per-instance event source (P5 merge) ----
     * The OWNER drives ownerSource(); an ATTACHED instance registers its OWN
     * source (its part) on acquire and removes it on release. queue* take the
     * source so each instance pushes to ITS ring (the SPSC producer side) —
     * the eventPump drains every registered source onto the one wire. */
    EventSource *ownerSource() { return &ownerSource_; }
    /* Register an attached instance's source for `channel` (its part). The
     * eventPump begins draining it on its next pass; safe to call mid-session
     * (acquire is off the audio path). Returns the source to queue* against, or
     * nullptr if the device's parts are all taken (kMaxSources sources already
     * registered) — that instance is then EVENT-DORMANT: it passes the nullptr
     * to queue*, which drops its events rather than racing a 17th producer onto
     * the owner's SPSC ring (see queue*). */
    EventSource *registerSource(uint8_t channel);
    /* Remove an attached source on release and free it. The runtime owns the
     * sources it allocated in registerSource; the producer must be quiescent
     * (the host stops process() before release). unregisterSource removes the
     * source from the registry FIRST (under the lock, so the eventPump never
     * touches it again), THEN drains its leftover queued-but-unwritten events —
     * dropping them (the part is gone) but DECREMENTING the event fence by that
     * count, so the device's consume target stays consistent and surviving
     * parts keep tight timing (see the impl). Idempotent on nullptr / the owner
     * source (which persists for the whole session). */
    void unregisterSource(EventSource *src);

    /* ---- audio thread (all lock-free) ---- */
    /* Each takes the calling instance's EventSource (its SPSC producer side).
     * Single-instance passes ownerSource(); the merge is the only difference
     * from the pre-P5 single-ring path. */
    void queueParamSet(EventSource *src, uint32_t id, float v, uint64_t ts);
    void queueRamp(EventSource *src, uint32_t id, float target, uint64_t start,
                   uint64_t end);
    void queueNote(EventSource *src, uint32_t umpWord, uint64_t ts);
    /* §9.7 transport anchor: (ts, ppq, tempo) defines musical time on the
     * device until superseded. ppq travels as bit-cast u64 (the ring's
     * fields are fixed); flags per spec key 0. Transport is GLOBAL (no part):
     * it is always queued on the OWNER source regardless of `src`, so a group
     * of aliases emits ONE transport stream, not N duplicates (the owner's is
     * canonical). */
    void queueTransport(EventSource *src, uint32_t flags, double tempo, double ppq,
                        uint64_t ts);
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
    static void paramsFromStore(harp_store *store, const harp_hash &target,
                                std::vector<std::pair<uint32_t, float>> &out);

    std::string serial() const { return serial_; }

#ifdef __APPLE__
    /* CoreAudio workgroup handoff (AU shells; VST3 has no API for it):
     * the host's render-context observer calls this with its workgroup;
     * the reader/pump/feeder threads join so the USB path is scheduled
     * WITH the audio graph. nullptr = leave. */
    void setWorkgroup(os_workgroup_t wg);
#endif

    /* One runtime per plugin instance (the singleton is gone — two
     * instances must own two devices). */
    HarpRuntime();
    ~HarpRuntime();
    HarpRuntime(const HarpRuntime &) = delete;
    HarpRuntime &operator=(const HarpRuntime &) = delete;

    /* Runtime-free recall-bundle param extraction, for hosts/controllers
     * that must display knob values without opening a device (the VST3
     * Controller). Ingests the bundle's embedded objects into `store`. */
    static bool bundleParams(const uint8_t *data, size_t len, harp_store *store,
                             std::vector<std::pair<uint32_t, float>> &out);
    /* the shared host object-cache dir (the controller opens it too). */
    static void defaultStoreDir(char *out, size_t n);

    /* Runtime-free extraction of the bundle's WANTED usb serial (§15.3 key 5
     * -> usb-identity -> serial). Empty if the bundle has no usb-identity
     * (fresh / pre-schema) or doesn't parse. The runtime registry (P4) uses
     * this to learn a project's target unit BEFORE any runtime exists, so a
     * shell that loaded a bundle pinning a serial can share that unit's runtime
     * with a sibling that pins the same serial. Selection is unchanged — this
     * only surfaces the serial the bundle already carries (the same one
     * setStateBundle would later record in wantUsbSerial_). */
    static std::string bundleWantedSerial(const uint8_t *data, size_t len);

private:

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

    /* Ring target depth in frames for a given DAW block — the single source
     * of the cushion math (incl. the HARP_CUSHION_BLOCKS override) shared by
     * configure() and the static latencyFor(). */
    static uint32_t targetFramesFor(uint32_t maxDawBlock) {
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
        return needed > floor_ ? needed : floor_;
    }

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
    /* multi-device selection: first bind picks by the project's wanted
     * identity (exact serial -> same model -> fresh-any); once bound,
     * reconnect targets that exact unit only. Claimed transport or null. */
    harp_io *selectDevice();
    void sessionDown(); /* reap reader+pump, orderly stop if alive, close usb */
    void feeder();      /* runs on the supervisor thread while connected */
    void reader();
    void eventPump();   /* dedicated event->wire thread: an event's deadline
                           budget is ~one DAW block (5.3 ms at 256), while a
                           pacing write can stall 8 ms in drain-on-stall —
                           events must never wait behind audio head-of-line.
                           P5: drains the SET of registered sources, stamping
                           each event with its source's channel. */
    /* drain one source's ring into `batch` as framed EVT messages (up to
     * `budget` events), returning how many were drained. SPSC consumer side
     * of that source — only the pump calls it. Encodes params/ramps with the
     * source's channel; notes carry their own channel in the UMP word;
     * transport (owner source only) is global. */
    int drainSource(EventSource &src, harp_cbuf &batch, harp_cbuf &msgbuf, int budget);
    void settlePadDebt(); /* drop late arrivals for already-padded SSIs */
    bool helloAndIdentity();
    bool audioStart(uint32_t rate);
    void audioStopLocked();
    /* encode one event message into `out` (no I/O) — the feeder batches
     * many messages into a single framed bulk write per cycle */
    /* channel = multitimbral part (§9.4, key 5); 0 omits the key so the
     * single-part wire is byte-identical. The host sends 0 until per-shell
     * channel routing lands (multitimbral group). */
    static void encodeParamEvent(harp_cbuf *out, uint32_t id, float v, uint64_t ts,
                                 uint8_t channel = 0);
    static void encodeRampEvent(harp_cbuf *out, uint32_t id, float target,
                                uint64_t start, uint64_t end, uint8_t channel = 0);
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

    void *usbCtx_ = nullptr; /* one libusb context for the whole plugin life:
                                created in start(), reused across every connect
                                attempt (no per-retry init/exit churn), and
                                destroyed in stop() after the supervisor joins,
                                before the DLL can unload */
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
    /* A 17th alias's registerSource() returned nullptr (table full at
     * kMaxSources parts): that instance is EVENT-DORMANT — queue* drops its
     * events rather than racing them onto the owner's SPSC ring. The audio
     * thread only raises the flag (RT-safe, no syscall); the eventPump emits
     * the one-shot log. */
    std::atomic<bool> dormantSrcSeen_{false};   /* set by queue* on a null source */
    std::atomic<bool> dormantSrcLogged_{false}; /* the pump logged it once */
    /* event-dormant (null-source) drop: raise the flag the pump logs. Audio
     * thread only — no allocation, no I/O. */
    void noteDormant() { dormantSrcSeen_.store(true, std::memory_order_relaxed); }

    /* §9.7 transport change detection (audio-thread-owned) */
    bool tpLastPlaying_ = false;
    bool tpSent_ = false;
    double tpLastTempo_ = 0, tpLastEndPpq_ = 0;
    uint64_t tpSamplesSince_ = 0;

    /* event fence sequence (§8.3.1): count of events QUEUED this session
     * (not yet written — queue time is what a racing pacing frame must
     * respect). Audio thread increments (queue*); the feeder stamps it into
     * fenced pacing frames; the device renders a range only after consuming
     * that many evt messages. unregisterSource decrements it by a released
     * source's leftover (queued-but-unwritten) events, so the high-water mark
     * stays equal to what the device will actually consume. Resets with the
     * session, like the SSI domain. */
    std::atomic<uint32_t> evtQueuedSeq_{0};

    FloatRing audioRing_{1 << 15}; /* 32768 floats = 16384 stereo frames */
    std::atomic<uint64_t> framesRecvAtomic_{0}; /* written by reader, read by feeder */

    /* P5 event-source MERGE (see EventSource doc above). The OWNER source is
     * built in and always present — it IS the pre-P5 timedRing_ + chan_, so a
     * single instance drains exactly this one ring with this one channel
     * (byte-identical golden path). Attached instances register additional
     * sources; the eventPump iterates the whole set.
     *
     * The registry is a fixed array of source pointers guarded by a short
     * mutex (sourcesMutex_). The eventPump holds that mutex while it DRAINS
     * every registered source's ring INTO its batch buffer (pure memory work —
     * pop + encode, no I/O), and writes the wire only AFTER unlocking; it never
     * holds the lock across the wire write. registerSource/unregisterSource take
     * the same mutex to add/remove a source — both off the RT audio path. The
     * audio thread's queue* never touch the registry at all (the producer
     * already holds its own source pointer). So no source ring ever becomes
     * multi-producer, and the only cross-thread sharing — the pointer array —
     * is mutated only off the RT path under that mutex.
     *
     * SAFE-FREE + FENCE: unregisterSource removes a source from the array
     * (under the lock) before draining/freeing it, so a pump pass either drains
     * it fully before it disappears or never sees it — it can never pop a freed
     * ring. The same drain also decrements evtQueuedSeq_ by the source's
     * leftover events, keeping the fence consistent for surviving parts (see
     * unregisterSource). */
    EventSource ownerSource_{0};
    static constexpr size_t kMaxSources = 16; /* one per multitimbral part */
    std::mutex sourcesMutex_;
    EventSource *sources_[kMaxSources] = {&ownerSource_};
    size_t nSources_ = 1; /* owner source occupies slot 0 for the session */

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

    /* audio.start key 4 = active-slots-out. {0,1} = the stereo main mix —
     * the historical default (the wire used to send an empty [] here; sending
     * the explicit {0,1} drives the same main-mix render byte-identically).
     * setOutSlots() overrides it before start() to pull a single part. */
    std::vector<uint32_t> outSlots_{0, 1};

    /* (the OWNER instance's part lives in ownerSource_.chan — see above; an
     * ATTACHED instance's part lives in the source it got from registerSource.
     * Notes don't read it — their channel is already baked into the UMP word
     * by the shell; only param sets/ramps need the per-source channel.) */
};
