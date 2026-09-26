/* HARP FX — VST3 shell variant (§8.8 audio.fx): presents a HARP EFFECT device
 * to a DAW as an insert/send.
 *
 * Where shell/plugin.cpp is an INSTRUMENT (event input -> stereo out, the synth
 * refdev), this is its EFFECT sibling: a STEREO IN + STEREO OUT plugin. The track
 * audio the DAW puts on the input bus travels H->D to a HARP `audio.fx` device
 * (e.g. examples/fx-filter), the device transforms it and returns the WET
 * (processed) signal D->H, and the plugin mixes that wet against the locally-held
 * DRY (§8.8: the dry path NEVER crosses the transport; the host holds it and the
 * device returns wet only).
 *
 * Identity, param map and input slots come from shell_config.h (HARP_FX_SHELL_*;
 * default = the examples/fx-filter device), so an effect product ships its own
 * plugin from these sources without editing them. Automation and recall behave as
 * in the instrument shell: DAW curves become §9.4 ramps (shared ParamAutomation),
 * device front-panel moves echo back as automation (§9.4 echo), edits made while
 * the device was offline replay on reconnect (§15.5), and the component state is
 * the §15.3 recall bundle.
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
 *   - the wet trails its input by a CONSTANT, enforced delay — the runtime's
 *     fxLatencySamples(), latched per activation: one DAW block (the automation horizon:
 *     a block's automation ramps from the previous point, so its audio is paced only after
 *     the next block's events) + 255 (a pacing frame) offline, plus the ring target live,
 *     plus the device's content pipeline (§8.8 device-pipeline-samples) when it is
 *     connected at activation. The plugin reports exactly that for PDC; its dry follows
 *     the runtime's actual wet delay, so dry and wet are sample-aligned at every Mix.
 */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
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

#include "param_automation.h"
#include "runtime.h"
#include "runtime_registry.h"
#include "shell_config.h" /* HARP_FX_SHELL_* identity/params/in-slots (default = examples/fx-filter) */

using namespace Steinberg;
using namespace Steinberg::Vst;

/* Frozen identity — its OWN class UIDs, distinct from harp-shell (so a DAW lists
 * both). NEVER change a shipped product's UIDs (recall/project stability). */
static const FUID kHarpFxProcessorUID(HARP_FX_SHELL_PROC_FUID);
static const FUID kHarpFxControllerUID(HARP_FX_SHELL_CTRL_FUID);

/* The effect device's param table (HARP_FX_SHELL_PARAMS) — same shape as the
 * instrument shell's DevParam. Ids + defaults MIRROR the device so automation lands
 * on the right param and recall stays sane. */
struct FxParam {
    uint32_t id;
    const char *name;
    int32 stepCount;    /* 0 = continuous (VST3: stepCount = steps - 1) */
    double defaultVal;
    const char *labels; /* nullptr, or "A|B|C" enum labels (stepCount+1 of them) */
};
static constexpr FxParam kFxParams[] = {HARP_FX_SHELL_PARAMS};
static constexpr int kNumFxParams = sizeof(kFxParams) / sizeof(kFxParams[0]);

/* HOST-SIDE dry/wet mix (§8.8) — NOT a device param: the device returns wet only
 * and this mixes it against the local dry. 1.0 = 100% wet (default). */
static constexpr uint32_t kMixParamId = 50;
static constexpr bool fxParamIdsClear(int i = 0) {
    return i == kNumFxParams || (kFxParams[i].id != kMixParamId && fxParamIdsClear(i + 1));
}
static_assert(fxParamIdsClear(), "a device param id collides with the host Mix param (50)");

/* The device's input slots (audio.start key 3): {0,1} = stereo, {0} = mono. */
static const std::vector<uint32_t> kFxInSlots = {HARP_FX_SHELL_IN_SLOTS};

