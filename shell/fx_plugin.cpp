/* HARP FX — VST3 shell variant (§8.8 audio.fx): presents a HARP EFFECT device
 * to a DAW as an insert/send.
 *
 * Where shell/plugin.cpp is an INSTRUMENT (event input -> stereo out, the synth
 * refdev), this is its EFFECT sibling: a STEREO IN + STEREO OUT plugin in the
 * kFxReverb category. The track audio the DAW puts on the input bus travels
 * H->D to a HARP `audio.fx` device (e.g. harp-fx-deviced, the resonator-network
 * reverb), the device transforms it and returns the WET (processed) signal D->H,
 * and the plugin mixes that wet against the locally-held DRY (§8.8: the dry path
 * NEVER crosses the transport; the host holds it and the device returns wet only).
 *
 * It shares the SAME embedded HarpRuntime as the instrument shell, opting in to
 * the runtime's §8.8 host->device input path (setFxInputSlots / writeFxInput):
 * the runtime's host-paced feeder carries the input columns in the H->D pacing
 * payload and the reader fills the wet from the device's active-slots-out, while
 * the instrument shell (which never arms it) renders byte-identically. The
 * instrument shell, its UIDs, and its golden test are untouched — this is a
 * separate VST3 with its own factory + frozen identity.
 *
 * v1 scope (the device's verified mode is host-paced / offline-deterministic):
 *   - dry/wet "Mix" knob, default 1.0 = 100% wet (§8.8 "a 100%-wet engine + a
 *     host mix control express every ratio"). At mix=1 the dry path is inert, so
 *     the plugin is robust in every host mode.
 *   - dry/wet are SAMPLE-ALIGNED in the host-paced / offline bounce (the device
 *     returns wet for exactly the input range, lockstep), which is what an
 *     Ableton offline bounce does and what the M3 demo exercises. In a live
 *     real-time insert the wet is PDC-delay-compensated (getLatencySamples), so
 *     100% wet is a clean reverb; a sample-exact dry delay-comp for mix<1 on the
 *     free-running real-time path is a documented §8.8 follow-up.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
#include "runtime_registry.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

/* Frozen identity — its OWN class UIDs, distinct from harp-shell (so a DAW lists
 * both). NEVER change once shipped (recall/project stability). */
static const FUID kHarpFxProcessorUID(0x6F8C21A4, 0x3E5B4C90, 0xB1D74E22, 0x0A93F5C7);
static const FUID kHarpFxControllerUID(0x2D4A9E70, 0x7C1F46B8, 0x95E20D33, 0xF4681BAE);

/* The harp-fx reverb device's param bank (device/reverb_engine.c g_params): ids
 * 1..4 are the user controls; defaults MIRROR the device so recall stays sane. */
struct FxParam {
    uint32_t id;
    const char *name;
    double defaultVal;
};
static const FxParam kFxParams[] = {
    {1, "Size", 0.62},    /* room/decay (-> t60) */
    {2, "Wet", 1.00},     /* device wet send (kept 1.0; the host Mix is separate) */
    {3, "Diffuse", 0.50}, /* input diffusion density */
    {4, "Width", 0.90},   /* stereo decorrelation */
};
static constexpr int kNumFxParams = sizeof(kFxParams) / sizeof(kFxParams[0]);

/* HOST-SIDE dry/wet mix (§8.8) — NOT a device param: the device returns wet only
 * and this mixes it against the local dry. 1.0 = 100% wet (default). */
static constexpr uint32_t kMixParamId = 50;

/* The device's input slots (key 3). The harp-fx reverb reads a single MONO
 * column; a stereo-in effect would be {0,1} (and the runtime caps at 2). */
static const std::vector<uint32_t> kFxInSlots = {0};

/* ---------------- processor ---------------- */

class HarpFxProcessor : public AudioEffect {
public:
    HarpFxProcessor() { setControllerClass(kHarpFxControllerUID); }

    ~HarpFxProcessor() override {
        releaseSource();
        runtime_release(handle_);
        handle_ = RuntimeHandle{};
    }

    static FUnknown *createInstance(void *) {
        return (IAudioProcessor *)new HarpFxProcessor();
    }

