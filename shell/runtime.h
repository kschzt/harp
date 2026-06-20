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
 * SCOPE: this struct is the EVENT plane merge. The per-part AUDIO demux (each
 * alias hearing only its own part) is implemented separately by the AudioSink
 * registry below (P5b) — the owner's reader() splits the device frame into
 * per-part sink rings; an instance with no sink still pulls the summed MAIN MIX,
 * the byte-identical default. */
struct EventSource {
    TimedRing ring;            /* this instance's outbound events (SPSC) */
    std::atomic<uint8_t> chan; /* the part (§9.4 key 5) every event carries */
    explicit EventSource(uint8_t channel = 0) : chan(channel & 0xf) {}
};

/* §8.2 per-part AUDIO demux (P5b). The device streams ONE frame per pacing
 * range carrying the UNION of every instance's requested output slots
 * (audio.start key 4), interleaved by slot. Each ATTACHED instance that wants
 * its OWN part's audio (not the summed main mix) registers an AudioSink keyed
 * to the slots it requested; reader() splits each device frame, copying THIS
 * sink's slot columns out of the union into ITS ring. pullAudio(sink) then
 * drains that ring — so every instance hears exactly the slot(s) it subscribed
 * to, demuxed from the one shared stream.
 *
 * Each sink is strictly SPSC: ONE producer (reader(), demuxing each frame) and
 * ONE consumer (that instance's pullAudio). The reader is the sole writer of
 * every sink (it iterates the SET); the only shared mutable structure is the
 * sink REGISTRY (added on acquire, removed on release), synchronised against the
 * reader's iteration by sinksMutex_ — which the audio thread never touches.
 *
 * SINGLE-INSTANCE / DEFAULT MAIN MIX is unchanged: the owner's main-mix audio
 * is the runtime's built-in audioRing_ (NOT an AudioSink), and pullAudio()/
 * pullAudioBlocking() with no sink drain it exactly as pre-P5b — byte-identical
 * (the golden gate). The demux loop touches audioRing_ only when the union is
 * the bare {0,1} owner default, in which case the column copy IS the same
 * contiguous nsamples*2 write the pre-P5b reader did (see reader()).
 *
 * `cols` are the COLUMN INDICES of this sink's slots WITHIN the union frame
 * (the order slots appear in the audio.start request), resolved at audioStart.
 * pullAudio always delivers a stereo interleaved pair: a 2-slot sink is its two
 * columns interleaved; a 1-slot sink resolves cols[1]=cols[0] so the same demux
 * write duplicates L=R. */
