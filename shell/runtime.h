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
#include "usb_io.h"
}

class HarpRuntime {
public:
    static HarpRuntime &instance();

    /* Claim the device, hello, start the host-paced stream. False if no
     * device (the shell then renders silence and may retry). */
    bool start(uint32_t sampleRate);
    void stop();
    bool connected() const { return connected_.load(std::memory_order_acquire); }

    /* ---- audio thread ---- */
    void setParam(uint32_t id, float normalized); /* enqueue, lock-free */
    /* Fill n interleaved-stereo samples; pads with silence on underrun
     * (counted). Returns samples padded (0 = clean). */
    size_t pullAudio(float *interleavedLR, size_t nFrames);
    /* Offline-mode variant (§8.3 / VST3 kOffline): block until the device
     * has rendered the requested range — offline bounce through hardware
     * may legitimately wait for the wire. */
    size_t pullAudioBlocking(float *interleavedLR, size_t nFrames, unsigned timeoutMs);

    uint32_t latencySamples() const { return kTargetDepthFrames * kBlock; }
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

    static constexpr uint32_t kBlock = 256;        /* pacing block, samples */
    static constexpr uint32_t kTargetDepthFrames = 3; /* latency = 3 blocks */

    void feeder();
    bool helloAndIdentity();
    bool audioStart(uint32_t rate);
    void audioStopLocked();
    bool pushKnob(uint32_t id, float v);

    /* control-plane request/response under ctlMutex_ */
    bool request(harp_cbuf *req, harp_cbuf *rsp, harp_env *e);
    bool refsLocked(harp_ref *live);
    bool snapshotLocked(harp_hash *out);
    bool fetchClosureLocked(const harp_hash &root);
    bool pushStateLocked(const harp_hash &target);

    harp_io *io_ = nullptr;
    harp_link link_;
    harp_cbuf msg_;
    uint64_t nextRid_ = 0;
    std::mutex ctlMutex_;

    harp_store store_;
    bool storeOk_ = false;

    std::thread feederThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> underruns_{0};

    FloatRing audioRing_{1 << 15}; /* 32768 floats = 16384 stereo frames */
    ParamRing paramRing_;
    uint64_t ssi_ = 0;
    uint64_t framesSent_ = 0, framesRecv_ = 0;
    uint64_t ahead_ = 4; /* adaptive in-flight cap (learned, §8.3 note) */

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
