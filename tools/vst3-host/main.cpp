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

/* MULTI-OUT: how many output buses (bus 0 = main mix, then per-part buses 1..16) the host
 * activates. Default 1 = main-mix only (the golden-identical path). --out-buses N activates N. */
static int g_outBuses = 1;
/* MULTI-OUT: which output bus to CAPTURE to the WAV/hash/rms (0 = main mix, 1..16 = parts).
 * Used by the per-part isolation test: drive notes on a channel, capture that part's bus and
 * a silent neighbour. --capture-bus N implies activating at least N+1 buses. */
static int g_captureBus = 0;
/* M2 per-channel device-param ids — mirrored from shell/shell_constants.h (base 0x4000, stride
 * 16). --set-ch CH:ID=V sends part CH's device param ID through the SAME synthetic-id path a
 * satellite's MIDI CC arrives on after the host's IMidiMapping, exercising the processor decode. */
static const uint32_t kPerChanParamBase = 0x4000u;
static const uint32_t kPerChanStride = 16u;
static inline uint32_t perChanParamId(uint32_t chan, uint32_t param /*1-based*/) {
    return kPerChanParamBase + (chan & 0xf) * kPerChanStride + ((param - 1u) & 0xf);
}

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
    if (a) fwrite(comp.getData(), 1, a, f); /* getData() is null for an empty stream, and */
    fwrite(&b, 4, 1, f);                    /* fwrite's buffer arg is nonnull even when n==0 */
    if (b) fwrite(ctrl.getData(), 1, b, f); /* (UB; AU/CLAP have no controller -> b==0). */
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

/* ---- DAW bus-arrangement negotiation (IAudioProcessor::setBusArrangements) ----
 *
 * A real DAW (Ableton Live) ADVERTISES its track's speaker layout to the plugin
 * via IAudioProcessor::setBusArrangements BEFORE setupProcessing/setActive, and
 * routes audio to the input bus only for an arrangement the plugin ACCEPTS. This
 * harness historically skipped that call — so a plugin whose setBusArrangements
 * rejected the host's layout (or whose SDK-base default mishandled an INPUT bus)
 * would be mis-driven in a DAW yet sail through here. We now emulate it: offer
 * each declared bus its own channel count (mono/stereo) and report the plugin's
 * tresult. When the offered arrangement already matches what the plugin declared
 * (the common case — both HARP shells declare what they want) the SDK base accepts
 * and nothing about the render changes, so this is a no-op for a plugin with no
 * input bus (the instrument golden stays byte-identical). Returns the plugin's
 * tresult (kResultTrue = accepted). */
static SpeakerArrangement arr_for(int32 nch) {
    switch (nch) {
        case 0:  return SpeakerArr::kEmpty;
        case 1:  return SpeakerArr::kMono;
        case 2:  return SpeakerArr::kStereo;
        default: return SpeakerArr::kStereo;
    }
}
static tresult negotiate_buses(IComponent *component, IAudioProcessor *processor, bool verbose) {
    int32 nin = component->getBusCount(kAudio, kInput);
    int32 nout = component->getBusCount(kAudio, kOutput);
    std::vector<SpeakerArrangement> ins((size_t)nin), outs((size_t)nout);
    for (int32 i = 0; i < nin; i++) {
        BusInfo b{};
        component->getBusInfo(kAudio, kInput, i, b);
        ins[(size_t)i] = arr_for(b.channelCount);
    }
    for (int32 i = 0; i < nout; i++) {
        BusInfo b{};
        component->getBusInfo(kAudio, kOutput, i, b);
        outs[(size_t)i] = arr_for(b.channelCount);
    }
    tresult r = processor->setBusArrangements(ins.empty() ? nullptr : ins.data(), nin,
                                              outs.empty() ? nullptr : outs.data(), nout);
    if (verbose)
        printf("bus-arrangements: setBusArrangements(in=%d, out=%d, stereo/mono) -> %s\n", nin,
               nout, r == kResultTrue ? "accepted" : (r == kResultFalse ? "REJECTED" : "other"));
    return r;
}

/* ---- multi-channel render (P6): the multitimbral MAIN INSTANCE through the plugin ----
 *
 * The single-instance host above loads ONE plugin and renders it with notes on
 * one channel. This renders the SAME ONE plugin instance but drives N MIDI
 * channels into it at once — the multi-out main-instance model: a note on channel
 * C routes to device part C (§9.4 key 5; plugin.cpp routes ev.noteOn.channel ->
 * device part), and the instance's bus 0 (the main mix) SUMS every engaged part.
 * This is exactly what a DAW does when N MIDI tracks (or Renoise aliases) feed one
 * HARP main track on channels 0..N-1; the per-channel routing is the DAW's job,
 * reproduced here by injecting each channel's notes into the one input stream.
 *
 * NO registry, NO sharing, NO per-instance Part parameter: there is exactly ONE
 * claimer (this instance) and routing is by the note's MIDI channel alone. (This
 * replaced the old N-plugin-instances-share-one-serial model when the device
 * became a single multi-out main; the registry that merged N event sources is
 * gone.) `channels` lists the active parts; each is struck with the base note
 * transposed by its channel so the parts are distinct pitches — a real
 * multitimbral spread, not a unison.
 *
 * CAPTURE: bus 0 = the device main mix. An N-channel render's main mix DIFFERS
 * from a 1-channel (part-0-only) render because more parts are summed — the
 * play-proof alias-group-e2e.sh measures (single hash != group hash), now produced
 * through the real plugin's one multi-out instance.
 *
 * Returns the process exit code. Single-channel invocations never reach here. */