struct AudioSink {
    FloatRing ring{1 << 15}; /* 32768 floats = 16384 stereo frames, like audioRing_ */
    uint16_t cols[2] = {0, 1}; /* union-frame column indices this sink reads */
    /* the slots this instance requested (P2.2 part pair {2+2k,3+2k}); resolved
     * to `cols` against the union at audioStart. Kept so a re-negotiation could
     * recompute columns; held only off the audio path. */
    std::vector<uint32_t> slots;
    /* pad-debt bookkeeping, exactly the per-sink analogue of padDebtFloats_:
     * a padded SSI is spent, and late arrivals for it are dropped (audio thread
     * only — this sink's pullAudio is its sole toucher). */
    size_t padDebt = 0;
    /* STREAM EPOCH — bumped (off the audio path, under sinksMutex_) by audioStart
     * each time this sink's slots are present in the union it just (re)negotiated.
     * A LATE sink registered mid-session pulls silence (accruing padDebt) for the
     * whole gap before the re-neg streams its slots; without correction
     * settleSinkPadDebt would then DROP the first real post-re-neg samples to pay
     * that bogus debt, leaving the sink silent (the B3 1-in-8 failure). On its
     * first pull after the epoch advances, pullAudio (the SOLE consumer, so no
     * race) ZEROES padDebt and clear()s the stale ring — consumer-side ops, safe
     * against the reader's producer writes — so real audio plays from the first
     * demuxed frame. epochSeen is consumer-private (pullAudio only). */
    std::atomic<uint32_t> epoch{0};
    uint32_t epochSeen = 0;
    explicit AudioSink(const std::vector<uint32_t> &s) : slots(s) {}
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

#include "transport.h" /* ShellTransport: the USB/Ethernet binding behind the runtime */

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
    /* §8.3-over-§8.7: the shell calls this from its offline-render hook (VST3
     * processMode==kOffline, CLAP CLAP_RENDER_OFFLINE, AU OfflineRender). On an
     * Ethernet binding it selects HOST-PACED (deterministic offline bounce over TCP)
     * vs free-running RTP; on USB it's a no-op (always host-paced). Before the session
     * starts, selectDevice reads it at the first dial. On a LIVE session (AU/CLAP can
     * flip OfflineRender / render-mode while active), a genuine mode change RE-DIALS
     * the Ethernet session in the new mode and BLOCKS (bounded, host-thread only) until
     * it is up — so the next offline pull is deterministic, not stale free-running. */
    void setOffline(bool o);
    /* Count of COMPLETED P5b audio re-negotiations this runtime has performed
     * (a late/removed sink changed the union -> the feeder re-streamed it). 0 on
     * the single-instance / owner-only / no-sink path, which never re-negotiates.
     * A test harness polls this to start a late-sink capture only AFTER the
     * re-neg has actually streamed the wider union (so the proof is deterministic,
     * not racing the re-neg). Incremented only on a SUCCESSFUL audio.start. */
    uint32_t renegCount() const { return renegCount_.load(std::memory_order_acquire); }

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

    /* §14.3 host LoopbackMeasurer (the digital round-trip probe). Arm the host
     * side: `in` is the H->D slot the host injects a stimulus on, `out` is the
     * D->H slot the device echoes it to (the device copies in->out in the same
     * rendered frame — see device/engine.c §14.3). Must be set BEFORE start()/
     * audioStart(): when armed, audio.start declares the in-slot in key 3 so the
     * device parses it into d->audio.in_slots[], and measureLoopback() can later
     * inject + locate the echo. DEFAULT off (in==out==-1) — audio.start sends the
     * unchanged key-3 array and the feeder NEVER populates an H->D payload, so the
     * render is byte-identical to the golden path (the §14.3 atomic-gating gate).
     * The out-slot MUST be one the synth is NOT driving with notes, else the echo
     * overwrites real synth output (the device overwrites, it does not mix). */
    void setLoopbackSlots(int in, int out) {
        loopbackIn_ = in;
        loopbackOut_ = out;
    }
    bool loopbackArmed() const { return loopbackIn_ >= 0 && loopbackOut_ >= 0; }

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
    /* §9.4 non-destructive modulation (etype 6): an additive signed offset on
     * one param's base value. `voice` is the §9.5 packed key of the target note
     * ((channel<<8)|note for MIDI-1.0 note-ons, exactly what the device assigns
     * at note-on); 0 means part-wide (every active voice). The part (key 5) is
     * derived from the voice key, so a mod follows its note's channel even when
     * one instance drives several. Maps a host's per-note expression (e.g. VST3
     * Brightness -> Filter Cutoff) to one voice without touching base/recall. */
    void queueMod(EventSource *src, uint32_t id, float offset, uint32_t voice,
                  uint64_t ts);
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
    /* ---- per-instance audio sink (P5b per-part demux) ----
     * The OWNER pulls the main mix via the built-in audioRing_ (the no-sink
     * pullAudio below). An ATTACHED instance that wants its OWN part's audio
     * registers a sink for the slots it requested (its P2.2 part pair) and
     * pulls THAT sink — reader() demuxes the device's union frame into it.
     * Register BEFORE start() so the slots enter the audio.start union (an
     * instance whose slots aren't in the live union gets silence until the next
     * audio.start — the P5b mid-attach limitation, documented in audioStart). */
    AudioSink *registerAudioSink(const std::vector<uint32_t> &slots);
    /* Remove + free a sink on release. Like unregisterSource: take sinksMutex_,
     * remove from the registry FIRST (so reader() can't touch it again), then
     * free it. The producer (reader, if alive) iterates only the registry under
     * the lock, so the removed sink is ours alone to free. Idempotent on null. */
    void unregisterAudioSink(AudioSink *sink);

