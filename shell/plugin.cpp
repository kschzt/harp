/* HARP RefDev — VST3 shell (§15): presents a HARP device to a DAW.
 *
 * One stereo output, host-paced audio via the embedded runtime, the 13
 * device params as automatable parameters (automation becomes §9.4
 * ramps, notes travel as §9.10 UMP, ProcessContext becomes §9.7
 * transport), getState/setState = Recall Bundle. The AU shell
 * (shell/au) wraps the same runtime; both render byte-identically.
 * Remaining spec deviation, by design: no four-actions UI yet —
 * mismatches auto-resolve by Push-with-archive, which is loss-free.
 */
#include <cstring>
#include <string>
#include <vector>

#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include "runtime.h"
#include "runtime_registry.h"
#include "ump.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

/* Frozen identity — see docs/vst3-shell-plan.md. NEVER change. */
static const FUID kHarpProcessorUID(0xB520EC1F, 0x856F4A80, 0xA09D6455, 0x12430ACB);
static const FUID kHarpControllerUID(0x3AF7D698, 0x0DB04F6E, 0x8F107EEF, 0x7480467A);

/* Mirrors the refdev's parameter set; replaced by evt.params descriptors
 * once the event plane lands. */
struct DevParam {
    uint32_t id;
    const char *name;
    int32 stepCount;   /* 0 = continuous (VST3: stepCount = steps - 1) */
    double defaultVal; /* must mirror the device defaults (recall sanity) */
};
static const DevParam kParams[] = {
    {1, "Osc Pitch", 0, 0.5},    {2, "Osc Shape", 0, 0.5},
    {3, "Filter Cutoff", 0, 0.5}, {4, "Filter Reso", 0, 0.5},
    {5, "Env Attack", 0, 0.5},   {6, "Env Release", 0, 0.5},
    {7, "FX Send", 0, 0.5},      {8, "Master Level", 0, 0.5},
    /* the arp (device params 9-12; param-map-hash changed with these) */
    {9, "Arp Mode", 4, 0.0},     {10, "Arp Division", 5, 0.6},
    {11, "Arp Gate", 0, 0.5},    {12, "Arp Octaves", 3, 0.0},
    {13, "Glide", 0, 0.0}, /* 0 = off; portamento is opt-in now */
};
static constexpr int kNumParams = sizeof(kParams) / sizeof(kParams[0]);
/* hidden parameter the DAW's panic (CC 120/123) maps onto via IMidiMapping */
static constexpr uint32_t kPanicParamId = 99;

/* ---------------- processor ---------------- */

class HarpProcessor : public AudioEffect {
public:
    HarpProcessor() { setControllerClass(kHarpControllerUID); }

    /* Defensive teardown: the host calls setActive(false) before destroying
     * us (which releases the handle), but if it doesn't, release here so the
     * shared refcount is correct and a private runtime is not leaked — the
     * same net cleanup the old by-value member's ~HarpRuntime gave. */
    ~HarpProcessor() override {
        releaseSource(); /* attached: drop our event source before the runtime */
        runtime_release(handle_);
        handle_ = RuntimeHandle{};
    }

    static FUnknown *createInstance(void *) {
        return (IAudioProcessor *)new HarpProcessor();
    }