static int run_multi_instance(VST3::Hosting::Module::Ptr &module, const VST3::Hosting::ClassInfo &ci,
                              const std::vector<int> &channels, const std::string &serial,
                              uint32_t rate, uint32_t block, double seconds, double bpm,
                              const std::vector<int> &notes, double note_period,
                              const std::vector<int> &chord, bool realtime,
                              const std::string &out_path, bool do_hash, bool do_json,
                              const std::string &expect_hash) {
    const int N = (int)channels.size();

    /* Device selection only (NOT sharing — there is ONE instance): honor an
     * explicit --serial / HARP_DEVICE_SERIAL so the lone claimer pins a board,
     * exactly as the single-instance path does. Set before setActive below. */
    if (!serial.empty()) {
#ifdef _WIN32
        _putenv_s("HARP_DEVICE_SERIAL", serial.c_str());
#else
        setenv("HARP_DEVICE_SERIAL", serial.c_str(), 1);
#endif
    }
    printf("multi-channel: %d active parts, serial=%s, channels=", N, serial.c_str());
    for (int k = 0; k < N; k++) printf("%s%d", k ? "," : "", channels[k]);
    printf("\n");

    auto factory = module->getFactory();

    /* ONE full plugin instance: the multi-out main. */
    IPtr<PlugProvider> provider = owned(new PlugProvider(factory, ci, true));
    if (!provider || !provider->initialize()) die("multi-channel: provider init failed");
    OPtr<IComponent> component = provider->getComponent();
    if (!component) die("multi-channel: no component");
    FUnknownPtr<IAudioProcessor> processor(component);
    if (!processor) die("multi-channel: component is not an IAudioProcessor");

    int32 nin = component->getBusCount(kAudio, kInput);
    int32 nout = component->getBusCount(kAudio, kOutput);
    for (int32 i = 0; i < nin; i++) component->activateBus(kAudio, kInput, i, true);
    int32 actOut = nout < g_outBuses ? nout : g_outBuses; /* MULTI-OUT: main + parts */
    for (int32 i = 0; i < actOut; i++) component->activateBus(kAudio, kOutput, i, true);
    if (nout == 0) die("multi-channel: no audio output bus");
    BusInfo outBus{};
    component->getBusInfo(kAudio, kOutput, 0, outBus);
    int32 out_ch = outBus.channelCount;

    ProcessSetup setup{realtime ? kRealtime : kOffline, kSample32, (int32)block, (SampleRate)rate};
    if (processor->setupProcessing(setup) != kResultOk) die("multi-channel: setupProcessing failed");
    HostProcessData pd;
    if (!pd.prepare(*component, (int32)block, kSample32)) die("multi-channel: process data prepare failed");

    if (component->setActive(true) != kResultOk) die("multi-channel: setActive failed");
    processor->setProcessing(true);
    printf("multi-channel: instance active, %d parts engaged on bus 0 main mix\n", N);

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
    std::vector<float> capture; /* bus 0 main mix */
    capture.reserve(total * (size_t)out_ch);

#ifdef __APPLE__
    if (realtime) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    auto rt0 = std::chrono::steady_clock::now();
    while (done < total) {
        if (realtime) {
            double target = (double)done / rate;
            double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - rt0).count();
            if (target > elapsed)
                std::this_thread::sleep_for(std::chrono::duration<double>(target - elapsed));
        }
        size_t n = std::min((size_t)block, total - done);
        if (bpm > 0) ctx.projectTimeSamples = (TSamples)done;

        pd.numSamples = (int32)n;

        ParameterChanges pcs, opcs;
        pd.inputParameterChanges = &pcs;
        pd.outputParameterChanges = &opcs;

        /* Inject every active channel's notes/chord into the ONE instance, each
         * on its own MIDI channel == its device part, transposed by channel so
         * the parts are distinct pitches — a real multitimbral spread. */
        EventList evl;
        for (int k = 0; k < N; k++) {
            int ch = channels[k];
            if (!chord.empty()) {
                size_t onAt = (size_t)(0.1 * rate);
                bool last = done + n >= total;
                for (int cn : chord) {
                    int pitch = cn + ch;
                    if (onAt >= done && onAt < done + n) {
                        Event ev{};
                        ev.type = Event::kNoteOnEvent;
                        ev.sampleOffset = (int32)(onAt - done);
                        ev.noteOn.channel = (int16)ch;
                        ev.noteOn.pitch = (int16)pitch;
                        ev.noteOn.velocity = 0.8f;
                        ev.noteOn.noteId = -1;
                        evl.addEvent(ev);
                    }
                    if (last) {
                        Event ev{};
                        ev.type = Event::kNoteOffEvent;
                        ev.sampleOffset = (int32)(n > 0 ? n - 1 : 0);
                        ev.noteOff.channel = (int16)ch;
                        ev.noteOff.pitch = (int16)pitch;
                        ev.noteOff.velocity = 0;
                        ev.noteOff.noteId = -1;
                        evl.addEvent(ev);
                    }
                }
            }
            for (size_t ni = 0; ni < notes.size(); ni++) {
                int64_t on_at = (int64_t)((double)ni * note_period * rate);
                int64_t off_at = on_at + (int64_t)(0.75 * note_period * rate);
                int pitch = notes[ni] + ch;
                if (on_at >= (int64_t)done && on_at < (int64_t)(done + n)) {
                    Event ev{};
                    ev.type = Event::kNoteOnEvent;
                    ev.sampleOffset = (int32)(on_at - (int64_t)done);
                    ev.noteOn.channel = (int16)ch;
                    ev.noteOn.pitch = (int16)pitch;
                    ev.noteOn.velocity = 0.9f;
                    ev.noteOn.noteId = -1;
                    evl.addEvent(ev);
                }
                if (off_at >= (int64_t)done && off_at < (int64_t)(done + n)) {
                    Event ev{};
                    ev.type = Event::kNoteOffEvent;
                    ev.sampleOffset = (int32)(off_at - (int64_t)done);
                    ev.noteOff.channel = (int16)ch;
                    ev.noteOff.pitch = (int16)pitch;
                    ev.noteOff.velocity = 0.f;
                    ev.noteOff.noteId = -1;
                    evl.addEvent(ev);
                }
            }
        }
        pd.inputEvents = &evl;

        if (processor->process(pd) != kResultOk) die("multi-channel: process failed");

        /* Capture bus 0 = the device main mix (sums every engaged part). */
        for (size_t s = 0; s < n; s++)
            for (int32 c = 0; c < out_ch; c++)
                capture.push_back(pd.outputs[0].channelBuffers32[c][s]);

        if (bpm > 0) ctx.projectTimeMusic += (double)n * bpm / (60.0 * rate);
        done += n;
        ctx.projectTimeSamples += (TSamples)n;
    }

    processor->setProcessing(false);
    component->setActive(false);

    /* ---- output (owner main mix), same oracle as the single-instance path ---- */
    double rms = 0;
    for (float v : capture) rms += (double)v * v;
    rms = capture.empty() ? 0 : sqrt(rms / capture.size());
    printf("processed %zu samples x %d ch (bus 0 main mix), rms=%.5f\n", done, out_ch, rms);

    uint64_t hash = harp_fnv1a(capture.data(), capture.size() * sizeof(float));
    char hashhex[17];
    snprintf(hashhex, sizeof hashhex, "%016llx", (unsigned long long)hash);
    if (do_hash) printf("output-hash: %s\n", hashhex);
    if (do_json)
        printf("{\"frames\":%zu,\"channels\":%d,\"rate\":%u,\"rms\":%.6f,\"hash\":\"%s\","
               "\"parts\":%d}\n",
               capture.size() / (size_t)(out_ch ? out_ch : 1), out_ch, rate, rms, hashhex, N);

    if (!out_path.empty()) {
        if (!harp_write_wav16(out_path, capture, (uint32_t)out_ch, rate))
            die("cannot write " + out_path);
        printf("-> %s\n", out_path.c_str());
    }
    if (!expect_hash.empty() && expect_hash != hashhex) {
        fprintf(stderr, "harp-vst3-host: FAIL expected hash %s, got %s\n", expect_hash.c_str(),
                hashhex);
        return 3;
    }
    return 0;
}