    /* SSI of the next sample pullAudio will deliver: the stream-domain "now"
     * for timestamping. Events for DAW offset s in the current block go to
     * streamPos() + s + latencySamples() — PDC makes that land on time. */
    uint64_t streamPos() const { return ssiRead_.load(std::memory_order_relaxed); }
    /* Fill n interleaved-stereo samples; pads with silence on underrun
     * (counted). Returns samples padded (0 = clean). The no-sink form drains the
     * OWNER's built-in main-mix ring (audioRing_) — byte-identical to pre-P5b.
     * The sink form drains that instance's demuxed per-part ring. */
    size_t pullAudio(float *interleavedLR, size_t nFrames);
    size_t pullAudio(AudioSink *sink, float *interleavedLR, size_t nFrames);
    /* Offline-mode variant (§8.3 / VST3 kOffline): block until the device
     * has rendered the requested range — offline bounce through hardware
     * may legitimately wait for the wire. */
    size_t pullAudioBlocking(float *interleavedLR, size_t nFrames, unsigned timeoutMs);
    size_t pullAudioBlocking(AudioSink *sink, float *interleavedLR, size_t nFrames,
                             unsigned timeoutMs);

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
    /* §14.4 host-context-A: assemble the DIAG bundle (a debug snapshot, NOT
     * project state). MIRRORS getStateBundle's shape — deterministic CBOR built
     * under ctlMutex_ off the control path, READ-ONLY: it never touches the audio
     * ring, event rings, padding debt, or any session-mutable state, so it has
     * ZERO effect on the render (the golden gate). It issues `req diag.bundle`
     * under the lock and embeds the device's response body VERBATIM as device-
     * section key 4 (the byte-identical conformance gate), then layers the HOST's
     * own counters (key 5) and audio-config (key 9) on top. With anonymize=true
     * the device-section is decode-reencoded with the §16 PII leaves cleared to ""
     * (mirroring host/harp-probe.c anonymize_device_section); the host sections
     * are numeric (no tstr leaves) so they pass through unchanged. v1 omits the
     * later sub-step keys 6/7/8/10-13 (a vN writer omits unfilled sections).
     * Returns the CBOR bytes (always a valid bundle, even with no device). */
    std::vector<uint8_t> getDiagBundle(bool anonymize = false);

    /* §14.3 host LoopbackMeasurer result. ok=true means the measurement ran end-
     * to-end (armed, routed, echo found); rtt_samples is the measured round-trip in
     * samples at `rate`; expected_samples is the §6.4 prediction (H->D + D->H queue
     * depth); delta_ms = (measured - expected) in ms. echo_found is set even when ok
     * is false on a soft path (so a caller can distinguish "connected+armed but the
     * platform jittered" from "never armed"). */
    struct LoopbackResult {
        bool ok = false;
        bool armed = false;
        bool echo_found = false;
        int in_slot = -1, out_slot = -1;
        uint32_t rate = 0;
        double rtt_samples = 0;
        double expected_samples = 0;
        double delta_ms = 0;
        std::string detail;
    };
    /* Run the §14.3 digital loopback measurement on the LIVE host-paced stream.
     * Requires loopbackArmed() (so audio.start declared the in-slot in key 3) and a
     * live host-paced (USB or §8.7-TCP) session. It quiesces the reader (it owns the
     * audio-IN endpoint for the probe, exactly as audioRenegotiateLocked does — NO
     * teardown, connected_ stays true), issues diag.loopback.start, injects periodic
     * one-sample impulses on the in-slot column in H->D pacing frames, locates the
     * echo on the out-slot column of the D->H frames (the out-slot carries no synth
     * notes, so the impulse dominates), computes RTT = rx_ts - tx_ts minus the start-
     * rsp device-internal latency (key 5 = 0 for digital), sends diag.loopback.stop,
     * then respawns the reader. NEVER touches the render path (gated on the device's
     * loopback_on atomic + the host's own armed flag). */
    LoopbackResult measureLoopback();
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

