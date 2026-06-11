/* HarpRuntime — the embedded host runtime (§15.1, embedded-in-process for
 * milestone 2; the per-machine daemon split comes later).
 *
 * Owns the USB transport, the control link, and the host-paced audio pump.
 * Threading contract:
 *   - audio thread:   pullAudio(), setParam()        (lock-free, never block)
 *   - feeder thread:  USB I/O — pacing, draining, param pushes
 *   - main/UI thread: start(), stop(), getStateBundle(), setStateBundle()
 *     (control-plane requests serialized with the feeder via ctlMutex_)
 */
#pragma once

#include <atomic>
#include <cstdint>
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
        uint32_t floor_ = kTargetDepthFrames * kBlock;
        targetFrames_ = needed > floor_ ? needed : floor_;
    }

    /* Claim the device, hello, start the host-paced stream. False if no
     * device (the shell then renders silence and may retry). */
    bool start(uint32_t sampleRate);
    void stop();
    bool connected() const { return connected_.load(std::memory_order_acquire); }

    /* ---- audio thread (all lock-free) ---- */
    void queueParamSet(uint32_t id, float v, uint64_t ts);
    void queueRamp(uint32_t id, float target, uint64_t start, uint64_t end);
    void queueNote(uint32_t umpWord, uint64_t ts);
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

    /* Reported latency = ring target + one DAW block of EVENT HEADROOM:
     * event timestamps are streamPos + offset + latency, and they must
     * beat the pacing of their own SSIs to the wire — with latency equal
     * to the ring target exactly, intra-block offsets could land in
     * already-paced territory and apply a block late (audible slop). */
    uint32_t latencySamples() const { return targetFrames_ + maxDawBlock_; }
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
    /* Knob values from the staged/last bundle, for controller display. */
    bool stagedParam(uint32_t id, float &value);

    std::string serial() const { return serial_; }

private:
    HarpRuntime();
    ~HarpRuntime();
    HarpRuntime(const HarpRuntime &) = delete;

    static constexpr uint32_t kBlock = 256; /* pacing block, samples */
    /* Ring cushion vs host-side completion-tail jitter. Sync libusb at
     * user priority shows occasional 20-25 ms read tails; 5 blocks
     * (26.7 ms at 48 k) absorbs them. Going lower wants async transfers +
     * CoreAudio workgroup integration (future). Reported latency derives
     * from this. */
    static constexpr uint32_t kTargetDepthFrames = 5;

    void feeder();
    void reader();
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

    std::thread feederThread_;
    std::thread readerThread_; /* always-pending audio-IN read: the device's
                                  response writes must never wait for us */
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> padSamples_{0}; /* total silence padded — severity, not count */
    std::atomic<uint64_t> evDrops_{0};    /* events lost to ring overflow — never silent */
    uint64_t evDropsLogged_ = 0;
    std::atomic<bool> panicPending_{false}; /* a note-off was lost: all-off NOW */

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
    harp_hash paramMapHash_{};

    /* staged recall state (from setState before/without a device) */
    std::mutex stagedMutex_;
    bool hasStaged_ = false;
    harp_hash stagedTarget_{};
    std::vector<std::pair<uint32_t, float>> stagedParams_;

    uint32_t rate_ = 48000;
};
