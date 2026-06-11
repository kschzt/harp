/* harp-vst3-host — a minimal CLI VST3 host for automated testing.
 *
 * Loads a .vst3 bundle, drives it the way a DAW would — bus activation,
 * parameter changes, block processing, state save/restore — entirely from
 * the command line, and writes the rendered audio to WAV for numeric
 * inspection. Exists so the HARP shell can be exercised and verified by
 * an agent with no DAW and no GUI in the loop.
 *
 *   harp-vst3-host PLUGIN.vst3 --list
 *   harp-vst3-host PLUGIN.vst3 --set 100=0.25 --seconds 2 --input sine:440 \
 *                  --out out.wav [--hash] [--save-state f] [--load-state f]
 */

#include <cmath>
#include <cstdint>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "base/source/fobject.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

static int die(const std::string &msg) {
    fprintf(stderr, "harp-vst3-host: %s\n", msg.c_str());
    exit(1);
}

/* ---- WAV (PCM16 stereo) ---- */
static void write_wav16(const std::string &path, const std::vector<float> &interleaved,
                        uint32_t channels, uint32_t rate) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) die("cannot write " + path);
    uint32_t nsamp = (uint32_t)(interleaved.size());
    uint32_t data_len = nsamp * 2;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t riff = 36 + data_len;
    memcpy(hdr + 4, &riff, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16;
    uint16_t pcm = 1, bits = 16;
    uint16_t align = (uint16_t)(channels * 2);
    uint32_t byterate = rate * align;
    memcpy(hdr + 16, &fmtlen, 4);
    memcpy(hdr + 20, &pcm, 2);
    memcpy(hdr + 22, &channels, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byterate, 4);
    memcpy(hdr + 32, &align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_len, 4);
    fwrite(hdr, 1, 44, f);
    for (float v : interleaved) {
        if (v > 1.f) v = 1.f;
        if (v < -1.f) v = -1.f;
        int16_t s = (int16_t)(v * 32767.f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static uint64_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ---- state container: [u32 len][component][u32 len][controller] ---- */
static void save_state_file(const std::string &path, MemoryStream &comp, MemoryStream &ctrl) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) die("cannot write state file");
    uint32_t a = (uint32_t)comp.getSize(), b = (uint32_t)ctrl.getSize();
    fwrite(&a, 4, 1, f);
    fwrite(comp.getData(), 1, a, f);
    fwrite(&b, 4, 1, f);
    fwrite(ctrl.getData(), 1, b, f);
    fclose(f);
}

static bool load_state_file(const std::string &path, std::vector<char> &comp,
                            std::vector<char> &ctrl) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t a = 0, b = 0;
    if (fread(&a, 4, 1, f) != 1) goto fail;
    comp.resize(a);
    if (a && fread(comp.data(), 1, a, f) != a) goto fail;
    if (fread(&b, 4, 1, f) != 1) goto fail;
    ctrl.resize(b);
    if (b && fread(ctrl.data(), 1, b, f) != b) goto fail;
    fclose(f);
    return true;
fail:
    fclose(f);
    return false;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: harp-vst3-host PLUGIN.vst3 [--list] [--rate N] [--block N]\n"
                "       [--seconds S] [--input sine[:HZ]|impulse|silence]\n"
                "       [--set ID=NORMVALUE]... [--ramp ID=V0:V1]... [--notes N,N,..]\n"
                "       [--realtime] [--out FILE.wav] [--hash]\n"
                "       [--save-state FILE] [--load-state FILE]\n");
        return 2;
    }
    std::string plugin_path = argv[1];
    bool do_list = false, do_hash = false, realtime = false;
    uint32_t rate = 48000, block = 256;
    double seconds = 2.0;
    std::string input_kind = "silence", out_path, save_state_path, load_state_path;
    double sine_hz = 440.0;
    std::vector<std::pair<uint32_t, double>> sets;
    std::vector<int> notes; /* played sequentially: on at i*0.6s, off at +0.45s */
    struct RampSpec {
        uint32_t id;
        double v0, v1;
    };
    std::vector<RampSpec> ramps; /* one automation point per block, linear v0->v1 */

    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value for " + a);
            return argv[++i];
        };
        if (a == "--list") do_list = true;
        else if (a == "--hash") do_hash = true;
        else if (a == "--realtime") realtime = true;
        else if (a == "--rate") rate = (uint32_t)atoi(next().c_str());
        else if (a == "--block") block = (uint32_t)atoi(next().c_str());
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--out") out_path = next();
        else if (a == "--save-state") save_state_path = next();
        else if (a == "--load-state") load_state_path = next();
        else if (a == "--input") {
            input_kind = next();
            auto colon = input_kind.find(':');
            if (colon != std::string::npos) {
                sine_hz = atof(input_kind.substr(colon + 1).c_str());
                input_kind = input_kind.substr(0, colon);
            }
        } else if (a == "--notes") {
            std::string list = next();
            size_t pos = 0;
            while (pos < list.size()) {
                notes.push_back(atoi(list.c_str() + pos));
                pos = list.find(',', pos);
                if (pos == std::string::npos) break;
                pos++;
            }
        } else if (a == "--ramp") { /* ID=V0:V1 over the whole duration */
            std::string kv = next();
            RampSpec r{};
            if (sscanf(kv.c_str(), "%u=%lf:%lf", &r.id, &r.v0, &r.v1) != 3)
                die("--ramp wants ID=V0:V1");
            ramps.push_back(r);
        } else if (a == "--set") {
            std::string kv = next();
            auto eq = kv.find('=');
            if (eq == std::string::npos) die("--set wants ID=VALUE");
            sets.push_back({(uint32_t)strtoul(kv.substr(0, eq).c_str(), nullptr, 0),
                            atof(kv.substr(eq + 1).c_str())});
        } else
            die("unknown option " + a);
    }

    /* ---- host context + module ---- */
    static HostApplication hostApp;
    PluginContextFactory::instance().setPluginContext(&hostApp);

    std::string err;
    auto module = VST3::Hosting::Module::create(plugin_path, err);
    if (!module) die("cannot load module: " + err);

    auto factory = module->getFactory();
    IPtr<PlugProvider> provider;
    for (auto &ci : factory.classInfos()) {
        if (ci.category() == kVstAudioEffectClass) {
            printf("class: %s (%s) by %s\n", ci.name().c_str(), ci.subCategoriesString().c_str(),
                   ci.vendor().c_str());
            provider = owned(new PlugProvider(factory, ci, true));
            break;
        }
    }
    if (!provider || !provider->initialize()) die("no audio effect class / init failed");

    OPtr<IComponent> component = provider->getComponent();
    OPtr<IEditController> controller = provider->getController();
    if (!component) die("no component");
    FUnknownPtr<IAudioProcessor> processor(component);
    if (!processor) die("component is not an IAudioProcessor");

    /* ---- restore state (as a DAW project-open would) ---- */
    if (!load_state_path.empty()) {
        std::vector<char> comp, ctrl;
        if (!load_state_file(load_state_path, comp, ctrl)) die("cannot read state file");
        MemoryStream cs(comp.data(), (TSize)comp.size());
        if (component->setState(&cs) != kResultOk) die("component setState failed");
        if (controller) {
            MemoryStream cs2(comp.data(), (TSize)comp.size());
            controller->setComponentState(&cs2);
            if (!ctrl.empty()) {
                MemoryStream ts(ctrl.data(), (TSize)ctrl.size());
                controller->setState(&ts);
            }
        }
        printf("state: restored from %s (%zu+%zu bytes)\n", load_state_path.c_str(),
               comp.size(), ctrl.size());
    }

    /* ---- list ---- */
    if (do_list && controller) {
        int32 n = controller->getParameterCount();
        printf("parameters (%d):\n", n);
        for (int32 i = 0; i < n; i++) {
            ParameterInfo info{};
            if (controller->getParameterInfo(i, info) != kResultOk) continue;
            printf("  [%u] %-24s default=%.3f flags=0x%x\n", info.id,
                   VST3::StringConvert::convert(info.title).c_str(),
                   info.defaultNormalizedValue, info.flags);
        }
    }

    /* ---- bus setup ---- */
    int32 nin = component->getBusCount(kAudio, kInput);
    int32 nout = component->getBusCount(kAudio, kOutput);
    for (int32 i = 0; i < nin; i++) component->activateBus(kAudio, kInput, i, true);
    for (int32 i = 0; i < nout; i++) component->activateBus(kAudio, kOutput, i, true);
    if (nout == 0) die("plugin has no audio output bus");
    BusInfo outBus{};
    component->getBusInfo(kAudio, kOutput, 0, outBus);
    int32 out_ch = outBus.channelCount;
    int32 in_ch = 0;
    if (nin > 0) {
        BusInfo inBus{};
        component->getBusInfo(kAudio, kInput, 0, inBus);
        in_ch = inBus.channelCount;
    }
    printf("buses: %d in (%d ch), %d out (%d ch)\n", nin, in_ch, nout, out_ch);

    /* ---- processing setup ----
     * Default: no wall clock, declared kOffline so plugins bridging real
     * hardware may legitimately block on their transport. --realtime paces
     * process() against the wall clock like a DAW — the mode that exposes
     * backpressure/congestion bugs offline rendering hides. */
    ProcessSetup setup{realtime ? kRealtime : kOffline, kSample32, (int32)block,
                       (SampleRate)rate};
    if (processor->setupProcessing(setup) != kResultOk) die("setupProcessing failed");
    HostProcessData pd;
    if (!pd.prepare(*component, (int32)block, kSample32)) die("process data prepare failed");

    if (component->setActive(true) != kResultOk) die("setActive failed");
    processor->setProcessing(true);

    ProcessContext ctx{};
    ctx.sampleRate = rate;
    ctx.state = ProcessContext::kPlaying;
    pd.processContext = &ctx;

    size_t total = (size_t)(seconds * rate);
    size_t done = 0;
    std::vector<float> capture;
    capture.reserve(total * (size_t)out_ch);
    double phase = 0;
    bool first_block = true;