    /* §8.7 bit-exact: the Ethernet jitter-buffer setpoint (frames). The feeder's
     * audio.trim loop holds audioRing_ here, AND audio.start sends it as key 2 so
     * the device PREFILLS this many frames in a startup burst — otherwise the
     * ppm-limited trim takes seconds to fill from empty (startup silence). */
    static constexpr uint32_t kEthTargetFrames = 2048;

    /* §8.7 latency knobs (measurement / clean-direct-link low-latency mode). The
     * default 2048-frame setpoint + 256-frame packet is the consumer-LAN-safe pair
     * (proven 127 dB). On a clean cable HARP_ETH_TARGET dials the buffer down toward
     * the DAW-block floor and HARP_ETH_NSAMPLES shrinks the device RTP packet. The
     * SAME ethTargetFrames() value flows to BOTH the trim setpoint (runtime.cpp) and
     * the audio.start key-2 prefill, so the device prefills to exactly the setpoint
     * (no startup transient the loop must chase). The fractional-error trim makes the
     * loop critically/well damped at ANY target with NO retuning (see feeder()). */
    uint32_t ethTargetFrames() const {
        uint32_t t = kEthTargetFrames;
        if (const char *e = getenv("HARP_ETH_TARGET")) {
            int v = atoi(e);
            /* env range: floor 64, ceil 12288. The ceil keeps the audioRing_
             * (16384-frame) elastic buffer below its silent write-cap with headroom
             * for jitter excursions and prefill. */
            if (v >= 64 && v <= 12288) t = (uint32_t)v;
        }
        /* UNDERRUN-SAFE FLOOR: the setpoint must clear two DAW pull-blocks, or a
         * single pull can drain the buffer below empty between fills (the steady
         * loop holds it AT target; one pull then dips it by a block — with target
         * < 2·block that dip underruns regardless of how well-damped the loop is).
         * This is the SAME 2·maxDawBlock cushion the USB path uses (targetFramesFor).
         * So the loop is target-invariant down to a few hundred frames, but the
         * REACHABLE floor is bounded by the consumer's block, not by loop gain. */
        uint32_t floor_ = 2u * maxDawBlock_;
        return t > floor_ ? t : floor_;
    }
    static uint32_t ethNsamples() {
        uint32_t n = kBlock;
        if (const char *e = getenv("HARP_ETH_NSAMPLES")) {
            int v = atoi(e);
            /* [32, kBlock]: this knob only LOWERS the packet to cut latency. The
             * device validates up to AUDIO_MAX_NSAMPLES(1024), but the reader's RTP
             * buffers are sized kRtpBufFloats = kBlock·34 (the widest per-part union),
             * so a packet larger than kBlock frames would truncate on a wide union and
             * corrupt the demux. Capping at kBlock keeps every union width within the
             * buffer with zero overflow, and a bigger packet would only raise latency
             * anyway (the opposite of this knob's purpose). */
            if (v >= 32 && v <= (int)kBlock) n = (uint32_t)v;
        }
        return n;
    }

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
    ShellTransport *selectDevice();
    void sessionDown(); /* reap reader+pump, orderly stop if alive, close usb */
    void feeder();      /* runs on the supervisor thread while connected */
    void reader();
    /* demux one union audio frame (ns frames of S slot-interleaved floats): main
     * mix {0,1} -> audioRing_, each per-part sink's columns -> its ring. Shared by
     * the USB framed reader and the §8.7 RTP reader. */
    void demuxUnionFrame(const float *pl, size_t ns, uint16_t S);
    /* reader-thread: smallest occupancy (stereo frames) across the owner main ring
     * AND every per-part sink. The §8.7 ASRC reader pulls until THIS reaches the
     * buffer setpoint, not just the main ring — so the FASTEST-draining consumer is
     * kept fed; a slower over-full ring harmlessly drops the surplus (FloatRing.write
     * caps at capacity). On the bit-exact path the device rate-trim makes this moot. */
    unsigned minRingFillFrames();
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
    /* the per-sink analogue: drop late arrivals owed to a sink's padded SSIs. */
    void settleSinkPadDebt(AudioSink &sink);
    /* B3: on a sink's first pull after its slots (re)entered the union, drop the
     * pad debt + stale ring it accrued while waiting for the re-negotiation, so
     * the first real demuxed frame is not eaten paying that bogus debt. Consumer-
     * side (pullAudio) only. */
    void syncSinkEpoch(AudioSink &sink);
    bool helloAndIdentity();
    /* Build unionSlots_ = owner outSlots_ then every registered sink's slots
     * (deduped, order preserved) and resolve each sink's `cols` to its slots'
     * column indices within that union. Called under sinksMutex_ from
     * audioStart, BEFORE the audio.start request encodes unionSlots_. With no
     * sink registered the union is exactly outSlots_ ({0,1} by default) — the
     * byte-identical golden request. */
    void computeUnionSlotsLocked();
    /* P5b RE-NEGOTIATION decision (off the wire). Build the union that the
     * current sink set WOULD produce — owner outSlots_ then every registered
     * sink's slots, deduped, order preserved, exactly as computeUnionSlotsLocked
     * — into a LOCAL vector and compare it to the LIVE unionSlots_. Returns true
     * iff they differ. Pure read of the registry + outSlots_; touches NO live
     * state (not unionSlots_, not any sink's cols), so register/unregisterAudioSink
     * can call it to decide whether a re-neg is needed WITHOUT disturbing the
     * reader's in-flight demux. Caller holds sinksMutex_. */
    bool unionWouldChangeLocked() const;
    bool audioStart(uint32_t rate);
    void audioStopLocked();
    /* P5b RE-NEGOTIATION (feeder/control thread, under ctlMutex_). A sink that
     * registered/unregistered mid-session changed the required slot set, so the
     * live audio.start union is stale and the late sink reads silence. Re-stream
     * the NEW union WITHOUT a sessionUp (which would reset RT state under the live
     * audio threads — the B1 race) and WITHOUT a reconnect:
     *   1. QUIESCE the reader (set readerStop_, join it) so the audio-IN endpoint
     *      has a SINGLE owner for the stop/start window — the reconnect-free analogue
     *      of sessionDown joining the reader. connected_ stays TRUE throughout, so
     *      the feeder/supervisor never tear the session down (no sessionUp -> no
     *      reset of ssi_/ssiRead_/framesSent_/framesRecv_/padDebtFloats_/the rings
     *      while the audio threads pull — the B1 race is structurally impossible).
     *   2. audio.stop -> DRAIN the stream tail (sole reader, like sessionDown) so
     *      the device's audio writes never block its session loop during the slow
     *      instrumented round-trip -> audio.start with the NEW union (audioStart
     *      recomputes unionSlots_ + every sink's cols). Draining the tail is what
     *      makes the stop/start RELIABLE under TSan (B3) instead of timing the
     *      control link out.
     *   3. Advance the §8.3.1 fence EPOCH: evtEpochBase_ = evtQueuedSeq_.load()
     *      (a single store under ctlMutex_; the monotonic counter is NEVER reset,
     *      so it never races queue*'s fetch_add — the B2 fix). Events queued before
     *      the re-neg fall below the new baseline -> they UNDER-count -> at worst
     *      evt_late at the boundary, never a wedge.
     *   4. RESPAWN the reader: SSI stays continuous (no reset), it just reads the
     *      now-wider frames and demuxes the late sink's columns. The brief gap pads
     *      as silence in pullAudio (the acceptable track-add glitch).
     * Caller holds ctlMutex_; only ever called from the feeder thread, which owns
     * readerThread_'s lifecycle (same thread as sessionUp/sessionDown). */
    void audioRenegotiateLocked();
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
    static void encodeModEvent(harp_cbuf *out, uint32_t id, float offset,
                               uint64_t ts, uint32_t voice, uint8_t srcChan = 0);
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