    tresult PLUGIN_API initialize(FUnknown *context) override {
        tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        /* Live refuses Instrument-category plugins without an event input
         * ("no valid event input bus"). Notes are ignored until §9.10 UMP
         * carriage lands — but the bus must exist. */
        addEventInput(STR16("MIDI In"), 16);
        return kResultOk;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        rate_ = (uint32_t)setup.sampleRate;
        maxBlock_ = (uint32_t)setup.maxSamplesPerBlock;
        offline_ = setup.processMode == kOffline;
        /* The ring cushion (and thus reported latency) scales with the DAW's
         * block size — a 1024-sample pull needs a deeper ring than a 64-sample
         * one. We CAPTURE the params here and let the OWNER apply them via
         * rt->configure() at acquire time (setActive), so the single-instance
         * sequence configure()->start() stays byte-identical even though the
         * runtime now comes from the registry. setupProcessing runs while
         * inactive (no runtime yet) for a fresh instance; if a runtime is
         * already acquired (a re-setup) and we own it, configure it now too. */
        if (runtime() && owner()) runtime()->configure(rate_, maxBlock_);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            /* Idempotent against a redundant setActive(true) (some hosts double-
             * activate): drop any handle we already hold before re-acquiring, so
             * we never leak a reference or strand a shared session's refcount.
             * Drop our event source first too (same ordering rule as release). */
            if (handle_.rt) {
                releaseSource();
                runtime_release(handle_);
                handle_ = RuntimeHandle{};
            }
            /* Acquire the (possibly shared) runtime for THIS instance's target
             * unit. The wanted serial is the explicit target, in priority:
             *   1. HARP_DEVICE_SERIAL env — the field/test pin (also how the
             *      out-of-process host forces a unit, like HARP_OUT_SLOTS).
             *   2. the loaded bundle's usb serial, if the project pinned one.
             *   3. else "" — NO explicit target -> a fresh, UNSHARED runtime
             *      that auto-selects, byte-identical to the old by-value member
             *      (the golden / single-instance / #16 multi-device gate). */
            std::string wantSerial;
            if (const char *e = getenv("HARP_DEVICE_SERIAL"); e && e[0])
                wantSerial = e;
            else if (!pendingState_.empty())
                wantSerial = HarpRuntime::bundleWantedSerial(pendingState_.data(),
                                                             pendingState_.size());

            handle_ = runtime_acquire(wantSerial);
            if (owner()) {
                /* First/sole instance for this unit: drive it exactly as the
                 * old by-value runtime did — configure, stage the project's
                 * bundle (stage-before-start, as setState did pre-acquire),
                 * then start. The owner's event source is the runtime's built-
                 * in ownerSource_ (its channel is set inside start() from
                 * HARP_CHANNEL — byte-identical single-instance path). */
                runtime()->configure(rate_, maxBlock_);
                if (!pendingState_.empty())
                    runtime()->setStateBundle(pendingState_.data(), pendingState_.size());
                runtime()->start(rate_);
                source_ = runtime()->ownerSource();
            } else {
                /* ATTACHED (P5): the shared session is already configured/
                 * started and streaming under the sibling owner. This instance
                 * is NO LONGER DORMANT FOR EVENTS — it registers its OWN event
                 * source (its part) and queues notes/params on it; the owner's
                 * eventPump merges every source onto the one session, so the
                 * group PLAYS multitimbrally. AUDIO stays dormant though: the
                 * owner pulls the MAIN MIX (which sums all parts) and this
                 * sibling emits silence — per-part audio demux is P5b. The
                 * part comes from HARP_CHANNEL (the out-of-process host's
                 * --channel), exactly as the owner reads it in start(). */
                /* FOLLOW-UP P5c — recall-safe per-instance channel. HARP_CHANNEL
                 * is a PROCESS-GLOBAL env: fine for the headless tsan-host test
                 * (it sets each instance's channel programmatically), but in a
                 * real DAW every plugin instance reads the SAME env, so all
                 * aliases would land on ONE part. Real in-DAW multitimbral needs
                 * a per-instance channel that PERSISTS with the project — a
                 * plugin parameter or a per-instance bundle field, recall-safe so
                 * it survives save/reload — NOT a global env. Not implemented
                 * here: a plugin-param channel needs recall-safe persistence
                 * design (parameter id, automation/display semantics, bundle
                 * round-trip). Until then, in-DAW aliasing collapses onto the
                 * env's single part. */
                uint8_t ch = 0;
                if (const char *e = getenv("HARP_CHANNEL"); e && e[0]) {
                    int v = atoi(e);
                    if (v >= 0 && v <= 15) ch = (uint8_t)v;
                }
                source_ = runtime()->registerSource(ch);
            }
        } else {
            /* Release: an ATTACHED instance first removes its event source so
             * the owner's eventPump stops draining it and never touches a freed
             * source (unregisterSource is a no-op on the owner source / null).
             * MUST happen BEFORE runtime_release — release may be the last one,
             * which stops+destroys the runtime; unregistering after that would
             * touch a freed runtime. Then drop the reference: the LAST holder of
             * a shared runtime stops+destroys it (joining its threads); a
             * private (unshared) runtime is torn down outright. */
            releaseSource();
            runtime_release(handle_);
            handle_ = RuntimeHandle{};
        }
        return AudioEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() override {
        /* The reported latency is a pure function of the DAW block size, so it
         * is byte-identical whether we ask the live runtime or compute it
         * statically (the runtime now arrives from the registry at activate-
         * time, so a host querying latency in the inactive window has nothing
         * to ask). An OWNER reports its live runtime's value.
         *
         * P5: an ATTACHED instance must report the SAME latency as the owner.
         * Its audio bus is silent, but it now SENDS events stamped at
         * streamPos() + latencySamples() (full PDC) — so the host must delay
         * this instance's EVENT timeline by that same latency, or the part's
         * notes/automation land misaligned against the owner's. (Reporting 0
         * was a P4 leftover from when attached instances were fully dormant.) */
        if (runtime()) return runtime()->latencySamples();
        return HarpRuntime::latencyFor(maxBlock_);
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    /* Zero the output bus and flag it silent. The dormant-attached path (P4)
     * and any pre-acquire call route here: no runtime touch, no event queueing,
     * just clean silence so the host bus is well-defined. */
    tresult processSilence(ProcessData &data) {
        if (data.numOutputs < 1 || data.numSamples <= 0 ||
            data.outputs[0].numChannels < 1 || !data.outputs[0].channelBuffers32)
            return kResultOk;
        int32 nch = data.outputs[0].numChannels;
        for (int32 c = 0; c < nch; c++) {
            float *ch = data.outputs[0].channelBuffers32[c];
            if (ch) memset(ch, 0, (size_t)data.numSamples * sizeof(float));
        }
        data.outputs[0].silenceFlags = (nch >= 64) ? ~0ull : ((1ull << nch) - 1);
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData &data) override {
        /* Unacquired (queried before activate): nothing to drive — clean
         * silence so the host bus is well-defined. */
        if (!runtime() || !source_) return processSilence(data);

        HarpRuntime &rt = *runtime();
        const bool isOwner = owner();
        /* P5: ATTACHED instances are NO LONGER DORMANT FOR EVENTS — they queue
         * notes/params on THEIR OWN source (source_), which the owner's
         * eventPump merges onto the one session, so the multitimbral group
         * PLAYS. What they still MUST NOT touch is the OWNER-only state: the
         * single audio ring (one consumer — they emit silence, not pull) and
         * the transport-change detector (one driver — the owner anchors the
         * session's musical time; an attached feedTransport would push a
         * second, conflicting transport stream). AUDIO demux per part is P5b. */

        /* Stream-domain "now" for this block: events at DAW offset s map to
         * SSI base + s; the latency term is repaid by the host's PDC, so
         * they sound exactly when the DAW intended (§9.2). streamPos advances
         * only as the OWNER pulls audio, but it is a shared-session read every
         * instance timestamps against — all parts ride the one stream clock. */
        uint64_t base = rt.streamPos() + rt.latencySamples();

        /* transport (§9.7): OWNER ONLY. An event on every CHANGE — play/stop,
         * tempo, locate or loop wrap (detected as a discontinuity between this
         * block's musical position and the last block's prediction) — plus a
         * >= 1 Hz refresh while playing (spec MUST). PPQ is anchored at `base`,
         * this block's start in the stream domain: after PDC that is exactly
         * when Live hears this block, so the device's musical grid aligns with
         * the project grid by construction. Transport is global, so only the
         * owner drives it (queueTransport pins it to the owner source anyway —
         * but the change DETECTOR must run on one thread, the owner's). */
        if (isOwner && data.processContext) {
            const ProcessContext &pc = *data.processContext;
            rt.feedTransport((pc.state & ProcessContext::kPlaying) != 0,
                             (pc.state & ProcessContext::kTempoValid) != 0, pc.tempo,
                             (pc.state & ProcessContext::kProjectTimeMusicValid) != 0,
                             pc.projectTimeMusic, (uint32_t)data.numSamples, base);
        }

        /* note events -> UMP (§9.10), timestamped to the sample, on OUR source */
        if (data.inputEvents) {
            int32 ne = data.inputEvents->getEventCount();
            for (int32 i = 0; i < ne; i++) {
                Event ev;
                if (data.inputEvents->getEvent(i, ev) != kResultOk) continue;
                if (ev.type == Event::kNoteOnEvent) {
                    uint32_t note = (uint32_t)(ev.noteOn.pitch & 0x7f);
                    uint32_t vel = (uint32_t)(ev.noteOn.velocity * 127.f + 0.5f);
                    uint32_t chan = (uint32_t)ev.noteOn.channel & 0xf; /* -> device part (P2.1) */
                    if (vel == 0) vel = 1;
                    if (vel > 127) vel = 127;
                    rt.queueNote(source_, ump_note_on(note, vel, chan),
                                 base + (uint64_t)ev.sampleOffset);
                } else if (ev.type == Event::kNoteOffEvent) {
                    uint32_t note = (uint32_t)(ev.noteOff.pitch & 0x7f);
                    uint32_t chan = (uint32_t)ev.noteOff.channel & 0xf;
                    rt.queueNote(source_, ump_note_off(note, chan),
                                 base + (uint64_t)ev.sampleOffset);
                }
            }
        }

        /* parameter changes -> timestamped sets; consecutive points become
         * §9.4 ramps — a DAW curve as a handful of ramps (§9.1). Thinned to
         * one emission per param per 256 samples: at 64-sample buffers a
         * DAW sends ~750 points/s/param, and ramps spanning 64 samples say
         * nothing a 256-sample ramp doesn't (the device interpolates at
         * control rate regardless). Skipped points fold into the next
         * ramp's target; a pend with no successor flushes below. */
        for (size_t idx = 0; idx < kNumParams; idx++) {
            /* Flush a pending fold only when the gesture is OVER — no
             * successor point for a full pacing block. Flushing one DAW
             * block after the fold (the original logic) emitted 64-sample
             * ramps whose END was already at "now": ~1100/s of
             * guaranteed-stale timestamps at 64-sample buffers (measured;
             * invisible at >= 256 where folding never triggers). The pend
             * holds a gesture's final settling value; a "now" set delivers
             * it without inventing a timestamp the stream already passed. */
            if (pendHas_[idx] && base >= pendTs_[idx] + 256) {
                rt.queueParamSet(source_, (uint32_t)idx + 1, pendVal_[idx], 0);
                lastTs_[idx] = pendTs_[idx];
                pendHas_[idx] = false;
            }
        }
        if (data.inputParameterChanges) {
            int32 nq = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < nq; i++) {
                IParamValueQueue *q = data.inputParameterChanges->getParameterData(i);
                if (!q) continue;
                uint32_t id = (uint32_t)q->getParameterId();
                int32 np = q->getPointCount();
                size_t idx = (id >= 1 && id <= kNumParams) ? id - 1 : SIZE_MAX;
                for (int32 k = 0; k < np; k++) {
                    int32 off;
                    ParamValue v;
                    if (q->getPoint(k, off, v) != kResultOk) continue;
                    uint64_t ts = base + (uint64_t)off;
                    if (id == kPanicParamId) { /* DAW panic -> all-notes-off */
                        rt.queueNote(source_, ump_all_notes_off(), 0);
                        continue;
                    }
                    if (idx == SIZE_MAX) {
                        rt.queueParamSet(source_, id, (float)v, ts);
                        continue;
                    }
                    if (hasLast_[idx] && ts > lastTs_[idx] && ts - lastTs_[idx] < 256) {
                        pendHas_[idx] = true; /* too soon: fold into next ramp */
                        pendTs_[idx] = ts;
                        pendVal_[idx] = (float)v;
                        continue;
                    }
                    bool ramp = hasLast_[idx] && ts > lastTs_[idx] &&
                                ts - lastTs_[idx] <= 4800; /* >100 ms gap = a jump */
                    if (ramp)
                        rt.queueRamp(source_, id, (float)v, lastTs_[idx], ts);
                    else
                        rt.queueParamSet(source_, id, (float)v, ts);
                    lastTs_[idx] = ts;
                    hasLast_[idx] = true;
                    pendHas_[idx] = false;
                }
            }
        }

        /* AUDIO: OWNER ONLY. The shared session's audio ring has ONE consumer
         * (the owner pulls the MAIN MIX, which already sums every part this
         * group injected above). An attached instance has queued its part's
         * events and is done — it emits SILENCE and must NOT pull the ring
         * (a second consumer would corrupt the SPSC tail and steal the owner's
         * samples) nor drain the echo ring (also single-consumer). Per-part
         * audio demux — each alias hearing only its own part — is the tracked
         * follow-up P5b; until then siblings are audio-silent by design. */
        if (!isOwner) return processSilence(data);

        if (data.numOutputs < 1 || data.numSamples <= 0 ||
            data.outputs[0].numChannels < 1 || !data.outputs[0].channelBuffers32)
            return kResultOk;
        int32 nch = data.outputs[0].numChannels;
        float *L = data.outputs[0].channelBuffers32[0];
        float *R = nch > 1 ? data.outputs[0].channelBuffers32[1] : nullptr;
        if (!L) return kResultOk;

        /* pull interleaved from the ring; deinterleave into bus channels
         * (mono hosts get L+R summed) */
        float tmp[4096 * 2];
        int32 remaining = data.numSamples;
        int32 written = 0;
        while (remaining > 0) {
            int32 chunk = remaining > 4096 ? 4096 : remaining;
            if (offline_) /* offline bounce: waiting for the wire is correct */
                rt.pullAudioBlocking(tmp, (size_t)chunk, 1000);
            else
                rt.pullAudio(tmp, (size_t)chunk); /* RT: silence on underrun */
            for (int32 s = 0; s < chunk; s++) {
                if (R) {
                    L[written + s] = tmp[2 * s];
                    R[written + s] = tmp[2 * s + 1];
                } else {
                    L[written + s] = 0.5f * (tmp[2 * s] + tmp[2 * s + 1]);
                }
            }
            written += chunk;
            remaining -= chunk;
        }
        data.outputs[0].silenceFlags = 0;

        /* device front-panel echoes (§9.4) -> output parameter changes:
         * the sanctioned RT path for plugin-originated edits; the host
         * updates the controller and records automation when armed */
        {
            uint32_t id;
            float v;
            while (rt.popEcho(id, v)) {
                if (!data.outputParameterChanges) continue; /* drain regardless */
                int32 qi = 0;
                IParamValueQueue *q = data.outputParameterChanges->addParameterData(id, qi);
                if (q) {
                    int32 pi = 0;
                    q->addPoint(0, v, pi);
                }
            }
        }
        return kResultOk;
    }

