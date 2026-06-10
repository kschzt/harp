/* HARP RefDev — VST3 shell (§15): presents a HARP device to a DAW.
 *
 * v0: one stereo output, host-paced audio via the embedded runtime, the 8
 * device params as automatable parameters, getState/setState = Recall
 * Bundle. Known deviations from spec, by design for now: no four-actions
 * UI (auto-Push with archive on mismatch), params pushed via the vendor
 * knob method instead of §9 events (event plane is the next milestone).
 */
#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
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
};
static const DevParam kParams[] = {
    {1, "Osc Pitch"},   {2, "Osc Shape"},  {3, "Filter Cutoff"}, {4, "Filter Reso"},
    {5, "Env Attack"},  {6, "Env Release"}, {7, "FX Send"},       {8, "Master Level"},
};
static constexpr int kNumParams = sizeof(kParams) / sizeof(kParams[0]);

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
        return kResultOk;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        rate_ = (uint32_t)setup.sampleRate;
        offline_ = setup.processMode == kOffline;
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

        /* parameter changes -> lock-free queue (feeder pushes to device) */
        if (data.inputParameterChanges) {
            int32 nq = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < nq; i++) {
                IParamValueQueue *q = data.inputParameterChanges->getParameterData(i);
                if (!q) continue;
                int32 np = q->getPointCount();
                if (np <= 0) continue;
                int32 off;
                ParamValue v;
                if (q->getPoint(np - 1, off, v) == kResultOk)
                    rt.setParam((uint32_t)q->getParameterId(), (float)v);
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
};

/* ---------------- controller ---------------- */

class HarpController : public EditController {
public:
    static FUnknown *createInstance(void *) {
        return (IEditController *)new HarpController();
    }

    tresult PLUGIN_API initialize(FUnknown *context) override {
        tresult r = EditController::initialize(context);
        if (r != kResultOk) return r;
        for (auto &p : kParams) {
            UString256 title(p.name);
            parameters.addParameter(title, nullptr, 0, 0.5,
                                    ParameterInfo::kCanAutomate, p.id);
        }
        return kResultOk;
    }

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
            if (rt.stagedParam(p.id, v)) setParamNormalized(p.id, v);
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