    ShellTransport *transport_ = nullptr; /* the active binding (USB now; Ethernet next) */
    std::atomic<bool> freeRunning_{false}; /* cached transport_->isFreeRunning() (set at
                                * sessionUp): false on USB, so every existing host-paced line
                                * is reached identically. The Ethernet steps gate the free-
                                * running branches on it. ATOMIC (relaxed everywhere — off the
                                * RT path) because setOffline reads it cross-thread to decide
                                * whether a live mode flip needs a re-dial. */
    std::atomic<bool> wantHostPaced_{false}; /* §8.3-over-§8.7: the DAW is rendering OFFLINE,
                                * so select host-paced (deterministic TCP) for an Ethernet
                                * binding instead of free-running RTP. Set by setOffline()
                                * from the shell's offline hook; read in selectDevice. */
    bool deviceRateLock_ = false; /* §8.7: device advertised "audio.rate-lock" (it honors
                                   * audio.trim). Captured in helloAndIdentity. */
    bool bitExact_ = true; /* §8.7 clock mode (set at sessionUp): a free-running device that
                            * rate-locks => play 1:1 + audio.trim (bit-exact). One that does
                            * NOT => host ASRC-resample. Always true on USB (host-paced;
                            * unused there) so the existing paths are unchanged. */
    std::atomic<uint16_t> unionWidth_{2}; /* §8.7: slot columns per RTP frame (the audio.start
                            * key-4 union the reader demuxes by). Set at audioStart, read per
                            * packet on the reader thread. USB carries it in the frame header. */
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
     * session, like the SSI domain.
     *
     * MONOTONIC across re-negotiations (P5b): queue* only ever fetch_add's it and
     * unregisterSource fetch_sub's it — a re-neg NEVER store(0)'s it (that would
     * race the lock-free queue* on the audio/control threads — the old B2 race).
     * The value a pacing frame FENCES with is (evtQueuedSeq_ - evtEpochBase_): the
     * events queued SINCE the current epoch's audio.start. See evtEpochBase_. */
    std::atomic<uint32_t> evtQueuedSeq_{0};
    /* §8.3.1 fence EPOCH baseline: evtQueuedSeq_'s value at the current stream's
     * audio.start. The device runs evq_reset_for_new_stream() (g_evt_consumed = 0)
     * at every audio.start, so the host must fence with a count that ALSO restarts
     * at 0 per stream — but without resetting the monotonic counter. The pacing
     * frame stamps (evtQueuedSeq_.load() - evtEpochBase_). At the INITIAL audio.start
     * evtQueuedSeq_ == 0 and evtEpochBase_ == 0, so the fenced value is identical to
     * the pre-P5b wire (byte-identical golden). At a re-neg the feeder sets
     * evtEpochBase_ = evtQueuedSeq_.load() (a single store under ctlMutex_, NO race
     * with queue*); events queued before the re-neg fall below the baseline so they
     * UNDER-count — per §8.3.1 the fence is a MINIMUM, so an under-count makes the
     * device render a touch early (at worst evt_late at the boundary), NEVER the
     * over-count that would wedge every later fenced frame. Written only under
     * ctlMutex_ (initial sessionUp and the re-neg); read by the feeder's pacing. */
    std::atomic<uint32_t> evtEpochBase_{0};

