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

#define _USE_MATH_DEFINES /* M_PI on MSVC */
#include <chrono>
#include <cmath>
#include <cstdint>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "render_check.h"

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
                "       [--lfo ID=HZ[:PTS_PER_BLOCK[:SHAPE]]]... [--flood]\n"
                "       [--bpm B] [--chord N,N,..] [--loop STARTPPQ:ENDPPQ]\n"
                "       [--realtime] [--out FILE.wav] [--hash] [--json] [--expect-hash HEX]\n"
                "       [--save-state FILE] [--load-state FILE]\n");
        return 2;
    }
    std::string plugin_path = argv[1];
    bool do_list = false, do_hash = false, realtime = false;
    bool do_flood = false, do_json = false, do_reset = false;
    std::string expect_hash; /* if set, assert the output hash (exit 3 on mismatch) */
    uint32_t rate = 48000, block = 256;
    double seconds = 2.0;
    std::string input_kind = "silence", out_path, save_state_path, load_state_path;
    double sine_hz = 440.0;
    std::vector<std::pair<uint32_t, double>> sets;
    std::vector<int> notes;    /* played sequentially at note_period spacing */
    std::vector<int> chord;    /* held from 0.1 s to the end (arp fodder) */
    double bpm = 0;            /* >0: emit a playing transport (tempo, PPQ) */
    double loop_a = -1, loop_b = -1; /* PPQ loop region: jump b -> a */
    double note_period = 0.6;  /* s between note-ons; gate = 75% of period */
    struct RampSpec {
        uint32_t id;
        double v0, v1;
    };
    std::vector<RampSpec> ramps; /* one automation point per block, linear v0->v1 */
    struct LfoSpec {
        uint32_t id;
        double hz;
        int ppb;    /* automation points emitted per process block (density) */
        char shape; /* 't' triangle, 's' saw, 'n' sine */
    };
    std::vector<LfoSpec> lfos; /* dense sub-block automation — the IDM hammer */

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
        } else if (a == "--bpm") {
            bpm = atof(argv[++i]);
        } else if (a == "--chord") {
            std::string list = argv[++i];
            size_t pos = 0;
            while (pos < list.size()) {
                chord.push_back(atoi(list.c_str() + pos));
                pos = list.find(',', pos);
                if (pos == std::string::npos) break;
                pos++;
            }
        } else if (a == "--loop") {
            if (sscanf(argv[++i], "%lf:%lf", &loop_a, &loop_b) != 2)
                die("--loop wants STARTPPQ:ENDPPQ");
        } else if (a == "--notes") {
            std::string list = next();
            size_t pos = 0;
            while (pos < list.size()) {
                notes.push_back(atoi(list.c_str() + pos));
                pos = list.find(',', pos);
                if (pos == std::string::npos) break;
                pos++;
            }
        } else if (a == "--note-period") {
            note_period = atof(next().c_str());
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
        } else if (a == "--lfo") { /* ID=HZ[:POINTS_PER_BLOCK[:SHAPE]] */
            std::string kv = next();
            LfoSpec l{};
            l.ppb = 8;
            char shape = 't';
            int got = sscanf(kv.c_str(), "%u=%lf:%d:%c", &l.id, &l.hz, &l.ppb, &shape);
            if (got < 2) die("--lfo wants ID=HZ[:POINTS_PER_BLOCK[:SHAPE]]");
            if (l.ppb < 1) l.ppb = 1;
            l.shape = shape;
            lfos.push_back(l);
        } else if (a == "--reset") {
            do_reset = true;
        } else if (a == "--flood") {
            do_flood = true;
        } else if (a == "--json") {
            do_json = true;
        } else if (a == "--expect-hash") {
            expect_hash = next();
        } else
            die("unknown option " + a);
    }

    /* The flood preset: an IDM-grade hammer on the event plane — tiny DAW
     * blocks, every knob under dense LFO automation, rapid notes, a fast tempo,
     * and a per-beat loop wrap. Each piece stays overridable by an explicit flag.
     * Two runs of this must still hash identically: the determinism gate. */
    if (do_flood) {
        if (block == 256) block = 64;   /* smallest DAW block = peak event rate */
        if (seconds == 2.0) seconds = 4.0;
        if (bpm == 0) bpm = 174.0;      /* + a per-beat loop wrap below */
        if (loop_a < 0) { loop_a = 0.0; loop_b = 1.0; }
        if (lfos.empty())
            for (uint32_t id = 1; id <= 8; id++)
                lfos.push_back({id, 2.0 + 1.7 * id, 8, (id & 1) ? 't' : 'n'});
        /* dense sample-exact notes (not host-thinnable, unlike automation) hammer
         * the note voice + retrigger path. The device arpeggiator is an even
         * denser event source, but its transport anchoring isn't reproducible
         * across separate processes — exercise it via the in-session T17 test
         * instead, and keep this flood deterministic for a clean CI gate. */
        if (notes.empty() && chord.empty()) {
            note_period = 0.05; /* 20 notes/s */
            for (int k = 0; k < 80; k++) notes.push_back(48 + (k * 5) % 25);
        }
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
    if (bpm > 0) {
        ctx.state |= ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
        ctx.tempo = bpm;
        ctx.projectTimeMusic = 0;
    }
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
    auto rt0 = std::chrono::steady_clock::now();
    while (done < total) {
        if (realtime) { /* wall-clock pacing: process() at DAW cadence */
            double target = (double)done / rate;
            double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - rt0).count();
            if (target > elapsed)
                std::this_thread::sleep_for(std::chrono::duration<double>(target - elapsed));
        }
        size_t n = std::min((size_t)block, total - done);
        pd.numSamples = (int32)n;
        if (bpm > 0) {
            ctx.projectTimeSamples = (TSamples)done;
            /* loop simulation: when the block START passes the loop end,
             * jump back — like Live, the wrap lands on a block boundary
             * and the new position is announced in this block's context */
            if (loop_b > loop_a && loop_a >= 0 &&
                ctx.projectTimeMusic + 1e-9 >= loop_b)
                ctx.projectTimeMusic = loop_a + (ctx.projectTimeMusic - loop_b);
        }

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
        if (do_reset && done == 0 && controller) {
            /* set EVERY param to its default at t=0 — a clean patch to render
             * from, regardless of the device's persisted state. NOTE: under
             * dense automation the shell's ramp thinning (§9.4) folds this into
             * the first ramp, so it does NOT guarantee cross-run determinism for
             * automated params; the flood determinism gate instead pins device
             * state directly via the front panel (scripts/flood-stress.sh). */
            int32 np = controller->getParameterCount();
            for (int32 pi = 0; pi < np; pi++) {
                ParameterInfo info{};
                if (controller->getParameterInfo(pi, info) != kResultOk) continue;
                if (info.flags & ParameterInfo::kIsReadOnly) continue;
                int32 qi = 0;
                auto *q = pc.addParameterData(info.id, qi);
                int32 pp = 0;
                if (q) q->addPoint(0, info.defaultNormalizedValue, pp);
                controller->setParamNormalized(info.id, info.defaultNormalizedValue);
            }
        }
        for (auto &r : ramps) { /* block-rate automation, like a DAW writes */
            double t = total > 1 ? (double)done / (double)total : 0.0;
            int32 qidx = 0;
            auto *q = pc.addParameterData(r.id, qidx);
            int32 pidx = 0;
            if (q) q->addPoint(0, r.v0 + (r.v1 - r.v0) * t, pidx);
        }
        for (auto &l : lfos) { /* dense sub-block automation — many points/block */
            int32 qidx = 0;
            auto *q = pc.addParameterData(l.id, qidx);
            if (!q) continue;
            for (int k = 0; k < l.ppb; k++) {
                int32 off = (int32)((int64_t)k * (int64_t)n / l.ppb);
                double ph = l.hz * (double)(done + (size_t)off) / rate;
                ph -= (double)(int64_t)ph; /* fractional phase 0..1, deterministic */
                double v = (l.shape == 's')   ? ph                                /* saw */
                           : (l.shape == 'n') ? 0.5 + 0.5 * sin(2.0 * M_PI * ph)  /* sine */
                                              : (ph < 0.5 ? 2.0 * ph : 2.0 - 2.0 * ph); /* tri */
                int32 pidx = 0;
                q->addPoint(off, v, pidx);
            }
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
        if (!chord.empty()) {
            size_t onAt = (size_t)(0.1 * rate);
            bool last = done + n >= total;
            for (int cn : chord) {
                if (onAt >= done && onAt < done + n) {
                    Event ev{};
                    ev.type = Event::kNoteOnEvent;
                    ev.sampleOffset = (int32)(onAt - done);
                    ev.noteOn.channel = 0;
                    ev.noteOn.pitch = (int16)cn;
                    ev.noteOn.velocity = 0.8f;
                    evList.addEvent(ev);
                }
                if (last) {
                    Event ev{};
                    ev.type = Event::kNoteOffEvent;
                    ev.sampleOffset = (int32)(n > 0 ? n - 1 : 0);
                    ev.noteOff.channel = 0;
                    ev.noteOff.pitch = (int16)cn;
                    ev.noteOff.velocity = 0;
                    evList.addEvent(ev);
                }
            }
        }
        for (size_t ni = 0; ni < notes.size(); ni++) {
            int64_t on_at = (int64_t)((double)ni * note_period * rate);
            int64_t off_at = on_at + (int64_t)(0.75 * note_period * rate);
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
        if (bpm > 0) ctx.projectTimeMusic += (double)n * bpm / (60.0 * rate);
        done += n;
        ctx.projectTimeSamples += (TSamples)n;
    }

    processor->setProcessing(false);
    component->setActive(false);

    /* ---- save state ---- */
    /* Deactivate BEFORE saving, matching a DAW's offline-render flow (REAPER
     * deactivates the plugin once the render is done, then writes the project):
     * the device is no longer claimed, so this exercises the shell's offline
     * getState path — the harder case that an active-plugin save would miss. */
    if (!save_state_path.empty()) {
        MemoryStream comp, ctrl;
        if (component->getState(&comp) != kResultOk) die("component getState failed");
        if (controller) controller->getState(&ctrl);
        save_state_file(save_state_path, comp, ctrl);
        printf("state: saved to %s (%lld+%lld bytes)\n", save_state_path.c_str(),
               (long long)comp.getSize(), (long long)ctrl.getSize());
    }

    /* ---- output ---- */
    double rms = 0;
    for (float v : capture) rms += (double)v * v;
    rms = capture.empty() ? 0 : sqrt(rms / capture.size());
    printf("processed %zu samples x %d ch, rms=%.5f\n", done, out_ch, rms);

    uint64_t hash = harp_fnv1a(capture.data(), capture.size() * sizeof(float));
    char hashhex[17];
    snprintf(hashhex, sizeof hashhex, "%016llx", (unsigned long long)hash);
    if (do_hash) printf("output-hash: %s\n", hashhex);
    if (do_json)
        printf("{\"frames\":%zu,\"channels\":%d,\"rate\":%u,\"rms\":%.6f,\"hash\":\"%s\"}\n",
               capture.size() / (size_t)(out_ch ? out_ch : 1), out_ch, rate, rms, hashhex);

    if (!out_path.empty()) {
        if (!harp_write_wav16(out_path, capture, (uint32_t)out_ch, rate)) die("cannot write " + out_path);
        printf("-> %s\n", out_path.c_str());
    }
    if (!expect_hash.empty() && expect_hash != hashhex) {
        fprintf(stderr, "harp-vst3-host: FAIL expected hash %s, got %s\n", expect_hash.c_str(),
                hashhex);
        return 3;
    }
    return 0;
}