/* Component state: 'H','F','1' + the host Mix (float32, little-endian) + the §15.3
 * recall bundle. The device's params live in the bundle; the Mix is host-side, so the
 * shell carries it (without it a reopened project came back 100% wet). 'H' (0x48) can
 * never start a recall bundle (a CBOR map), so a header-less state from an older build
 * is detected and loads as a bare bundle with the default Mix. */
static const uint8_t kFxStateMagic[3] = {'H', 'F', '1'};
static constexpr size_t kFxStateHeaderLen = sizeof kFxStateMagic + 4;

static std::vector<uint8_t> readStream(IBStream *state) {
    std::vector<uint8_t> raw;
    uint8_t buf[8192];
    int32 got = 0;
    while (state && state->read(buf, sizeof buf, &got) == kResultOk && got > 0) {
        raw.insert(raw.end(), buf, buf + got);
        if (got < (int32)sizeof buf) break;
    }
    return raw;
}
/* split a component state into (mix, bundle); false if it has no bundle */
static bool fxStateDecode(const std::vector<uint8_t> &raw, float &mix, std::vector<uint8_t> &bundle) {
    mix = 1.0f;
    if (raw.size() >= kFxStateHeaderLen && memcmp(raw.data(), kFxStateMagic, sizeof kFxStateMagic) == 0) {
        const uint8_t *m = raw.data() + sizeof kFxStateMagic;
        uint32_t bits = (uint32_t)m[0] | (uint32_t)m[1] << 8 | (uint32_t)m[2] << 16 | (uint32_t)m[3] << 24;
        memcpy(&mix, &bits, sizeof mix);
        if (!(mix >= 0.0f && mix <= 1.0f)) mix = 1.0f; /* corrupt/NaN -> the safe default */
        bundle.assign(raw.begin() + kFxStateHeaderLen, raw.end());
    } else
        bundle = raw;
    return !bundle.empty();
}

/* ---------------- processor ---------------- */

class HarpFxProcessor : public AudioEffect {
public:
    HarpFxProcessor() {
        setControllerClass(kHarpFxControllerUID);
        uint32_t ids[kNumFxParams];
        float defs[kNumFxParams];
        for (int i = 0; i < kNumFxParams; i++) {
            ids[i] = kFxParams[i].id;
            defs[i] = (float)kFxParams[i].defaultVal;
        }
        automation_.init(ids, defs, kNumFxParams);
    }