    tresult PLUGIN_API initialize(FUnknown *context) override {
        tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        /* the §8.8 difference from the instrument shell: an audio INPUT bus (the
         * track signal the host routes to the device) alongside the stereo out. */
        addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        rate_ = (uint32_t)setup.sampleRate;
        maxBlock_ = (uint32_t)setup.maxSamplesPerBlock;
        offline_ = setup.processMode == kOffline;
        if (runtime() && owner()) runtime()->configure(rate_, maxBlock_);
        if (runtime()) runtime()->setOffline(offline_);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            if (handle_.rt) {
                releaseSource();
                runtime_release(handle_);
                handle_ = RuntimeHandle{};
            }
            std::string wantSerial;
            if (const char *e = getenv("HARP_DEVICE_SERIAL"); e && e[0])
                wantSerial = e;
            else if (!pendingState_.empty())
                wantSerial = HarpRuntime::bundleWantedSerial(pendingState_.data(),
                                                             pendingState_.size());
            handle_ = runtime_acquire(wantSerial);
            if (owner()) {
                runtime()->configure(rate_, maxBlock_);
                runtime()->setOffline(offline_);
                /* §8.8: arm the host->device EFFECT input BEFORE start(), so
                 * audio.start declares the in-slots (key 3) and the feeder carries
                 * the track audio in the H->D payload. The instrument shell never
                 * calls this, so its wire stays byte-identical (the golden gate). */
                runtime()->setFxInputSlots(kFxInSlots);
                if (!pendingState_.empty())
                    runtime()->setStateBundle(pendingState_.data(), pendingState_.size());
                runtime()->start(rate_);
                source_ = runtime()->ownerSource();
            }
            /* (A second FX instance with no pinned serial gets its OWN runtime and
             * is its own owner; for v1 we expect a single insert per device.) */
        } else {
            releaseSource();
            runtime_release(handle_);
            handle_ = RuntimeHandle{};
        }
        return AudioEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() override {
        /* §8.8 PDC. Host-paced/offline is LOCKSTEP — the device returns wet for
         * exactly the input range this process() supplied, so the added latency is
         * ~0 and dry/wet are sample-aligned. A live real-time insert reads the wet
         * a pipeline-depth behind, so report the runtime's path latency there for
         * the host to delay-compensate the wet. */
        if (offline_) return 0;
        if (runtime()) return runtime()->latencySamples();
        return HarpRuntime::latencyFor(maxBlock_);
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    tresult silenceOut(ProcessData &data) {
        if (data.numOutputs < 1 || data.numSamples <= 0 ||
            data.outputs[0].numChannels < 1 || !data.outputs[0].channelBuffers32)
            return kResultOk;
        int32 nch = data.outputs[0].numChannels;
        for (int32 c = 0; c < nch; c++)
            if (float *ch = data.outputs[0].channelBuffers32[c])
                memset(ch, 0, (size_t)data.numSamples * sizeof(float));
        data.outputs[0].silenceFlags = (nch >= 64) ? ~0ull : ((1ull << nch) - 1);
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData &data) override {
        if (!runtime() || !source_) return silenceOut(data);
        HarpRuntime &rt = *runtime();
        uint64_t base = rt.streamPos() + rt.latencySamples();

        /* parameter changes: device params 1..4 -> §9.4 timestamped sets; the
         * host-side Mix (id 50) updates the local dry/wet ratio (never sent). */
        if (data.inputParameterChanges) {
            int32 nq = data.inputParameterChanges->getParameterCount();
            for (int32 i = 0; i < nq; i++) {
                IParamValueQueue *q = data.inputParameterChanges->getParameterData(i);
                if (!q) continue;
                uint32_t id = (uint32_t)q->getParameterId();
                int32 np = q->getPointCount();
                for (int32 k = 0; k < np; k++) {
                    int32 off;
                    ParamValue v;
                    if (q->getPoint(k, off, v) != kResultOk) continue;
                    if (id == kMixParamId) {
                        mix_ = (float)v;
                        continue;
                    }
                    if (id >= 1 && id <= (uint32_t)kNumFxParams)
                        rt.queueParamSet(source_, id, (float)v, base + (uint64_t)off);
                }
            }
        }

        int32 n = data.numSamples;
        if (n <= 0) return kResultOk;
        if (data.numOutputs < 1 || data.outputs[0].numChannels < 1 ||
            !data.outputs[0].channelBuffers32)
            return kResultOk;

        /* INPUT: the track signal on the input bus -> MONO (the device reads one
         * column) -> the runtime's H->D effect input. Keep a local stereo DRY copy
         * for the mix (the dry NEVER crosses the transport, §8.8). */
        const float *inL = nullptr, *inR = nullptr;
        if (data.numInputs >= 1 && data.inputs[0].channelBuffers32 &&
            data.inputs[0].numChannels >= 1) {
            inL = data.inputs[0].channelBuffers32[0];
            inR = data.inputs[0].numChannels > 1 ? data.inputs[0].channelBuffers32[1] : inL;
        }
        static thread_local std::vector<float> mono;
        if ((int)mono.size() < n) mono.resize(n);
        for (int32 s = 0; s < n; s++)
            mono[s] = inL ? 0.5f * (inL[s] + inR[s]) : 0.0f;
        rt.writeFxInput(mono.data(), (size_t)n);

        /* WET: pull the device's processed stereo return. Offline blocks for the
         * lockstep wet (deterministic bounce); real-time pads silence on underrun. */
        static thread_local std::vector<float> wet;
        if ((int)wet.size() < 2 * n) wet.resize(2 * n);
        if (offline_)
            rt.pullAudioBlocking(wet.data(), (size_t)n, 1000);
        else
            rt.pullAudio(wet.data(), (size_t)n);

        /* MIX: out = mix*wet + (1-mix)*dry. In the host-paced/offline path wet[s]
         * is the device's processing of the SAME input[s] this block supplied, so
         * dry and wet are sample-aligned. */
        float mix = mix_ < 0.f ? 0.f : (mix_ > 1.f ? 1.f : mix_);
        int32 nch = data.outputs[0].numChannels;
        float *outL = data.outputs[0].channelBuffers32[0];
        float *outR = nch > 1 ? data.outputs[0].channelBuffers32[1] : nullptr;
        for (int32 s = 0; s < n; s++) {
            float dryL = inL ? inL[s] : 0.0f;
            float dryR = inR ? inR[s] : 0.0f;
            float wL = wet[2 * s], wR = wet[2 * s + 1];
            float l = mix * wL + (1.0f - mix) * dryL;
            float r = mix * wR + (1.0f - mix) * dryR;
            outL[s] = l;
            if (outR) outR[s] = r;
            else outL[s] = 0.5f * (l + r); /* mono host: sum */
        }
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

    /* component state = Recall Bundle (§15.3) — the device's param bank. No P6
     * part header (the effect is not multitimbral). */
    tresult PLUGIN_API getState(IBStream *state) override {
        if (!runtime()) return kResultFalse;
        std::vector<uint8_t> bundle;
        if (!runtime()->getStateBundle(bundle)) return kResultFalse;
        int32 written = 0;
        return state->write(bundle.data(), (int32)bundle.size(), &written);
    }

    tresult PLUGIN_API setState(IBStream *state) override {
        std::vector<uint8_t> raw;
        uint8_t buf[8192];
        int32 got = 0;
        while (state->read(buf, sizeof buf, &got) == kResultOk && got > 0) {
            raw.insert(raw.end(), buf, buf + got);
            if (got < (int32)sizeof buf) break;
        }
        if (raw.empty()) return kResultFalse;
        pendingState_ = raw;
        if (owner() && runtime())
            return runtime()->setStateBundle(raw.data(), raw.size()) ? kResultOk : kResultFalse;
        return kResultOk;
    }

private:
    RuntimeHandle handle_;
    EventSource *source_ = nullptr;
    void releaseSource() {
        if (source_ && runtime()) runtime()->unregisterSource(source_);
        source_ = nullptr;
    }
    HarpRuntime *runtime() const { return handle_.rt; }
    bool owner() const { return handle_.owner; }
    uint32_t rate_ = 48000;
    uint32_t maxBlock_ = 1024;
    bool offline_ = false;
    float mix_ = 1.0f; /* §8.8 host dry/wet; 1.0 = 100% wet */
    std::vector<uint8_t> pendingState_;
};

/* ---------------- controller ---------------- */

class HarpFxController : public EditController {
public:
    static FUnknown *createInstance(void *) {
        return (IEditController *)new HarpFxController();
    }

    tresult PLUGIN_API initialize(FUnknown *context) override {
        tresult r = EditController::initialize(context);
        if (r != kResultOk) return r;
        for (auto &p : kFxParams) {
            UString256 title(p.name);
            parameters.addParameter(title, nullptr, 0, p.defaultVal,
                                    ParameterInfo::kCanAutomate, p.id);
        }
        parameters.addParameter(STR16("Mix"), STR16("%"), 0, 1.0,
                                ParameterInfo::kCanAutomate, kMixParamId);
        return kResultOk;
    }
};

/* ---------------- factory ---------------- */

#define stringFxName "HARP FX"

BEGIN_FACTORY_DEF("HARP Project", "https://github.com/kschzt/harp",
                  "mailto:harp@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(kHarpFxProcessorUID), PClassInfo::kManyInstances,
           kVstAudioEffectClass, stringFxName, Vst::kDistributable,
           Vst::PlugType::kFxReverb, "0.1.0", kVstVersionString,
           HarpFxProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(kHarpFxControllerUID), PClassInfo::kManyInstances,
           kVstComponentControllerClass, stringFxName " Controller", 0, "", "0.1.0",
           kVstVersionString, HarpFxController::createInstance)

END_FACTORY