    /* component state = Recall Bundle (§15.3). getStateBundle is a ctlMutex_-
     * guarded READ of the shared device state, so it is safe for an ATTACHED
     * instance too — it reports the same project state the shared session
     * holds. With no runtime yet (queried before activate) there is nothing
     * to pull. */
    tresult PLUGIN_API getState(IBStream *state) override {
        if (!runtime()) return kResultFalse;
        std::vector<uint8_t> bundle;
        if (!runtime()->getStateBundle(bundle)) return kResultFalse;
        int32 written = 0;
        return state->write(bundle.data(), (int32)bundle.size(), &written);
    }

    tresult PLUGIN_API setState(IBStream *state) override {
        std::vector<uint8_t> bundle;
        uint8_t buf[8192];
        int32 got = 0;
        while (state->read(buf, sizeof buf, &got) == kResultOk && got > 0) {
            bundle.insert(bundle.end(), buf, buf + got);
            if (got < (int32)sizeof buf) break;
        }
        if (bundle.empty()) return kResultFalse;
        /* ALWAYS stash the raw bundle: it is this instance's project state and
         * the source of the registry's wanted serial at acquire time. (On
         * project-open setState lands before setupProcessing/setActive, so the
         * runtime usually does not exist yet — staging here reproduces today's
         * stage-before-start; the owner applies it just before start().) */
        pendingState_ = bundle;

        if (owner() && runtime())
            /* Live, sole/owning instance: push now, exactly as before. */
            return runtime()->setStateBundle(bundle.data(), bundle.size()) ? kResultOk
                                                                      : kResultFalse;
        /* ATTACHED: the shared session is the OWNER's to (re)assert — an
         * attached push would fight the owner's "Live wins" bundle on one
         * device. So we only stage locally (P5 will route per-part state). */
        /* No runtime yet: staged above; the owner will apply it at activate. */
        return kResultOk;
    }

private:
    /* The runtime this instance drives — obtained from the PROCESS-GLOBAL
     * registry (P4), not owned by value. Instances that target the SAME unit
     * (same explicit serial) share ONE runtime / ONE USB claim; that is what
     * makes multitimbral aliasing possible (P5). A single instance, or one
     * that auto-selects (no explicit serial), gets its OWN fresh runtime with
     * handle_.owner == true and behaves BYTE-IDENTICALLY to the old by-value
     * member. See runtime_registry.h for the "not the old singleton" rule.
     *   owner == true:  drive the session — configure / pull MAIN-MIX audio /
     *                   anchor transport / get+set state — and queue events on
     *                   the runtime's built-in OWNER source (byte-identical to
     *                   today for a single instance).
     *   owner == false: ATTACHED (P5) — a sibling owns and streams the shared
     *                   session. This instance is no longer dormant for EVENTS:
     *                   it queues notes/params on its OWN registered source_
     *                   (its part), which the owner's eventPump merges onto the
     *                   one session, so the group PLAYS multitimbrally. It stays
     *                   AUDIO-SILENT (the owner pulls the summed main mix; per-
     *                   part audio demux is the follow-up P5b) and does NOT
     *                   anchor transport (the owner is canonical) or push state
     *                   (the owner's bundle wins on the one device). */
    RuntimeHandle handle_;
    /* This instance's event SOURCE (P5): the runtime's built-in owner source
     * for an owner, a per-instance registered source for an attached one. All
     * queue* route through it so each instance is the SOLE producer of its own
     * SPSC ring. nullptr until acquired (process() emits silence then). */
    EventSource *source_ = nullptr;
    /* Drop our event source. For an ATTACHED instance this removes + frees the
     * source we registered (after which the owner's eventPump never touches it);
     * for an owner / unacquired instance it is a no-op (the owner source belongs
     * to the runtime and persists for the session). MUST run before
     * runtime_release (a last release destroys the runtime). Idempotent. */
    void releaseSource() {
        if (source_ && runtime()) runtime()->unregisterSource(source_);
        source_ = nullptr;
    }
    HarpRuntime *runtime() const { return handle_.rt; }
    bool owner() const { return handle_.owner; }
    uint32_t rate_ = 48000;
    uint32_t maxBlock_ = 1024; /* captured in setupProcessing; the owner feeds
                                  it to rt->configure() at acquire/start time
                                  (the ring cushion scales with it) */
    /* Raw recall bundle staged by setState BEFORE a runtime exists (state is
     * restored on project-open, before setActive/acquire). The OWNER applies
     * it via setStateBundle() right before start(), reproducing today's
     * stage-before-start ordering. Also the source of the wanted serial used
     * as the registry key when no HARP_DEVICE_SERIAL env pins one. */
    std::vector<uint8_t> pendingState_;
    bool offline_ = false;
    /* per-param ramp-synthesis state: last emitted point + pending folded
     * point awaiting the 256-sample thinning interval */
    uint64_t lastTs_[kNumParams] = {};
    bool hasLast_[kNumParams] = {};
    bool pendHas_[kNumParams] = {};
    uint64_t pendTs_[kNumParams] = {};
    float pendVal_[kNumParams] = {};


};