/* ---- FX multi-insert (§8.8): faithful model of a DAW loading two HARP-FX inserts ----
 *
 * The single-instance path makes the SOLE FX the registry owner, so it always arms
 * its host-paced session and returns the reverb — it cannot reproduce the Ableton
 * "FX on a track is silent" bug, where a TRANSIENT scan instance grabs ownership of
 * the by-serial shared runtime first and the LIVE insert attaches as a NON-OWNER
 * (which runs no session setup -> process() has no event source -> the track goes
 * silent). run_multi_instance only ever captures the OWNER (alias 0), so it can't
 * see a non-owner's output either.
 *
 * This loads N harp-fx-shell instances in ONE process, all pinned to ONE device
 * serial (so they share via the P4 registry exactly as two FX inserts in Live do),
 * activates them OWNER-FIRST, feeds the SAME track audio (--input) to each, and
 * captures a CHOSEN insert (default the LAST = the non-owner). With --fx-drop-owner
 * it tears insert 0 down before the capture loop — the transient scan instance going
 * away — leaving the captured insert as the lone survivor, the precise condition from
 * the Ableton log (one live insert, non-owner, silent).
 *
 *   PRE-FIX  capture #1 -> rms ~0  (the Ableton silence, reproduced headlessly)
 *   POST-FIX capture #1 -> non-silent (each insert now owns its own session)
 *
 * Returns the process exit code. */
