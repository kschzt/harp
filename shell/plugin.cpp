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
        offline_ = setup.processMode == kOffline;
        /* the ring cushion (and thus reported latency) scales with the
         * DAW's block size — a 1024-sample pull needs a deeper ring than
         * a 64-sample one */
        HarpRuntime::instance().configure(rate_, (uint32_t)setup.maxSamplesPerBlock);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) override {
        if (state)
            HarpRuntime::instance().start(rate_);
        else
            HarpRuntime::instance().stop();
        return AudioEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() override {
        return HarpRuntime::instance().latencySamples();
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API process(ProcessData &data) override {
        auto &rt = HarpRuntime::instance();

        /* Stream-domain "now" for this block: events at DAW offset s map to
         * SSI base + s; the latency term is repaid by the host's PDC, so
         * they sound exactly when the DAW intended (§9.2). */
        uint64_t base = rt.streamPos() + rt.latencySamples();

        /* transport (§9.7): an event on every CHANGE — play/stop, tempo,
         * locate or loop wrap (detected as a discontinuity between this
         * block's musical position and the last block's prediction) — plus
         * a >= 1 Hz refresh while playing (spec MUST). PPQ is anchored at
         * `base`, this block's start in the stream domain: after PDC that
         * is exactly when Live hears this block, so the device's musical
         * grid aligns with the project grid by construction. */
        if (data.processContext) {
            const ProcessContext &pc = *data.processContext;
            rt.feedTransport((pc.state & ProcessContext::kPlaying) != 0,
                             (pc.state & ProcessContext::kTempoValid) != 0, pc.tempo,
                             (pc.state & ProcessContext::kProjectTimeMusicValid) != 0,
                             pc.projectTimeMusic, (uint32_t)data.numSamples, base);
        }

        /* note events -> UMP (§9.10), timestamped to the sample */
        if (data.inputEvents) {
            int32 ne = data.inputEvents->getEventCount();
            for (int32 i = 0; i < ne; i++) {
                Event ev;
                if (data.inputEvents->getEvent(i, ev) != kResultOk) continue;
                if (ev.type == Event::kNoteOnEvent) {
                    uint32_t note = (uint32_t)(ev.noteOn.pitch & 0x7f);
                    uint32_t vel = (uint32_t)(ev.noteOn.velocity * 127.f + 0.5f);
                    if (vel == 0) vel = 1;
                    if (vel > 127) vel = 127;
                    rt.queueNote(0x20900000u | (note << 8) | vel,
                                 base + (uint64_t)ev.sampleOffset);
                } else if (ev.type == Event::kNoteOffEvent) {
                    uint32_t note = (uint32_t)(ev.noteOff.pitch & 0x7f);
                    rt.queueNote(0x20800000u | (note << 8) | 0x40,
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
                rt.queueParamSet((uint32_t)idx + 1, pendVal_[idx], 0);
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
                        rt.queueNote(0x20B07B00u, 0);
                        continue;
                    }
                    if (idx == SIZE_MAX) {
                        rt.queueParamSet(id, (float)v, ts);
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
                        rt.queueRamp(id, (float)v, lastTs_[idx], ts);
                    else
                        rt.queueParamSet(id, (float)v, ts);
                    lastTs_[idx] = ts;
                    hasLast_[idx] = true;
                    pendHas_[idx] = false;
                }
            }
        }

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

    /* component state = Recall Bundle (§15.3) */
    tresult PLUGIN_API getState(IBStream *state) override {
        std::vector<uint8_t> bundle;
        if (!HarpRuntime::instance().getStateBundle(bundle)) return kResultFalse;
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
        return HarpRuntime::instance().setStateBundle(bundle.data(), bundle.size())
                   ? kResultOk
                   : kResultFalse;
    }

private:
    uint32_t rate_ = 48000;
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
        auto &rt = HarpRuntime::instance();
        /* the processor's setState stages the bundle; we read knob values
         * from the staged params (setStateBundle parsed them) */
        rt.setStateBundle(bundle.data(), bundle.size());
        for (auto &p : kParams) {
            float v;
            if (rt.bundleParam(p.id, v)) setParamNormalized(p.id, v);
        }
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