    ~HarpFxProcessor() override {
        releaseSource();
        rt_.reset();
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
        /* 3 ms one-pole for the Mix de-click */
        mixCoef_ = 1.0f - std::exp(-1.0f / (0.003f * (float)(rate_ ? rate_ : 48000)));
        if (runtime()) runtime()->configure(rate_, maxBlock_);
        if (runtime()) runtime()->setOffline(offline_);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            if (rt_) {
                releaseSource();
                rt_.reset();
            }
            /* §8.8 OWNERSHIP — an FX insert is PER-TRACK: each instance drives its
             * OWN private host-paced session THROUGH the device (H->D track audio
             * in, D->H wet). Every instance owns its runtime outright (the
             * share-by-serial registry is retired), so the Ableton "non-owner
             * insert renders silence" failure is structurally impossible: there is
             * no shared runtime to attach to as a non-owner. Device binding is the
             * runtime's own job: selectDevice() dials HARP_ETH_DEVICE (or the USB
             * serial); the §12.2 device-identity gate is preserved because
             * setStateBundle() below records the bundle's wanted serial
             * (wantSerial_) for the serial-differs read-only hold. */
            rt_ = runtime_acquire();
            runtime()->configure(rate_, maxBlock_);
            runtime()->setOffline(offline_);
            /* §8.8: arm the host->device EFFECT input BEFORE start(), so
             * audio.start declares the in-slots (key 3) and the feeder carries
             * the track audio in the H->D payload. The instrument shell never
             * calls this, so its wire stays byte-identical (the golden gate).
             * Arming also FORCES host-paced (wantHostPacedMode()), so an armed FX
             * is never free-running — it returns the wet in live playback too. */
            runtime()->setFxInputSlots(kFxInSlots);
            if (!pendingState_.empty())
                runtime()->setStateBundle(pendingState_.data(), pendingState_.size());
            runtime()->start(rate_);
            source_ = runtime()->ownerSource();
            /* audio-thread buffers, allocated here — never on the audio thread. The dry
             * line holds kDryMaxFrames: it follows the runtime's ACTUAL wet delay every
             * block (fxWetDelay), which can differ from the value sized at activation (a
             * device profile learned at connect, a late-guard re-anchor). */
            fxin_.assign(2 * (size_t)maxBlock_, 0.0f);
            wet_.assign(2 * (size_t)maxBlock_, 0.0f);
            dryBuf_.assign(2 * (size_t)kDryMaxFrames, 0.0f);
            dryW_ = 0;
            dryD_ = dryPrevD_ = runtime()->fxWetDelay();
            dryXfade_ = 0;
            dryPrimed_ = false;
            mixLin_ = mixSm_ = mixTarget_.load(std::memory_order_relaxed);
            mixFresh_ = true; /* the first block STARTS at its first Mix value (no glide) */
        } else {
            /* §14.4 host-context-A capture, OPT-IN by env (harp-vst3-host --diag-bundle),
             * exactly as the instrument shell: read-only, after the render, while the
             * session is still up. Unset env = no-op. */
            if (const char *p = getenv("HARP_DIAG_BUNDLE_OUT"); p && p[0] && runtime()) {
                const char *a = getenv("HARP_DIAG_BUNDLE_ANON");
                std::vector<uint8_t> db = runtime()->getDiagBundle(a && a[0] && a[0] != '0');
                if (FILE *f = fopen(p, "wb")) {
                    if (!db.empty()) fwrite(db.data(), 1, db.size(), f);
                    fclose(f);
                }
            }
            releaseSource();
            rt_.reset();
        }
        return AudioEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() override {
        /* §8.8 PDC: the runtime ENFORCES this delay between the input and its wet
         * (fxLatencySamples), so what the host compensates is what it gets. */
        if (runtime()) return runtime()->fxLatencySamples();
        return offline_ ? HarpRuntime::fxOfflineLatencyFor(maxBlock_) : HarpRuntime::fxLiveLatencyFor(maxBlock_);
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

    /* DEFENSIVE (§8.8): no event source means this instance never armed a session —
     * which an FX must never do now that it always owns a fresh runtime (see
     * setActive). If it ever did happen, pass the DRY track signal straight through
     * (or silence if there is none) rather than killing the track: a stray insert
     * then degrades to clean dry audio, never the silent-track bug. (A connected-but-
     * disconnected device is NOT this case — source_ is set there, so a dead port
     * still correctly yields silence via the normal wet=0 path below.) */
    tresult passthroughDry(ProcessData &data) {
        if (data.numOutputs < 1 || data.numSamples <= 0 ||
            data.outputs[0].numChannels < 1 || !data.outputs[0].channelBuffers32)
            return kResultOk;
        int32 n = data.numSamples, nch = data.outputs[0].numChannels;
        const float *inL = nullptr, *inR = nullptr;
        if (data.numInputs >= 1 && data.inputs[0].channelBuffers32 &&
            data.inputs[0].numChannels >= 1) {
            inL = data.inputs[0].channelBuffers32[0];
            inR = data.inputs[0].numChannels > 1 ? data.inputs[0].channelBuffers32[1] : inL;
        }
        if (!inL) return silenceOut(data);
        float *outL = data.outputs[0].channelBuffers32[0];
        float *outR = nch > 1 ? data.outputs[0].channelBuffers32[1] : nullptr;
        for (int32 s = 0; s < n; s++) {
            if (outR) { outL[s] = inL[s]; outR[s] = inR[s]; }
            else outL[s] = 0.5f * (inL[s] + inR[s]);
        }
        data.outputs[0].silenceFlags = 0;
        return kResultOk;
    }

    tresult PLUGIN_API process(ProcessData &data) override {
        if (!runtime() || !source_) return passthroughDry(data);
        HarpRuntime &rt = *runtime();
        /* Adopt a new session's SSI domain FIRST (a reconnect or late connect happened on
         * the supervisor thread): from here on this block's input, events and pull all
         * belong to it. */
        rt.fxBeginBlock();
        /* Stream position of THIS block's input (§9.2): its events must land on the
         * same SSI as the audio they accompany, and the runtime knows that SSI exactly
         * (fxInputPos). (The instrument shell leads by latencySamples() because its
         * audio is RENDERED ahead on the device; an effect's audio is the host's input.) */
        uint64_t base = rt.fxInputPos();

        /* parameter changes: device params -> §9.4 sets/ramps (ParamAutomation, the
         * instrument shell's policy); the host-side Mix updates the local dry/wet
         * ratio and is never sent. */
        automation_.beginBlock(rt, source_, base);
        nMixPts_ = 0;
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
                    if (id == kMixParamId) { /* host-side: sample-accurate, applied below */
                        if (nMixPts_ < kMaxMixPts) mixPts_[nMixPts_++] = {off, (float)v};
                        else mixPts_[kMaxMixPts - 1] = {off, (float)v};
                        continue;
                    }
                    size_t slot = automation_.slotOf(id);
                    if (slot != SIZE_MAX)
                        automation_.point(rt, source_, slot, (float)v, base + (uint64_t)off);
                }
            }
        }

        int32 n = data.numSamples;
        if (n <= 0) return kResultOk;
        if (data.numOutputs < 1 || data.outputs[0].numChannels < 1 ||
            !data.outputs[0].channelBuffers32)
            return kResultOk;

        /* INPUT: the track signal on the input bus -> the device's input columns
         * (stereo L/R interleaved, or summed to mono for a one-slot device) -> the
         * runtime's H->D effect input. The dry stays local for the mix (the dry
         * NEVER crosses the transport, §8.8). */
        const float *inL = nullptr, *inR = nullptr;
        if (data.numInputs >= 1 && data.inputs[0].channelBuffers32 &&
            data.inputs[0].numChannels >= 1) {
            inL = data.inputs[0].channelBuffers32[0];
            inR = data.inputs[0].numChannels > 1 ? data.inputs[0].channelBuffers32[1] : inL;
        }
        const size_t ncols = kFxInSlots.size() >= 2 ? 2 : 1;
        if (fxin_.size() < 2 * (size_t)n) { /* host broke maxSamplesPerBlock: survive it */
            fxin_.resize(2 * (size_t)n);
            wet_.resize(2 * (size_t)n);
        }
        for (int32 s = 0; s < n; s++) {
            float l = inL ? inL[s] : 0.0f, r = inR ? inR[s] : 0.0f;
            if (ncols == 2) {
                fxin_[2 * (size_t)s] = l;
                fxin_[2 * (size_t)s + 1] = r;
            } else
                fxin_[(size_t)s] = 0.5f * (l + r);
        }
        rt.writeFxInput(fxin_.data(), (size_t)n);

        /* WET: pull the device's processed stereo return, fxLatencySamples() behind
         * its input. Offline blocks until it has arrived (deterministic bounce);
         * real-time pads silence on underrun (and drops the late wet, keeping the delay). */
        /* Offline waits as long as the device is CONNECTED — a bounce has no real-time
         * deadline, and giving up early would pad silence into a render that must be
         * deterministic (a stalled host or device for 0.5 s used to do exactly that). The
         * bound (20000 polls x 0.5 ms = 10 s) only catches a connected-but-wedged device;
         * a disconnect ends the wait at once. */
        if (offline_)
            rt.pullAudioBlocking(wet_.data(), (size_t)n, 20000);
        else
            rt.pullAudio(wet_.data(), (size_t)n);

        /* MIX: out = mix*wet + (1-mix)*dry, per sample.
         * - The wet trails its input by the runtime's actual wet delay (fxWetDelay), so the
         *   dry is read that far back in dryBuf_ and the two stay sample-aligned — also
         *   after a reconnect or a late-guard re-anchor changes the delay.
         * - Mix automation is sample-accurate: linear between this block's points (VST3
         *   point semantics), from the previous block's value; with no points it heads for
         *   mixTarget_ (a UI edit or a restored state). A 3 ms one-pole de-clicks steps. */
        uint32_t d = rt.fxWetDelay(); /* <= kFxMaxWetDelay < kDryMaxFrames (the runtime caps it) */
        if (d >= kDryMaxFrames) d = kDryMaxFrames - 1;
        if (d != dryD_) { /* the wet's delay changed (new session, re-anchor): crossfade the */
            dryPrevD_ = dryD_; /* dry's read point over kDryXfade samples instead of jumping */
            dryD_ = d;
            dryXfade_ = dryPrimed_ ? kDryXfade : 0; /* nothing played yet: just take it */
        }
        dryPrimed_ = true;
        const bool mixAutomated = nMixPts_ > 0;
        if (!mixAutomated) mixPts_[nMixPts_++] = {0, mixTarget_.load(std::memory_order_relaxed)};
        if (mixFresh_) { /* a render's first block starts at the value the host gave it, not a
                          * glide from the default — so how the Mix arrived (automation, a
                          * restored state) cannot change the bounce */
            mixLin_ = mixSm_ = mixPts_[0].off == 0 ? mixPts_[0].v : mixLin_;
            mixFresh_ = false;
        }
        int32 nch = data.outputs[0].numChannels;
        float *outL = data.outputs[0].channelBuffers32[0];
        float *outR = nch > 1 ? data.outputs[0].channelBuffers32[1] : nullptr;
        float segV0 = mixLin_;
        int32 segS0 = 0;
        uint32_t pi = 0;
        for (int32 s = 0; s < n; s++) {
            while (pi < nMixPts_ && mixPts_[pi].off < s) { /* passed: next segment */
                segV0 = mixPts_[pi].v;
                segS0 = mixPts_[pi].off;
                pi++;
            }
            float lin = segV0;
            if (pi < nMixPts_) {
                int32 span = mixPts_[pi].off - segS0;
                lin = span > 0 ? segV0 + (mixPts_[pi].v - segV0) * (float)(s - segS0) / (float)span
                               : mixPts_[pi].v;
            }
            lin = lin < 0.f ? 0.f : (lin > 1.f ? 1.f : lin);
            mixSm_ += (lin - mixSm_) * mixCoef_;
            size_t w = dryW_, r = (dryW_ - dryD_) & (kDryMaxFrames - 1);
            dryBuf_[2 * w] = inL ? inL[s] : 0.0f;
            dryBuf_[2 * w + 1] = inR ? inR[s] : 0.0f;
            float dryL = dryBuf_[2 * r], dryR = dryBuf_[2 * r + 1];
            if (dryXfade_) {
                size_t r0 = (dryW_ - dryPrevD_) & (kDryMaxFrames - 1);
                float a = (float)dryXfade_-- / (float)kDryXfade; /* weight of the old read point */
                dryL = a * dryBuf_[2 * r0] + (1.0f - a) * dryL;
                dryR = a * dryBuf_[2 * r0 + 1] + (1.0f - a) * dryR;
            }
            dryW_ = (dryW_ + 1) & (kDryMaxFrames - 1);
            float l = mixSm_ * wet_[2 * (size_t)s] + (1.0f - mixSm_) * dryL;
            float rr = mixSm_ * wet_[2 * (size_t)s + 1] + (1.0f - mixSm_) * dryR;
            outL[s] = l;
            if (outR) outR[s] = rr;
            else outL[s] = 0.5f * (l + rr); /* mono host: sum */
        }
        mixLin_ = mixPts_[nMixPts_ - 1].v; /* the curve ends at its last point */
        /* publish automation back (getState must save it) — but ONLY when this block had
         * real Mix points: otherwise a setState/preset landing mid-block would be overwritten
         * with the value this block started from */
        if (mixAutomated) mixTarget_.store(mixLin_, std::memory_order_relaxed);
        data.outputs[0].silenceFlags = 0;
        /* device front-panel echoes (§9.4) -> output parameter changes, so a knob
         * turned on the hardware moves (and records into) the DAW's parameter. An
         * effect is one part: drain part 0's echo ring, forwarding only this
         * plugin's params (the device also echoes its readonly meters that way). */
        {
            uint32_t id;
            float v;
            while (rt.popEcho(0, id, v)) {
                if (!data.outputParameterChanges || automation_.slotOf(id) == SIZE_MAX) continue;
                int32 qi = 0;
                if (IParamValueQueue *q = data.outputParameterChanges->addParameterData(id, qi)) {
                    int32 pi = 0;
                    q->addPoint(0, v, pi);
                }
            }
        }
        /* §8.8 NEVER-SILENT guard: the runtime's wet-side watchdog (observeFxWet, run
         * inside pullAudio/pullAudioBlocking) trips when this armed FX has been fed live
         * input but the device returned silence for a full window — the H->D input path
         * is dead ("the §8.8 trap"). On the OFFLINE/host-paced bounce that is a HARD
         * failure: fail the render LOUDLY (non-OK process status -> the host/CLI bounce
         * exits non-zero, e.g. harp-vst3-host --fx-instances die()s) rather than write a
         * silent file. A live insert keeps running — the ERROR log + the host-readable
         * x.harp.fx_silent_wet counter surface it without killing the DAW. */
        if (offline_ && rt.fxSilentWetTripped()) return kResultFalse;
        return kResultOk;
    }

    /* component state = kFxStateMagic + host Mix + Recall Bundle (§15.3, the
     * device's param bank). No P6 part byte (the effect is not multitimbral). */
    tresult PLUGIN_API getState(IBStream *state) override {
        if (!runtime()) return kResultFalse;
        std::vector<uint8_t> bundle;
        if (!runtime()->getStateBundle(bundle)) return kResultFalse;
        uint8_t header[kFxStateHeaderLen];
        memcpy(header, kFxStateMagic, sizeof kFxStateMagic);
        uint32_t bits;
        const float mix = mixTarget_.load(std::memory_order_relaxed);
        memcpy(&bits, &mix, sizeof bits);
        for (int i = 0; i < 4; i++) header[sizeof kFxStateMagic + i] = (uint8_t)(bits >> (8 * i));
        int32 written = 0;
        if (state->write(header, (int32)sizeof header, &written) != kResultOk) return kResultFalse;
        return state->write(bundle.data(), (int32)bundle.size(), &written);
    }

    tresult PLUGIN_API setState(IBStream *state) override {
        std::vector<uint8_t> bundle;
        float mix;
        if (!fxStateDecode(readStream(state), mix, bundle)) return kResultFalse;
        mixTarget_.store(mix, std::memory_order_relaxed); /* the audio thread glides to it */
        pendingState_ = bundle;
        if (runtime())
            return runtime()->setStateBundle(bundle.data(), bundle.size()) ? kResultOk : kResultFalse;
        return kResultOk;
    }