static int run_fx_instances(VST3::Hosting::Module::Ptr &module, const VST3::Hosting::ClassInfo &ci,
                            int n_inst, int capture_idx, bool drop_owner, const std::string &serial,
                            uint32_t rate, uint32_t block, double seconds,
                            const std::string &input_kind, double sine_hz, bool realtime,
                            const std::string &out_path, bool do_hash, bool do_json,
                            const std::string &expect_hash) {
    /* Pin the serial for the whole process BEFORE any setActive: every FX instance
     * reads HARP_DEVICE_SERIAL and (pre-fix) hands it to the registry, so all N
     * share ONE runtime / ONE claim and only insert 0 owns it. */
#ifdef _WIN32
    _putenv_s("HARP_DEVICE_SERIAL", serial.c_str());
#else
    setenv("HARP_DEVICE_SERIAL", serial.c_str(), 1);
#endif
    if (capture_idx < 0 || capture_idx >= n_inst) capture_idx = n_inst - 1;
    printf("fx-instances: %d FX inserts, serial=%s, capture=#%d%s\n", n_inst, serial.c_str(),
           capture_idx, drop_owner ? ", drop-owner (scan instance torn down before capture)" : "");

    auto factory = module->getFactory();
    struct Inst {
        IPtr<PlugProvider> provider;
        OPtr<IComponent> component;
        OPtr<IEditController> controller;
        FUnknownPtr<IAudioProcessor> processor{nullptr};
        HostProcessData pd;
        int32 out_ch = 0, in_ch = 0, nin = 0, nout = 0;
    };
    std::vector<std::unique_ptr<Inst>> insts((size_t)n_inst);

    for (int k = 0; k < n_inst; k++) {
        auto in = std::make_unique<Inst>();
        in->provider = owned(new PlugProvider(factory, ci, true));
        if (!in->provider || !in->provider->initialize())
            die("fx-instances: provider init failed for insert " + std::to_string(k));
        in->component = in->provider->getComponent();
        in->controller = in->provider->getController();
        if (!in->component) die("fx-instances: no component for insert " + std::to_string(k));
        in->processor = FUnknownPtr<IAudioProcessor>(in->component);
        if (!in->processor) die("fx-instances: component is not an IAudioProcessor");
        in->nin = in->component->getBusCount(kAudio, kInput);
        in->nout = in->component->getBusCount(kAudio, kOutput);
        for (int32 i = 0; i < in->nin; i++) in->component->activateBus(kAudio, kInput, i, true);
        for (int32 i = 0; i < in->nout; i++) in->component->activateBus(kAudio, kOutput, i, true);
        if (in->nout == 0) die("fx-instances: insert has no audio output bus");
        /* DAW arrangement negotiation, like Ableton (stereo in / stereo out). */
        negotiate_buses(in->component.get(), in->processor, k == 0);
        BusInfo ob{};
        in->component->getBusInfo(kAudio, kOutput, 0, ob);
        in->out_ch = ob.channelCount;
        if (in->nin > 0) {
            BusInfo ib{};
            in->component->getBusInfo(kAudio, kInput, 0, ib);
            in->in_ch = ib.channelCount;
        }
        ProcessSetup setup{realtime ? kRealtime : kOffline, kSample32, (int32)block,
                           (SampleRate)rate};
        if (in->processor->setupProcessing(setup) != kResultOk)
            die("fx-instances: setupProcessing failed for insert " + std::to_string(k));
        if (!in->pd.prepare(*in->component, (int32)block, kSample32))
            die("fx-instances: process data prepare failed");
        insts[(size_t)k] = std::move(in);
    }

    /* Activate OWNER-FIRST: insert 0 acquires the shared-by-serial runtime (the
     * registry owner), the rest attach owner=false — Ableton's exact lifecycle. */
    for (int k = 0; k < n_inst; k++) {
        if (insts[(size_t)k]->component->setActive(true) != kResultOk)
            die("fx-instances: setActive failed for insert " + std::to_string(k));
        insts[(size_t)k]->processor->setProcessing(true);
    }
    printf("fx-instances: %d inserts active (insert 0 = first to acquire / registry owner)\n",
           n_inst);

    /* --fx-drop-owner: tear insert 0 down (the transient scan instance Ableton
     * discards) before the capture, leaving the captured insert as the survivor. */
    if (drop_owner && n_inst >= 2) {
        insts[0]->processor->setProcessing(false);
        insts[0]->component->setActive(false);
        insts[0].reset(); /* release -> runtime_release on insert 0's handle */
        printf("fx-instances: dropped insert 0 (scan instance); capturing survivor #%d\n",
               capture_idx);
    }

    ProcessContext ctx{};
    ctx.sampleRate = rate;
    ctx.state = ProcessContext::kPlaying;
    for (int k = 0; k < n_inst; k++)
        if (insts[(size_t)k]) insts[(size_t)k]->pd.processContext = &ctx;

    Inst *cap = insts[(size_t)capture_idx].get();
    if (!cap) die("fx-instances: captured insert was dropped (do not --fx-capture the dropped owner)");
    const int32 out_ch = cap->out_ch;
    size_t total = (size_t)(seconds * rate), done = 0;
    std::vector<float> capture;
    capture.reserve(total * (size_t)out_ch);
    double phase = 0;

#ifdef __APPLE__
    if (realtime) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    auto rt0 = std::chrono::steady_clock::now();
    while (done < total) {
        if (realtime) {
            double target = (double)done / rate;
            double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - rt0).count();
            if (target > elapsed)
                std::this_thread::sleep_for(std::chrono::duration<double>(target - elapsed));
        }
        size_t n = std::min((size_t)block, total - done);
        for (int k = 0; k < n_inst; k++) {
            Inst *in = insts[(size_t)k].get();
            if (!in) continue;
            in->pd.numSamples = (int32)n;
            /* feed the same --input track signal to every insert's input bus */
            if (in->nin > 0 && in->pd.inputs) {
                for (int32 c = 0; c < in->in_ch; c++) {
                    float *buf = in->pd.inputs[0].channelBuffers32[c];
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
            }
            in->pd.inputParameterChanges = nullptr;
            in->pd.outputParameterChanges = nullptr;
            in->pd.inputEvents = nullptr;
            if (in->processor->process(in->pd) != kResultOk)
                die("fx-instances: process failed for insert " + std::to_string(k));
        }
        phase += (double)n * sine_hz / rate;
        for (size_t s = 0; s < n; s++)
            for (int32 c = 0; c < out_ch; c++)
                capture.push_back(cap->pd.outputs[0].channelBuffers32[c][s]);
        done += n;
    }

    for (int k = n_inst - 1; k >= 0; k--) {
        if (!insts[(size_t)k]) continue;
        insts[(size_t)k]->processor->setProcessing(false);
        insts[(size_t)k]->component->setActive(false);
    }

    double rms = 0;
    for (float v : capture) rms += (double)v * v;
    rms = capture.empty() ? 0 : sqrt(rms / capture.size());
    printf("processed %zu samples x %d ch (FX insert #%d), rms=%.5f\n", done, out_ch, capture_idx,
           rms);
    uint64_t hash = harp_fnv1a(capture.data(), capture.size() * sizeof(float));
    char hashhex[17];
    snprintf(hashhex, sizeof hashhex, "%016llx", (unsigned long long)hash);
    if (do_hash) printf("output-hash: %s\n", hashhex);
    if (do_json)
        printf("{\"frames\":%zu,\"channels\":%d,\"rate\":%u,\"rms\":%.6f,\"hash\":\"%s\","
               "\"fx_instances\":%d,\"capture\":%d}\n",
               capture.size() / (size_t)(out_ch ? out_ch : 1), out_ch, rate, rms, hashhex, n_inst,
               capture_idx);
    if (!out_path.empty()) {
        if (!harp_write_wav16(out_path, capture, (uint32_t)out_ch, rate))
            die("cannot write " + out_path);
        printf("-> %s\n", out_path.c_str());
    }
    if (!expect_hash.empty() && expect_hash != hashhex) {
        fprintf(stderr, "harp-vst3-host: FAIL expected hash %s, got %s\n", expect_hash.c_str(),
                hashhex);
        return 3;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage: harp-vst3-host PLUGIN.vst3 [--list] [--rate N] [--block N]\n"
                "       [--seconds S] [--input sine[:HZ]|impulse|silence]\n"
                "       [--set ID=NORMVALUE]... [--ramp ID=V0:V1]... [--notes N,N,..]\n"
                "       [--lfo ID=HZ[:PTS_PER_BLOCK[:SHAPE]]]... [--flood]\n"
                "       [--bpm B] [--chord N,N,..] [--brightness V] [--brightness-idx K] [--tuning SEMIS] [--tuning-idx K]\n"
                "       [--channel N] [--loop STARTPPQ:ENDPPQ]\n"
                "       [--part N] [--realtime] [--out FILE.wav] [--hash] [--json]\n"
                "       [--expect-hash HEX] [--save-state FILE] [--load-state FILE]\n"
                "       [--load-state-after-connect FILE]\n"
                "       [--diag-bundle FILE | --diag-bundle-anon FILE]\n"
                "       [--loopback IN,OUT]\n"
                "       [--instances N | --aliases ch0,ch1,..] [--serial SERIAL]\n"
                "       [--fx-instances N [--fx-capture K] [--fx-drop-owner]]\n"
                "  --diag-bundle FILE   §14.4 host-context-A: after the render, capture\n"
                "             the runtime's diag bundle (device-section + host counters +\n"
                "             audio-config) and write the CBOR to FILE (read-only, no\n"
                "             audio-path effect). --diag-bundle-anon adds the §16 PII pass.\n"
                "  --loopback IN,OUT  §14.3 digital round-trip probe: inject an impulse on\n"
                "             H->D slot IN, capture the echo on D->H slot OUT (the device\n"
                "             copies IN->OUT in-frame), and print measured RTT + §6.4\n"
                "             expected + delta. OUT must be a slot no note is driving.\n"
                "  --part N   pull device part N's (0..15) stereo output instead of the\n"
                "             main mix (slots {2+2N,3+2N}); default = main mix\n"
                "  --part-audio   multi-instance: attached aliases pull their OWN part\n"
                "             (P5b demux), not silence; default off (siblings silent)\n"
                "  --instances N   load N plugin instances in ONE process, sharing the\n"
                "             device (pinned serial -> one claim, P4 registry). Instance\n"
                "             i drives part/channel i (Part param id 98) with a distinct\n"
                "             note; the OWNER's main mix is captured -> a multitimbral mix.\n"
                "  --aliases L     like --instances but with an explicit channel list,\n"
                "             e.g. --aliases 0,1,2,3 (one alias per channel)\n"
                "  --serial S      serial to pin for the shared claim (default: env\n"
                "             HARP_DEVICE_SERIAL or PI4B-0001); multi-instance only\n"
                "  --fx-instances N  §8.8: load N harp-fx-shell inserts in ONE process,\n"
                "             all pinned to ONE device serial (default KR260-FX01), activated\n"
                "             OWNER-FIRST, each fed --input; captures --fx-capture K (default\n"
                "             the LAST = the non-owner). Reproduces the Ableton FX-track-silent\n"
                "             bug headlessly: pre-fix capture of a non-owner is rms~0.\n"
                "  --fx-capture K  which insert to capture (default last/non-owner)\n"
                "  --fx-drop-owner tear insert 0 down before the capture loop (the transient\n"
                "             DAW scan instance going away), leaving the captured insert alone\n");
        return 2;
    }
    std::string plugin_path = argv[1];
    bool do_list = false, do_hash = false, realtime = false;
    bool do_flood = false, do_json = false, do_reset = false;
    bool part_audio = false; /* --part-audio: attached aliases pull their OWN part
                              * (P5b) — sets HARP_PART_AUDIO so each attached plugin
                              * registers a per-part sink the owner demuxes for it */
    std::string expect_hash; /* if set, assert the output hash (exit 3 on mismatch) */
    uint32_t rate = 48000, block = 256;
    double seconds = 2.0;
    std::string input_kind = "silence", out_path, save_state_path, load_state_path;
    /* §11.4 staged-while-connected (HIGH #8): --load-state-after-connect FILE restores
     * the SAME bundle as --load-state, but DEFERS the setState until AFTER setActive(true)
     * (the connect). A pre-activate restore lands while the runtime is still disconnected;
     * deferring it drives HarpRuntime::setStateBundle's connected() branch — the #73
     * production trigger (a DAW staging a recall onto an already-live device). Empty =
     * off; mutually exclusive with --load-state (the pre-activate path). */
    std::string load_state_after_path;
    double sine_hz = 440.0;
    std::vector<std::pair<uint32_t, double>> sets;
    /* §15.5 offline-edit hook: --set-at SEC:ID=V applies a param mid-render (vs --set at
     * t=0), so a param can be edited AFTER a mid-render device disconnect and we can prove
     * the edit reaches the device on reattach. */
    struct SetAt { double sec; uint32_t id; double v; };
    std::vector<SetAt> setAts;
    std::vector<int> notes;    /* played sequentially at note_period spacing */
    std::vector<int> chord;    /* held from 0.1 s to the end (arp fodder) */
    /* --brightness V: §9.4 per-voice demo. Sends a VST3 Brightness Note
     * Expression (value V, 0..1) on the FIRST chord note only — the shell maps
     * it to a signed Filter-Cutoff mod on that ONE voice (§9.5), so the chord's
     * other voices are unmodulated. <0 = off (no expression emitted). */
    double brightness = -1.0;
    /* --tuning SEMIS: a VST3 Tuning Note Expression (the MPE pitch axis) on chord
     * note `tuning_idx` — the shell maps it to a per-voice pitch bend, so the same
     * SEMIS as CLAP's --bend yields the byte-identical render (cross-format MPE). */
    bool has_tuning = false;
    double tuning_semis = 0.0;
    int tuning_idx = 0;
    int brightness_idx = 0;    /* which chord note (index) gets the Brightness expression;
                                  lets a test modulate voice 0 vs voice 1 of ONE arrangement
                                  to prove the mod is per-voice, not part-wide (§9.5). */
    int channel = 0;           /* MIDI channel 0..15 for emitted notes -> device part (P2.1) */
    int part = -1;             /* -1 = main mix (default); 0..15 = pull that part's stereo pair (P2.2) */
    /* multi-instance (P6): >1 plugin instances in ONE process, one per channel,
     * sharing the device via the P4 registry under a pinned serial. Empty =
     * single-instance, the byte-identical golden/timing/recall path. */
    std::vector<int> alias_channels; /* one entry per instance; channel == device part */
    std::string mt_serial;           /* serial to pin for the shared claim */
    /* §8.8 FX multi-insert: >=1 enables run_fx_instances — N coexisting harp-fx-shell
     * inserts sharing one device serial, owner-first, capturing fx_capture (default
     * the last = the NON-OWNER) to reproduce the Ableton "FX track silent" bug. */
    int fx_instances = 0;       /* 0 = off (single-instance / instrument paths unchanged) */
    int fx_capture = -1;        /* which insert to capture; <0 => last (non-owner) */
    bool fx_drop_owner = false; /* tear insert 0 down before capture (scan instance leaves) */
    /* §14.4 host-context-A: --diag-bundle OUTFILE captures the runtime's diag
     * bundle after the render and writes the CBOR bytes to OUTFILE. The runtime
     * lives inside the dlopen'd plugin (not this host), so we reach it the way
     * the rest of the harness reaches it — an env var the plugin reads when it
     * tears the session down (HARP_DIAG_BUNDLE_OUT), at which point the bundle is
     * captured READ-ONLY off the control path (no audio-path effect). Empty =
     * off, the byte-identical golden path. */
    std::string diag_bundle_path;
    bool diag_bundle_anon = false; /* --diag-bundle-anon: run the §16 anon pass */
    /* §14.3 host LoopbackMeasurer: --loopback IN,OUT runs the digital round-trip
     * probe after the render (mirrors --diag-bundle). IN = the H->D slot the host
     * injects an impulse on, OUT = the D->H slot the device echoes it to (the device
     * copies IN->OUT in the same rendered frame). OUT MUST be a slot the synth is NOT
     * driving with notes (else the echo overwrites real output). -1/-1 = off, the
     * byte-identical golden path. Reached through HARP_LOOPBACK_IN/_OUT, the same
     * cross-plugin-boundary env mechanism --diag-bundle uses. */
    int loopback_in = -1, loopback_out = -1;
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
        else if (a == "--out-buses") g_outBuses = atoi(next().c_str()); /* MULTI-OUT: activate N output buses (main + parts) */
        else if (a == "--capture-bus") { g_captureBus = atoi(next().c_str()); /* MULTI-OUT: capture bus N */
            if (g_outBuses < g_captureBus + 1) g_outBuses = g_captureBus + 1; }
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--out") out_path = next();
        else if (a == "--save-state") save_state_path = next();
        else if (a == "--load-state") load_state_path = next();
        else if (a == "--load-state-after-connect") load_state_after_path = next(); /* §11.4 HIGH #8 */
        else if (a == "--diag-bundle") diag_bundle_path = next(); /* §14.4 host-context-A */
        else if (a == "--diag-bundle-anon") { /* + §16 anon pass */
            diag_bundle_path = next();
            diag_bundle_anon = true;
        }
        else if (a == "--loopback") { /* §14.3: --loopback IN,OUT */
            std::string lv = next();
            if (sscanf(lv.c_str(), "%d,%d", &loopback_in, &loopback_out) != 2)
                die("--loopback wants IN,OUT (e.g. --loopback 5,10)");
        }
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
        } else if (a == "--brightness") {
            brightness = atof(argv[++i]);
        } else if (a == "--brightness-idx") {
            brightness_idx = atoi(argv[++i]);
        } else if (a == "--tuning") {
            tuning_semis = atof(argv[++i]);
            has_tuning = true;
        } else if (a == "--tuning-idx") {
            tuning_idx = atoi(argv[++i]);
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
        } else if (a == "--channel") { /* MIDI channel for emitted notes, clamped 0..15 */
            channel = atoi(next().c_str());
            if (channel < 0) channel = 0;
            if (channel > 15) channel = 15;
        } else if (a == "--part") { /* pull device PART N's stereo pair, not the main mix (P2.2) */
            part = atoi(next().c_str());
            if (part < 0 || part > 15) die("--part wants 0..15");
        } else if (a == "--instances") { /* N plugin instances, channels 0..N-1 (P6) */
            int n = atoi(next().c_str());
            if (n < 1 || n > 16) die("--instances wants 1..16");
            alias_channels.clear();
            for (int k = 0; k < n; k++) alias_channels.push_back(k);
        } else if (a == "--aliases") { /* explicit channel list, one alias each (P6) */
            std::string list = next();
            alias_channels.clear();
            size_t pos = 0;
            while (pos < list.size()) {
                int ch = atoi(list.c_str() + pos);
                if (ch < 0 || ch > 15) die("--aliases wants channels 0..15");
                alias_channels.push_back(ch);
                pos = list.find(',', pos);
                if (pos == std::string::npos) break;
                pos++;
            }
            if (alias_channels.empty()) die("--aliases wants ch0,ch1,..");
        } else if (a == "--serial") { /* serial to pin for the shared claim (P6 / FX) */
            mt_serial = next();
        } else if (a == "--fx-instances") { /* §8.8 FX multi-insert repro: N inserts */
            fx_instances = atoi(next().c_str());
            if (fx_instances < 1 || fx_instances > 16) die("--fx-instances wants 1..16");
        } else if (a == "--fx-capture") { /* which insert to capture (default last/non-owner) */
            fx_capture = atoi(next().c_str());
        } else if (a == "--fx-drop-owner") { /* drop insert 0 (scan instance) before capture */
            fx_drop_owner = true;
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
        } else if (a == "--set-ch") { /* M2: CH:ID=V — set part CH's device param ID (per-channel) */
            std::string kv = next();
            auto colon = kv.find(':');
            auto eq = kv.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon)
                die("--set-ch wants CH:ID=VALUE");
            uint32_t ch = (uint32_t)strtoul(kv.substr(0, colon).c_str(), nullptr, 0);
            uint32_t pid = (uint32_t)strtoul(kv.substr(colon + 1, eq - colon - 1).c_str(), nullptr, 0);
            sets.push_back({perChanParamId(ch, pid), atof(kv.substr(eq + 1).c_str())});
        } else if (a == "--set-at") { /* SEC:ID=V — apply a param mid-render (§15.5 offline edit) */
            std::string kv = next();
            auto colon = kv.find(':');
            auto eq = kv.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon)
                die("--set-at wants SEC:ID=VALUE");
            setAts.push_back({atof(kv.substr(0, colon).c_str()),
                              (uint32_t)strtoul(kv.substr(colon + 1, eq - colon - 1).c_str(), nullptr, 0),
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
        } else if (a == "--part-audio") {
            part_audio = true;
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

    /* --part N: ask the shell runtime to subscribe to device part N's stereo
     * output (slots {2+2N, 3+2N}) instead of the main mix. The runtime lives
     * inside the plugin (a separate module we dlopen), so we reach it the way
     * the rest of the harness does — an env var the runtime reads at start():
     * HARP_OUT_SLOTS, equivalent to calling rt.setOutSlots({2+2N, 3+2N}). It
     * MUST be set before the plugin's setActive(true) below (where start()
     * runs). Without --part we leave it unset -> the runtime's {0,1} main-mix
     * default -> golden byte-identical. Either way the output bus is stereo and
     * we still pull/capture 2 channels. */
    if (part >= 0) {
        char slots[32];
        snprintf(slots, sizeof slots, "%d,%d", 2 + 2 * part, 3 + 2 * part);
#ifdef _WIN32
        _putenv_s("HARP_OUT_SLOTS", slots);
#else
        setenv("HARP_OUT_SLOTS", slots, 1);
#endif
        printf("part: %d -> output slots {%s}\n", part, slots);
    }

    /* --part-audio (P5b, multi-instance): tell every ATTACHED plugin instance to
     * pull its OWN part's audio instead of staying audio-silent. The plugin reads
     * HARP_PART_AUDIO at setActive and registers a per-part AudioSink for its part
     * pair {2+2k,3+2k}; the owner's reader demuxes the shared device stream into
     * it. NOTE (P5b limitation): the audio.start UNION is fixed when the OWNER
     * activates; aliases activating AFTER it (the usual sequential order) register
     * their sink too late for the union and read silence until the next
     * audio.start. So this flag exercises the per-part REGISTRATION/teardown path
     * through the real plugin; the owner's main-mix capture below is unchanged
     * and stays the byte-deterministic e2e signal. Default off => attached aliases
     * are audio-silent exactly as P5, byte-identical. */
    if (part_audio) {
#ifdef _WIN32
        _putenv_s("HARP_PART_AUDIO", "1");
#else
        setenv("HARP_PART_AUDIO", "1", 1);
#endif
        printf("part-audio: attached aliases pull their own part (P5b)\n");
    }

    /* --channel k tags this instance's NOTES (the per-event UMP word, below)
     * AND its PARAM sets/ramps with part k, so --set/--ramp/--lfo drive the
     * SAME part the notes reach — one host fully owns its part (P3). Notes
     * carry the channel in their word directly; params are tagged inside the
     * runtime (it owns the param event encode), so we reach it the same way
     * --part reaches it: an env var read at the plugin's start(). channel 0
     * (the default) leaves it unset -> key omitted -> golden byte-identical. */
    if (channel != 0) {
        char chbuf[8];
        snprintf(chbuf, sizeof chbuf, "%d", channel);
#ifdef _WIN32
        _putenv_s("HARP_CHANNEL", chbuf);
#else
        setenv("HARP_CHANNEL", chbuf, 1);
#endif
        printf("channel: %d (notes + params -> part %d)\n", channel, channel);
    }

    /* §14.4 host-context-A: hand the diag-bundle output path to the plugin via an
     * env var (the runtime lives inside the dlopen'd plugin module). The plugin
     * captures getDiagBundle() and writes it at session teardown — after this
     * render, off the audio path. MUST be set before setActive(true)/(false).
     * The single-instance path reaches the runtime ONLY when a serial is pinned
     * (HARP_DEVICE_SERIAL), so the plugin's owner registers a runtime the capture
     * can read; with no serial the runtime is private and still captured by the
     * SAME owner instance at teardown. Unset env (no flag) is the golden no-op. */
    if (!diag_bundle_path.empty()) {
#ifdef _WIN32
        _putenv_s("HARP_DIAG_BUNDLE_OUT", diag_bundle_path.c_str());
        if (diag_bundle_anon) _putenv_s("HARP_DIAG_BUNDLE_ANON", "1");
#else
        setenv("HARP_DIAG_BUNDLE_OUT", diag_bundle_path.c_str(), 1);
        if (diag_bundle_anon) setenv("HARP_DIAG_BUNDLE_ANON", "1", 1);
#endif
        printf("diag-bundle: -> %s%s\n", diag_bundle_path.c_str(),
               diag_bundle_anon ? " (anonymized, §16)" : "");
    }

    /* §14.3 host LoopbackMeasurer: hand the in/out slots to the plugin's runtime via
     * env vars (same cross-plugin-boundary mechanism --diag-bundle uses). The runtime
     * reads them at start() (arming key 3) and runs measureLoopback() at setActive
     * (false), printing the measured RTT + §6.4 expected + delta. MUST be set before
     * setActive(true)/(false). Unset (no flag) is the byte-identical golden no-op. */
    if (loopback_in >= 0 && loopback_out >= 0) {
        char lin[16], lout[16];
        snprintf(lin, sizeof lin, "%d", loopback_in);
        snprintf(lout, sizeof lout, "%d", loopback_out);
#ifdef _WIN32
        _putenv_s("HARP_LOOPBACK_IN", lin);
        _putenv_s("HARP_LOOPBACK_OUT", lout);
#else
        setenv("HARP_LOOPBACK_IN", lin, 1);
        setenv("HARP_LOOPBACK_OUT", lout, 1);
#endif
        printf("loopback: in-slot=%d out-slot=%d (§14.3 digital round-trip probe)\n",
               loopback_in, loopback_out);
    }

    /* ---- host context + module ---- */
    static HostApplication hostApp;
    PluginContextFactory::instance().setPluginContext(&hostApp);

    std::string err;
    auto module = VST3::Hosting::Module::create(plugin_path, err);
    if (!module) die("cannot load module: " + err);

    auto factory = module->getFactory();
    IPtr<PlugProvider> provider;
    VST3::Hosting::ClassInfo audio_ci; /* bare ClassInfo == Steinberg::PClassInfo here; qualify */
    bool found_ci = false;
    for (auto &ci : factory.classInfos()) {
        if (ci.category() == kVstAudioEffectClass) {
            printf("class: %s (%s) by %s\n", ci.name().c_str(), ci.subCategoriesString().c_str(),
                   ci.vendor().c_str());
            audio_ci = ci;
            found_ci = true;
            break;
        }
    }
    if (!found_ci) die("no audio effect class");

    /* P6 multi-instance: a multitimbral alias GROUP through the real plugin. The
     * single-instance path below is untouched (and unreached) — golden/timing/
     * recall tests pass no --instances/--aliases, so found_ci falls straight into
     * the single PlugProvider setup and renders byte-identically to today. */
    if (!alias_channels.empty()) {
        std::string serial = mt_serial;
        if (serial.empty()) {
            const char *e = getenv("HARP_DEVICE_SERIAL");
            serial = (e && e[0]) ? e : std::string("PI4B-0001");
        }
        return run_multi_instance(module, audio_ci, alias_channels, serial, rate, block, seconds,
                                  bpm, notes, note_period, chord, realtime, out_path, do_hash,
                                  do_json, expect_hash);
    }

    /* §8.8 FX multi-insert repro: N coexisting harp-fx-shell inserts sharing one
     * device serial. The single-instance path below is untouched (and unreached) —
     * the golden/recall tests pass no --fx-instances, so they fall straight through. */
    if (fx_instances >= 1) {
        std::string serial = mt_serial;
        if (serial.empty()) {
            const char *e = getenv("HARP_DEVICE_SERIAL");
            serial = (e && e[0]) ? e : std::string("KR260-FX01");
        }
        return run_fx_instances(module, audio_ci, fx_instances, fx_capture, fx_drop_owner, serial,
                                rate, block, seconds, input_kind, sine_hz, realtime, out_path,
                                do_hash, do_json, expect_hash);
    }

    provider = owned(new PlugProvider(factory, audio_ci, true));
    if (!provider || !provider->initialize()) die("no audio effect class / init failed");

    OPtr<IComponent> component = provider->getComponent();
    OPtr<IEditController> controller = provider->getController();
    if (!component) die("no component");
    FUnknownPtr<IAudioProcessor> processor(component);
    if (!processor) die("component is not an IAudioProcessor");

    /* ---- restore state (as a DAW project-open would) ----
     * The restore plumbing is shared by both staging paths: --load-state runs it HERE
     * (pre-activate, the disconnected branch) and --load-state-after-connect (§11.4 HIGH
     * #8) defers the same call to AFTER setActive(true) so it hits setStateBundle's
     * connected() branch. The only difference is WHEN setState fires, so the body lives
     * in one lambda. */
    auto restore_state = [&](const std::string &path) {
        std::vector<char> comp, ctrl;
        if (!load_state_file(path, comp, ctrl)) die("cannot read state file");
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
        printf("state: restored from %s (%zu+%zu bytes)\n", path.c_str(),
               comp.size(), ctrl.size());
    };
    if (!load_state_path.empty()) restore_state(load_state_path);

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
        /* MULTI-OUT (M5): enumerate every declared OUTPUT bus by name + channel count,
         * so main-multi-output-test can assert the 17-bus whole-device layout (bus 0
         * main mix + buses 1..16 per-part, all stereo). */
        int32 nob = component->getBusCount(kAudio, kOutput);
        printf("output-buses (%d):\n", nob);
        for (int32 i = 0; i < nob; i++) {
            BusInfo b{};
            component->getBusInfo(kAudio, kOutput, i, b);
            printf("  out-bus[%d] \"%s\" channels=%d\n", i,
                   VST3::StringConvert::convert(b.name).c_str(), b.channelCount);
        }
    }

    /* ---- bus setup ---- */
    int32 nin = component->getBusCount(kAudio, kInput);
    int32 nout = component->getBusCount(kAudio, kOutput);
    for (int32 i = 0; i < nin; i++) component->activateBus(kAudio, kInput, i, true);
    /* MULTI-OUT: activate the main mix (bus 0) plus the first g_outBuses-1 per-part buses
     * (default 1 = main-mix only, the golden-identical path). Activating ALL declared output
     * buses would make a multi-out shell stream the full 34-channel union even for a
     * main-mix-only render. The multi-out test raises g_outBuses via --out-buses N. */
    int32 actOut = nout < g_outBuses ? nout : g_outBuses;
    for (int32 i = 0; i < actOut; i++) component->activateBus(kAudio, kOutput, i, true);
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

    /* Negotiate speaker arrangements the way a DAW does (Ableton offers stereo/stereo
     * before activating) — BEFORE setupProcessing/setActive. For the instrument (no
     * input bus) this matches the declared stereo out, the SDK base accepts, and the
     * render is byte-identical; for the FX it confirms the stereo-in/stereo-out layout
     * Ableton routes track audio through. */
    negotiate_buses(component.get(), processor, true);

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

    /* §11.4 staged-while-connected (HIGH #8): restore the bundle AFTER the connect.
     * Unlike the pre-activate --load-state above, the runtime is now live/connected, so
     * setStateBundle takes its connected() branch (#73). Same restore plumbing, deferred. */
    if (!load_state_after_path.empty()) restore_state(load_state_after_path);

    /* §6.4 reported PDC latency the DAW sees via setLatencySamples — queried AFTER
     * setActive(true) so it reflects the live owner runtime (audit gap #4 item 3).
     * Invariant for the run; printed once. */
    uint32 reported_latency = processor->getLatencySamples();
    printf("latency: reported-samples=%u block=%u rate=%u\n", reported_latency, block, rate);

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
                /* automation point to the processor only (DAW-representative; see --set) */
                if (q) q->addPoint(0, info.defaultNormalizedValue, pp);
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
                /* Deliver --set the way a DAW delivers automation: a sample-accurate
                 * point to the PROCESSOR. A host does NOT call the plugin's
                 * controller->setParamNormalized to feed the audio engine (the
                 * processor reads inputParameterChanges; the host manages UI display
                 * itself) — so we don't either. */
                if (q) q->addPoint(0, kv.second, pidx);
            }
            first_block = false;
        }
        for (auto &sa : setAts) { /* §15.5: sample-accurate param edit mid-render (e.g. while the device is unplugged) */
            size_t at = (size_t)(sa.sec * rate);
            if (at >= done && at < done + n) {
                int32 qidx = 0;
                auto *q = pc.addParameterData(sa.id, qidx);
                int32 pidx = 0;
                if (q) q->addPoint((int32)(at - done), sa.v, pidx);
            }
        }
        pd.inputParameterChanges = &pc;
        ParameterChanges opc; /* plugin-originated changes (e.g. HARP echo) */
        pd.outputParameterChanges = &opc;

        EventList evList; /* scheduled --notes falling inside this block */
        if (!chord.empty()) {
            size_t onAt = (size_t)(0.1 * rate);
            bool last = done + n >= total;
            int32 nid = 0; /* a stable per-note id so a Note Expression can target one */
            for (int cn : chord) {
                if (onAt >= done && onAt < done + n) {
                    Event ev{};
                    ev.type = Event::kNoteOnEvent;
                    ev.sampleOffset = (int32)(onAt - done);
                    ev.noteOn.channel = (int16)channel;
                    ev.noteOn.pitch = (int16)cn;
                    ev.noteOn.velocity = 0.8f;
                    ev.noteOn.noteId = nid;
                    evList.addEvent(ev);
                    /* --brightness: one Brightness expression on chord note
                     * `brightness_idx` (default 0), in the same block right after
                     * its note-on, so the shell mods that ONE voice's cutoff (§9.5). */
                    if (brightness >= 0.0 && nid == brightness_idx) {
                        Event nx{};
                        nx.type = Event::kNoteExpressionValueEvent;
                        nx.sampleOffset = ev.sampleOffset;
                        nx.noteExpressionValue.typeId = kBrightnessTypeID;
                        nx.noteExpressionValue.noteId = nid;
                        nx.noteExpressionValue.value = brightness;
                        evList.addEvent(nx);
                    }
                    /* --tuning: a Tuning expression (MPE pitch) on chord note
                     * `tuning_idx`. VST3 Tuning is normalized about 0.5 with a
                     * ±120-semitone full range, so SEMIS -> value 0.5 + semis/240
                     * (the shell inverts it back to semitones). */
                    if (has_tuning && nid == tuning_idx) {
                        Event nx{};
                        nx.type = Event::kNoteExpressionValueEvent;
                        nx.sampleOffset = ev.sampleOffset;
                        nx.noteExpressionValue.typeId = kTuningTypeID;
                        nx.noteExpressionValue.noteId = nid;
                        nx.noteExpressionValue.value = 0.5 + tuning_semis / 240.0;
                        evList.addEvent(nx);
                    }
                }
                if (last) {
                    Event ev{};
                    ev.type = Event::kNoteOffEvent;
                    ev.sampleOffset = (int32)(n > 0 ? n - 1 : 0);
                    ev.noteOff.channel = (int16)channel;
                    ev.noteOff.pitch = (int16)cn;
                    ev.noteOff.velocity = 0;
                    ev.noteOff.noteId = nid;
                    evList.addEvent(ev);
                }
                nid++;
            }
        }
        for (size_t ni = 0; ni < notes.size(); ni++) {
            int64_t on_at = (int64_t)((double)ni * note_period * rate);
            int64_t off_at = on_at + (int64_t)(0.75 * note_period * rate);
            if (on_at >= (int64_t)done && on_at < (int64_t)(done + n)) {
                Event ev{};
                ev.type = Event::kNoteOnEvent;
                ev.sampleOffset = (int32)(on_at - (int64_t)done);
                ev.noteOn.channel = (int16)channel;
                ev.noteOn.pitch = (int16)notes[ni];
                ev.noteOn.velocity = 0.9f;
                ev.noteOn.noteId = -1;
                evList.addEvent(ev);
            }
            if (off_at >= (int64_t)done && off_at < (int64_t)(done + n)) {
                Event ev{};
                ev.type = Event::kNoteOffEvent;
                ev.sampleOffset = (int32)(off_at - (int64_t)done);
                ev.noteOff.channel = (int16)channel;
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

        /* MULTI-OUT: capture the selected output bus (default 0 = main mix). The shell writes
         * explicit silence to a routed-but-idle part bus, so capturing one proves isolation. */
        int32 cb = (g_captureBus < pd.numOutputs && pd.outputs[g_captureBus].channelBuffers32 &&
                    pd.outputs[g_captureBus].channelBuffers32[0])
                       ? g_captureBus
                       : 0;
        for (size_t s = 0; s < n; s++)
            for (int32 c = 0; c < out_ch; c++)
                capture.push_back(pd.outputs[cb].channelBuffers32[c][s]);
        if (bpm > 0) ctx.projectTimeMusic += (double)n * bpm / (60.0 * rate);
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

    uint64_t hash = harp_fnv1a(capture.data(), capture.size() * sizeof(float));
    char hashhex[17];
    snprintf(hashhex, sizeof hashhex, "%016llx", (unsigned long long)hash);
    if (do_hash) printf("output-hash: %s\n", hashhex);
    if (do_json)
        printf("{\"frames\":%zu,\"channels\":%d,\"rate\":%u,\"rms\":%.6f,\"hash\":\"%s\",\"reported_latency_samples\":%u}\n",
               capture.size() / (size_t)(out_ch ? out_ch : 1), out_ch, rate, rms, hashhex, reported_latency);

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