#ifdef __APPLE__
    if (realtime) /* compete like a DAW audio thread, not a background job */
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    struct timespec rt0;
    clock_gettime(CLOCK_MONOTONIC, &rt0);
    while (done < total) {
        if (realtime) { /* wall-clock pacing: process() at DAW cadence */
            double target = (double)done / rate;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed =
                (double)(now.tv_sec - rt0.tv_sec) + (now.tv_nsec - rt0.tv_nsec) / 1e9;
            if (target > elapsed) {
                struct timespec slp;
                double wait = target - elapsed;
                slp.tv_sec = (time_t)wait;
                slp.tv_nsec = (long)((wait - (double)slp.tv_sec) * 1e9);
                nanosleep(&slp, nullptr);
            }
        }
        size_t n = std::min((size_t)block, total - done);
        pd.numSamples = (int32)n;

        if (nin > 0 && pd.inputs) {
            for (int32 c = 0; c < in_ch; c++) {
                float *buf = pd.inputs[0].channelBuffers32[c];
                for (size_t s = 0; s < n; s++) {
                    if (input_kind == "sine")
                        buf[s] = 0.5f * (float)sin(2.0 * M_PI *
                                                   (phase + (double)s * sine_hz / rate));
                    else if (input_kind == "impulse")
                        buf[s] = (done == 0 && s == 0) ? 1.0f : 0.0f;
                    else
                        buf[s] = 0.0f;
                }
            }
            phase += (double)n * sine_hz / rate;
        }

        ParameterChanges pc;
        for (auto &r : ramps) { /* block-rate automation, like a DAW writes */
            double t = total > 1 ? (double)done / (double)total : 0.0;
            int32 qidx = 0;
            auto *q = pc.addParameterData(r.id, qidx);
            int32 pidx = 0;
            if (q) q->addPoint(0, r.v0 + (r.v1 - r.v0) * t, pidx);
        }
        if (first_block) {
            for (auto &kv : sets) {
                int32 qidx = 0;
                auto *q = pc.addParameterData(kv.first, qidx);
                int32 pidx = 0;
                if (q) q->addPoint(0, kv.second, pidx);
                if (controller) controller->setParamNormalized(kv.first, kv.second);
            }
            first_block = false;
        }
        pd.inputParameterChanges = &pc;
        ParameterChanges opc; /* plugin-originated changes (e.g. HARP echo) */
        pd.outputParameterChanges = &opc;

        EventList evList; /* scheduled --notes falling inside this block */
        for (size_t ni = 0; ni < notes.size(); ni++) {
            int64_t on_at = (int64_t)((double)ni * 0.6 * rate);
            int64_t off_at = on_at + (int64_t)(0.45 * rate);
            if (on_at >= (int64_t)done && on_at < (int64_t)(done + n)) {
                Event ev{};
                ev.type = Event::kNoteOnEvent;
                ev.sampleOffset = (int32)(on_at - (int64_t)done);
                ev.noteOn.pitch = (int16)notes[ni];
                ev.noteOn.velocity = 0.9f;
                ev.noteOn.noteId = -1;
                evList.addEvent(ev);
            }
            if (off_at >= (int64_t)done && off_at < (int64_t)(done + n)) {
                Event ev{};
                ev.type = Event::kNoteOffEvent;
                ev.sampleOffset = (int32)(off_at - (int64_t)done);
                ev.noteOff.pitch = (int16)notes[ni];
                ev.noteOff.velocity = 0.f;
                ev.noteOff.noteId = -1;
                evList.addEvent(ev);
            }
        }
        pd.inputEvents = &evList;

        if (processor->process(pd) != kResultOk) die("process failed");

        for (int32 i = 0; i < opc.getParameterCount(); i++) {
            IParamValueQueue *q = opc.getParameterData(i);
            if (!q || q->getPointCount() <= 0) continue;
            int32 off;
            ParamValue v;
            if (q->getPoint(q->getPointCount() - 1, off, v) == kResultOk)
                printf("echo: param %u -> %.4f (block %zu)\n", q->getParameterId(), v,
                       done / block);
        }

        for (size_t s = 0; s < n; s++)
            for (int32 c = 0; c < out_ch; c++)
                capture.push_back(pd.outputs[0].channelBuffers32[c][s]);
        done += n;
        ctx.projectTimeSamples += (TSamples)n;
    }

    processor->setProcessing(false);

    /* ---- save state ---- */
    if (!save_state_path.empty()) {
        MemoryStream comp, ctrl;
        if (component->getState(&comp) != kResultOk) die("component getState failed");
        if (controller) controller->getState(&ctrl);
        save_state_file(save_state_path, comp, ctrl);
        printf("state: saved to %s (%lld+%lld bytes)\n", save_state_path.c_str(),
               (long long)comp.getSize(), (long long)ctrl.getSize());
    }

    component->setActive(false);

    /* ---- output ---- */
    double rms = 0;
    for (float v : capture) rms += (double)v * v;
    rms = capture.empty() ? 0 : sqrt(rms / capture.size());
    printf("processed %zu samples x %d ch, rms=%.5f\n", done, out_ch, rms);
    if (do_hash)
        printf("output-hash: %016llx\n",
               (unsigned long long)fnv1a(capture.data(), capture.size() * sizeof(float)));
    if (!out_path.empty()) {
        write_wav16(out_path, capture, (uint32_t)out_ch, rate);
        printf("-> %s\n", out_path.c_str());
    }
    return 0;
}