/* ---------------- controller ---------------- */

class HarpController : public EditController, public IMidiMapping {
public:
    static FUnknown *createInstance(void *) {
        return (IEditController *)new HarpController();
    }

    tresult PLUGIN_API initialize(FUnknown *context) override {
        tresult r = EditController::initialize(context);
        if (r != kResultOk) return r;
        for (auto &p : kParams) {
            UString256 title(p.name);
            parameters.addParameter(title, nullptr, p.stepCount, p.defaultVal,
                                    ParameterInfo::kCanAutomate, p.id);
        }
        parameters.addParameter(STR16("Panic"), nullptr, 0, 0,
                                ParameterInfo::kIsHidden, kPanicParamId);
        return kResultOk;
    }

    /* DAW panic reaches plugins as CC 120/123 through the MIDI mapping —
     * route both onto the hidden Panic param */
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 /*channel*/,
                                                   CtrlNumber cc,
                                                   ParamID &id) override {
        if (busIndex == 0 && (cc == kCtrlAllSoundsOff || cc == kCtrlAllNotesOff)) {
            id = kPanicParamId;
            return kResultTrue;
        }
        return kResultFalse;
    }

    OBJ_METHODS(HarpController, EditController)
    DEFINE_INTERFACES
    DEF_INTERFACE(IMidiMapping)
    END_DEFINE_INTERFACES(EditController)
    REFCOUNT_METHODS(EditController)

    /* component state arrives here too on project load: surface the saved
     * knob positions so the DAW shows what the device will hold */
    tresult PLUGIN_API setComponentState(IBStream *state) override {
        std::vector<uint8_t> bundle;
        uint8_t buf[8192];
        int32 got = 0;
        while (state->read(buf, sizeof buf, &got) == kResultOk && got > 0) {
            bundle.insert(bundle.end(), buf, buf + got);
            if (got < (int32)sizeof buf) break;
        }
        if (bundle.empty()) return kResultFalse;
        /* The Controller is a separate object from the Processor (possibly
         * a separate process): it must NOT open a device or own a runtime.
         * The bundle is self-describing (it embeds its object closure), so
         * we extract knob values straight from it, via a transient store
         * in the shared cache dir (content-addressed, safe to share). */
        char dir[512];
        HarpRuntime::defaultStoreDir(dir, sizeof dir);
        harp_store store;
        if (harp_store_open(&store, dir) != 0) return kResultFalse;
        std::vector<std::pair<uint32_t, float>> params;
        if (HarpRuntime::bundleParams(bundle.data(), bundle.size(), &store, params))
            for (auto &kv : params)
                setParamNormalized(kv.first, kv.second);
        return kResultOk;
    }
};

/* ---------------- factory ---------------- */

#define stringPluginName "HARP RefDev"

BEGIN_FACTORY_DEF("HARP Project", "https://github.com/kschzt/harp",
                  "mailto:harp@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(kHarpProcessorUID), PClassInfo::kManyInstances,
           kVstAudioEffectClass, stringPluginName, Vst::kDistributable,
           Vst::PlugType::kInstrumentSynth, "0.1.0", kVstVersionString,
           HarpProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(kHarpControllerUID), PClassInfo::kManyInstances,
           kVstComponentControllerClass, stringPluginName " Controller", 0, "", "0.1.0",
           kVstVersionString, HarpController::createInstance)

END_FACTORY