private:
    std::unique_ptr<HarpRuntime> rt_;
    EventSource *source_ = nullptr;
    void releaseSource() {
        /* source_ is the runtime's own owner source — freed with the runtime; just
         * forget it here (run before rt_.reset()). */
        source_ = nullptr;
    }
    HarpRuntime *runtime() const { return rt_.get(); }
    uint32_t rate_ = 48000;
    uint32_t maxBlock_ = 1024;
    bool offline_ = false;
    /* §8.8 host dry/wet (1.0 = 100% wet). mixTarget_ crosses threads (getState/setState
     * on the host's threads, process() on the audio thread); the rest is audio-thread. */
    std::atomic<float> mixTarget_{1.0f};
    float mixLin_ = 1.0f, mixSm_ = 1.0f, mixCoef_ = 0.0f;
    bool mixFresh_ = true;
    struct MixPt { int32 off; float v; };
    static constexpr uint32_t kMaxMixPts = 64;
    MixPt mixPts_[kMaxMixPts];
    uint32_t nMixPts_ = 0;
    /* audio-thread buffers, sized in setActive: device input columns, the wet, and the
     * dry delay line (a power of two, so the read index wraps by mask) */
    static constexpr size_t kDryMaxFrames = 1u << 16;
    std::vector<float> fxin_, wet_, dryBuf_;
    size_t dryW_ = 0;
    uint32_t dryD_ = 0, dryPrevD_ = 0, dryXfade_ = 0; /* current / previous read delay, fade left */
    bool dryPrimed_ = false;                           /* a block has been played this activation */
    static constexpr uint32_t kDryXfade = 64;
    static_assert(HarpRuntime::kFxMaxWetDelay < kDryMaxFrames, "the dry line must hold the longest wet delay");
    /* DAW automation -> §9.4 set/ramp events + §15.5 offline-edit replay (shared
     * with the instrument shell). Seeded from kFxParams in the constructor. */
    ParamAutomation automation_;
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
            if (p.labels) { /* a NAMED picker: register the enum labels so the DAW shows them */
                auto *sl = new StringListParameter(UString256(p.name), p.id);
                for (const char *s = p.labels; *s;) {
                    const char *e = strchr(s, '|');
                    size_t len = e ? (size_t)(e - s) : strlen(s);
                    char buf[64];
                    if (len >= sizeof buf) len = sizeof buf - 1;
                    memcpy(buf, s, len);
                    buf[len] = 0;
                    sl->appendString(UString256(buf));
                    if (!e) break;
                    s = e + 1;
                }
                parameters.addParameter(sl);
                continue;
            }
            parameters.addParameter(UString256(p.name), nullptr, p.stepCount, p.defaultVal,
                                    ParameterInfo::kCanAutomate, p.id);
        }
        parameters.addParameter(STR16("Mix"), STR16("%"), 0, 1.0,
                                ParameterInfo::kCanAutomate, kMixParamId);
        return kResultOk;
    }

    /* project reopen: show the restored host Mix (the device params are restored on
     * the device itself, from the bundle) */
    tresult PLUGIN_API setComponentState(IBStream *state) override {
        std::vector<uint8_t> bundle;
        float mix;
        if (!fxStateDecode(readStream(state), mix, bundle)) return kResultFalse;
        setParamNormalized(kMixParamId, mix);
        return kResultOk;
    }
};

/* ---------------- factory ---------------- */

#define stringFxName HARP_FX_SHELL_PLUGIN_NAME

BEGIN_FACTORY_DEF("HARP Project", "https://github.com/kschzt/harp",
                  "mailto:harp@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(kHarpFxProcessorUID), PClassInfo::kManyInstances,
           kVstAudioEffectClass, stringFxName, Vst::kDistributable,
           HARP_FX_SHELL_CATEGORY, "0.1.0", kVstVersionString,
           HarpFxProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(kHarpFxControllerUID), PClassInfo::kManyInstances,
           kVstComponentControllerClass, stringFxName " Controller", 0, "", "0.1.0",
           kVstVersionString, HarpFxController::createInstance)

END_FACTORY