    FloatRing audioRing_{1 << 15}; /* 32768 floats = 16384 stereo frames */
    std::atomic<uint64_t> framesRecvAtomic_{0}; /* written by reader, read by feeder */

    /* P5b per-part AUDIO demux (see AudioSink doc above). The OWNER's main mix
     * stays audioRing_ (NOT a sink) — the no-sink pullAudio path is byte-
     * identical. Each ATTACHED per-part instance registers an AudioSink; the
     * registry is a fixed array guarded by sinksMutex_. reader() holds that
     * mutex while it DEMUXES each device frame into every sink's ring (pure
     * memory work — column copy, no I/O), exactly as the eventPump holds
     * sourcesMutex_ over its drain. register/unregisterAudioSink take the same
     * mutex to add/remove a sink — both OFF the RT audio path; pullAudio(sink)
     * never touches the registry (the consumer holds its own sink pointer). So
     * no sink ring is ever multi-producer (reader is the sole writer) or multi-
     * consumer (one instance per sink), and the only shared mutable structure —
     * the pointer array — is mutated only off the RT path under the mutex.
     * SAFE-FREE: unregisterAudioSink removes the sink from the array under the
     * lock FIRST, so reader()'s next demux can never write a freed ring. */
    static constexpr size_t kMaxSinks = 16; /* one per multitimbral part */
    std::mutex sinksMutex_;
    AudioSink *sinks_[kMaxSinks] = {nullptr};
    size_t nSinks_ = 0;
    /* P5b RE-NEGOTIATION. A sink that registered/unregistered while the session
     * is already streaming changed the required slot set; the live audio.start
     * union no longer carries the late sink's slots, so it would read silence
     * (the pre-reneg fixed-at-start limitation). register/unregisterAudioSink set
     * this flag (under sinksMutex_, OFF the wire — they run on the plugin's
     * setActive/main thread) when unionWouldChangeLocked(); the feeder, at a safe
     * boundary under ctlMutex_, observes it and runs audioRenegotiateLocked()
     * (audio.stop -> new union -> audio.start + fence reset). Cleared by the
     * feeder once the re-neg has run. NEVER set by the single-instance / owner-
     * only / no-sink path (the union never changes), so that path NEVER
     * re-negotiates and stays byte-identical (the golden gate). */
    std::atomic<bool> audioRenegPending_{false};
    /* §14.3 LoopbackMeasurer coordination. measureLoopback() (host/main thread)
     * sets loopbackPending_ and BLOCKS until loopbackDone_; the FEEDER thread runs
     * the probe at the SAME safe boundary the re-neg uses (under ctlMutex_, owning
     * both audio endpoints — the reader quiesced exactly as audioRenegotiateLocked
     * does), then publishes loopbackResult_ and raises loopbackDone_. Running it on
     * the feeder means the probe's H->D pacing writes never race the feeder's own
     * pacing (the feeder is doing the probe instead). NEVER set off the golden path
     * (only an armed measureLoopback() call sets it). */
    std::atomic<bool> loopbackPending_{false};
    std::atomic<bool> loopbackDone_{false};
    LoopbackResult loopbackResult_;
    void runLoopbackProbeLocked(); /* feeder-thread body; caller holds ctlMutex_ */
    /* §8.3-over-§8.7 MID-STREAM LIVE<->OFFLINE TOGGLE. setOffline (host thread), on a
     * genuine mode flip on a LIVE Ethernet session, sets modeFlipPending_ (sticky, like
     * audioRenegPending_ — NOT a connected_ stomp an in-flight sessionUp could clobber).
     * The feeder returns on it; the supervisor then re-dials (sessionDown+sessionUp in the
     * new mode). sessionGen_ bumps at the END of every sessionUp; flipTargetGen_ is the
     * absolute generation the offline pull fence waits for (read the ring only once
     * sessionGen_>=flipTargetGen_, so opening samples are the fresh host-paced stream, not
     * the stale live ring). sessionUp clears modeFlipPending_ after the bump. */
    std::atomic<bool> modeFlipPending_{false};
    std::atomic<uint64_t> sessionGen_{0};
    std::atomic<uint64_t> flipTargetGen_{0};
    /* Signal to the reader to EXIT its loop for a re-negotiation WITHOUT tearing
     * the session down. The reader loops on running_ && !readerStop_; the re-neg
     * sets it, joins the reader (so the audio-IN endpoint has a single owner for
     * the stop/drain/start window), then clears it and respawns the reader. Unlike
     * connected_=false (which the feeder/supervisor read as "session died" ->
     * sessionDown/sessionUp -> the B1 state reset), this quiesces ONLY the reader;
     * connected_ stays true and the session continues with a continuous SSI domain.
     * Only the feeder thread sets/clears it and joins/respawns readerThread_ (the
     * same thread sessionUp/sessionDown manage it on), so no concurrent join. */
    std::atomic<bool> readerStop_{false};
    /* monotonic count of completed re-negotiations (see renegCount()); bumped by
     * audioRenegotiateLocked on a successful audio.start, read by a test harness. */
    std::atomic<uint32_t> renegCount_{0};
    /* relaxed mirror of nSinks_ != 0 so reader()'s hot loop SKIPS the demux lock
     * entirely when no per-part sink exists — the single-instance / default
     * main-mix reader stays exactly the pre-P5b lock-free path. Published with
     * release under sinksMutex_ on every add/remove; the reader's relaxed load
     * only ever turns the (locked) demux on AFTER a sink is registered (off the
     * audio path, before that session's frames matter). */
    std::atomic<bool> haveSinks_{false};
    /* The UNION of every instance's requested output slots, in the exact order
     * sent as audio.start key 4 (owner outSlots_ first, then each registered
     * sink's slots, deduplicated). reader() demuxes the device frame's columns
     * by this order; each sink's `cols` index into it. Fixed at audioStart (the
     * P5b mid-attach limitation — see audioStart). */
    std::vector<uint32_t> unionSlots_;

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
    /* audio thread: drain echoed device-side edits (§9.4 echo) for THIS instance's part
     * only. A device front-panel edit carries its part; an instance must reflect ONLY
     * its own part, else every instance mirrors every part's edits (the bug). Echoes for
     * other parts are dropped here. */
    bool popEcho(uint32_t myPart, uint32_t &id, float &v) {
        ParamChange c;
        while (echoRing_.pop(c)) {
            if (c.part != (uint16_t)myPart) continue; /* another part's edit — not ours */
            id = c.id;
            v = c.value;
            return true;
        }
        return false;
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

    /* §14.3 LoopbackMeasurer arming (host side). -1/-1 = OFF, the byte-identical
     * golden path: audio.start sends the unchanged key-3 array and the feeder never
     * builds an H->D payload. Set via setLoopbackSlots() before audioStart(); read
     * by audioStart() (to declare the in-slot in key 3) and by measureLoopback(). */
    int loopbackIn_ = -1, loopbackOut_ = -1;

    /* §6.4 latency-profile, cached from the hello identity (key 8) at
     * helloAndIdentity(). Indexed implicitly by rate (matched in expectedLoopback).
     * Used ONLY by the §14.3 measurement; unset (nLat_==0) on a device that omits
     * key 8 (the probe then falls back to a one-block host-paced turnaround). */
    static constexpr size_t kMaxLatProfiles = 8;
    struct LatProfile { uint32_t rate, in_lat, out_lat, buf_depth; };
    LatProfile latProfiles_[kMaxLatProfiles];
    size_t nLat_ = 0;
    /* §6.4 expected round-trip in samples for the negotiated rate: input-latency +
     * output-latency + one buffer-depth turnaround (the pure-transport buffering the
     * §14.3 digital loop measures, device-internal loop latency = 0). Falls back to
     * kBlock (one host-paced pacing block) if the device omitted the profile. */
    double expectedLoopbackSamples(uint32_t rate) const {
        for (size_t i = 0; i < nLat_; i++)
            if (latProfiles_[i].rate == rate)
                return (double)latProfiles_[i].in_lat + latProfiles_[i].out_lat +
                       latProfiles_[i].buf_depth;
        return (double)kBlock;
    }

    /* (the OWNER instance's part lives in ownerSource_.chan — see above; an
     * ATTACHED instance's part lives in the source it got from registerSource.
     * Notes don't read it — their channel is already baked into the UMP word
     * by the shell; only param sets/ramps need the per-source channel.) */
};
