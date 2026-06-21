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

/* MPE host-side param ids — mirrored from shell/shell_constants.h (this host keeps
 * local copies of the shell's host param ids, like kPartParamId, rather than
 * pulling the shell header). The "MPE" toggle (97) arms raw-MIDI MPE; each
 * (channel, axis) maps to a hidden param the shell decodes back into a §9.5 mod. */
static const uint32_t kMpeEnableParamId = 97;
static const uint32_t kMpeMidiBase = 0x3000u;
static const uint32_t kMpeMidiAxes = 4u;
enum { kMpeAxisBend = 0, kMpeAxisTimbre = 1, kMpeAxisPressure = 2 };
static inline uint32_t mpeMidiId(uint32_t chan, uint32_t axis) {
    return kMpeMidiBase + (chan & 0xf) * kMpeMidiAxes + axis;
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

/* ---- multi-instance (P6): a multitimbral alias GROUP through the REAL plugin ----
 *
 * The single-instance host above loads ONE plugin/component/controller and
 * renders it. This drives N plugin instances created from the SAME module
 * factory, in ONE process, the way several plugin tracks in one DAW would —
 * and the way the runtime-direct tsan-host already does at the runtime layer,
 * but here through the FULL VST3 plugin chain (component/controller/processor,
 * setActive, process()).
 *
 * SHARING: HARP_DEVICE_SERIAL is pinned for the whole process before any
 * setActive(true). Every instance's plugin reads it, hands it to the P4
 * registry (runtime_acquire), and so all N instances RIDE ONE shared runtime /
 * ONE USB claim. The FIRST instance to activate is the registry owner (it
 * configures/starts the session and pulls the main mix); the rest ATTACH —
 * each registers its own event source on its own part and injects notes there,
 * which the owner's eventPump merges onto the one session (P5). So the group
 * plays multitimbrally and the owner's main mix SUMS every engaged part.
 *
 * PART ROUTING: each instance gets its channel as its device part two ways,
 * matching the plugin's design — (a) its NOTE events carry that channel in the
 * UMP word (plugin.cpp routes ev.noteOn.channel -> device part directly), and
 * (b) its Part param (id 98, 0..15) is set via setParamNormalized(98, ch/15),
 * the recall-safe per-instance channel the plugin task adds (replacing the
 * process-global HARP_CHANNEL env that would otherwise collapse every alias
 * onto one part). We set BOTH so this works whether routing is by event
 * channel or by the persisted Part param.
 *
 * CAPTURE: only the OWNER's process() output is captured — that is the device
 * main mix (attached instances are audio-silent per P5). The captured mix of
 * an N-alias group differs from a 1-alias (owner-only) run because more parts
 * are summed: the same play-proof signal alias-play-test.sh measures, but
 * produced through the real plugin chain rather than the runtime directly.
 *
 * Returns the process exit code. Single-instance invocations never reach here. */
static int run_multi_instance(VST3::Hosting::Module::Ptr &module, const VST3::Hosting::ClassInfo &ci,
                              const std::vector<int> &channels, const std::string &serial,
                              uint32_t rate, uint32_t block, double seconds, double bpm,
                              const std::vector<int> &notes, double note_period,
                              const std::vector<int> &chord, bool realtime,
                              const std::string &out_path, bool do_hash, bool do_json,
                              const std::string &expect_hash) {
    const int N = (int)channels.size();
    /* Part param the plugin task adds (id 98, 0..15): we set it per instance to
     * route that instance to its device part, recall-safe. */
    static const uint32_t kPartParamId = 98;

    /* Pin the serial for the whole process: every instance's plugin reads
     * HARP_DEVICE_SERIAL at setActive(true) and hands it to the registry, so all
     * N share ONE runtime / ONE claim. MUST be set before any setActive below. */
#ifdef _WIN32
    _putenv_s("HARP_DEVICE_SERIAL", serial.c_str());
#else
    setenv("HARP_DEVICE_SERIAL", serial.c_str(), 1);
#endif
    printf("multi-instance: %d aliases, serial=%s, channels=", N, serial.c_str());
    for (int k = 0; k < N; k++) printf("%s%d", k ? "," : "", channels[k]);
    printf("\n");

    auto factory = module->getFactory();

    /* One full plugin instance per alias: PlugProvider news a fresh component +
     * controller pair from the same factory (so N components/processors/
     * controllers exist at once), exactly as N plugin tracks would. */
    struct Inst {
        IPtr<PlugProvider> provider;
        OPtr<IComponent> component;
        OPtr<IEditController> controller;
        FUnknownPtr<IAudioProcessor> processor{nullptr};
        HostProcessData pd;
        int32 out_ch = 0, nin = 0, in_ch = 0, nout = 0;
        int channel = 0;
    };
    std::vector<std::unique_ptr<Inst>> insts;
    insts.reserve(N);

    for (int k = 0; k < N; k++) {
        auto in = std::make_unique<Inst>();
        in->channel = channels[k];
        in->provider = owned(new PlugProvider(factory, ci, true));
        if (!in->provider || !in->provider->initialize())
            die("multi-instance: provider init failed for alias " + std::to_string(k));
        in->component = in->provider->getComponent();
        in->controller = in->provider->getController();
        if (!in->component) die("multi-instance: no component for alias " + std::to_string(k));
        in->processor = FUnknownPtr<IAudioProcessor>(in->component);
        if (!in->processor) die("multi-instance: component is not an IAudioProcessor");

        in->nin = in->component->getBusCount(kAudio, kInput);
        in->nout = in->component->getBusCount(kAudio, kOutput);
        for (int32 i = 0; i < in->nin; i++) in->component->activateBus(kAudio, kInput, i, true);
        for (int32 i = 0; i < in->nout; i++) in->component->activateBus(kAudio, kOutput, i, true);
        if (in->nout == 0) die("multi-instance: alias has no audio output bus");
        BusInfo outBus{};
        in->component->getBusInfo(kAudio, kOutput, 0, outBus);
        in->out_ch = outBus.channelCount;
        if (in->nin > 0) {
            BusInfo inBus{};
            in->component->getBusInfo(kAudio, kInput, 0, inBus);
            in->in_ch = inBus.channelCount;
        }

        ProcessSetup setup{realtime ? kRealtime : kOffline, kSample32, (int32)block,
                           (SampleRate)rate};
        if (in->processor->setupProcessing(setup) != kResultOk)
            die("multi-instance: setupProcessing failed for alias " + std::to_string(k));
        if (!in->pd.prepare(*in->component, (int32)block, kSample32))
            die("multi-instance: process data prepare failed");

        /* Route this instance to its part via the Part param BEFORE setActive,
         * so the runtime sees the channel as it starts its session; we also feed
         * it as a block-0 automation point below. setParamNormalized on a plugin
         * that lacks id 98 yet is a harmless no-op (kResultFalse), so the host
         * still builds/runs against the param before the sibling task lands. */
        if (in->controller)
            in->controller->setParamNormalized(kPartParamId, in->channel / 15.0);
        insts.push_back(std::move(in));
    }

    /* Activate in order: the FIRST instance becomes the registry owner (it
     * acquires the runtime for the pinned serial, configures + starts the shared
     * session, and pulls the main mix); the rest attach inside their own
     * setActive. setProcessing(true) on all. */
    for (int k = 0; k < N; k++) {
        if (insts[k]->component->setActive(true) != kResultOk)
            die("multi-instance: setActive failed for alias " + std::to_string(k));
        insts[k]->processor->setProcessing(true);
    }
    printf("multi-instance: %d aliases active (alias 0 = owner / main mix)\n", N);

    /* Shared transport context (owner-anchored inside the plugin). */
    ProcessContext ctx{};
    ctx.sampleRate = rate;
    ctx.state = ProcessContext::kPlaying;
    if (bpm > 0) {
        ctx.state |= ProcessContext::kTempoValid | ProcessContext::kProjectTimeMusicValid;
        ctx.tempo = bpm;
        ctx.projectTimeMusic = 0;
    }
    for (int k = 0; k < N; k++) insts[k]->pd.processContext = &ctx;

    const int32 owner_ch = insts[0]->out_ch;
    size_t total = (size_t)(seconds * rate);
    size_t done = 0;
    std::vector<float> capture; /* OWNER main mix only */
    capture.reserve(total * (size_t)owner_ch);
    std::vector<bool> first_block(N, true);

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

        /* Per-instance param-change + event lists must outlive process(): one
         * vector entry per alias, kept alive for the whole block. */
        std::vector<ParameterChanges> pcs(N), opcs(N);
        std::vector<EventList> evs(N);

        for (int k = 0; k < N; k++) {
            Inst &in = *insts[k];
            in.pd.numSamples = (int32)n;

            /* block-0: pin this alias's Part param so the device routes it. */
            if (first_block[k]) {
                int32 qi = 0;
                auto *q = pcs[k].addParameterData(kPartParamId, qi);
                int32 pi = 0;
                if (q) q->addPoint(0, in.channel / 15.0, pi);
                first_block[k] = false;
            }
            in.pd.inputParameterChanges = &pcs[k];
            in.pd.outputParameterChanges = &opcs[k];

            /* notes/chord on THIS alias's channel == its device part. Each alias
             * gets its base note transposed by its channel so the parts are
             * distinct pitches — a real multitimbral spread, not a unison. */
            if (!chord.empty()) {
                size_t onAt = (size_t)(0.1 * rate);
                bool last = done + n >= total;
                for (int cn : chord) {
                    int pitch = cn + in.channel;
                    if (onAt >= done && onAt < done + n) {
                        Event ev{};
                        ev.type = Event::kNoteOnEvent;
                        ev.sampleOffset = (int32)(onAt - done);
                        ev.noteOn.channel = (int16)in.channel;
                        ev.noteOn.pitch = (int16)pitch;
                        ev.noteOn.velocity = 0.8f;
                        ev.noteOn.noteId = -1;
                        evs[k].addEvent(ev);
                    }
                    if (last) {
                        Event ev{};
                        ev.type = Event::kNoteOffEvent;
                        ev.sampleOffset = (int32)(n > 0 ? n - 1 : 0);
                        ev.noteOff.channel = (int16)in.channel;
                        ev.noteOff.pitch = (int16)pitch;
                        ev.noteOff.velocity = 0;
                        ev.noteOff.noteId = -1;
                        evs[k].addEvent(ev);
                    }
                }
            }
            for (size_t ni = 0; ni < notes.size(); ni++) {
                int64_t on_at = (int64_t)((double)ni * note_period * rate);
                int64_t off_at = on_at + (int64_t)(0.75 * note_period * rate);
                int pitch = notes[ni] + in.channel;
                if (on_at >= (int64_t)done && on_at < (int64_t)(done + n)) {
                    Event ev{};
                    ev.type = Event::kNoteOnEvent;
                    ev.sampleOffset = (int32)(on_at - (int64_t)done);
                    ev.noteOn.channel = (int16)in.channel;
                    ev.noteOn.pitch = (int16)pitch;
                    ev.noteOn.velocity = 0.9f;
                    ev.noteOn.noteId = -1;
                    evs[k].addEvent(ev);
                }
                if (off_at >= (int64_t)done && off_at < (int64_t)(done + n)) {
                    Event ev{};
                    ev.type = Event::kNoteOffEvent;
                    ev.sampleOffset = (int32)(off_at - (int64_t)done);
                    ev.noteOff.channel = (int16)in.channel;
                    ev.noteOff.pitch = (int16)pitch;
                    ev.noteOff.velocity = 0.f;
                    ev.noteOff.noteId = -1;
                    evs[k].addEvent(ev);
                }
            }
            in.pd.inputEvents = &evs[k];
        }

        /* Drive every alias this block. The owner (alias 0) renders the main mix
         * that sums all engaged parts; attached aliases are audio-silent (P5) but
         * their notes/Part param reach the shared session via the owner's pump. */
        for (int k = 0; k < N; k++) {
            if (insts[k]->processor->process(insts[k]->pd) != kResultOk)
                die("multi-instance: process failed for alias " + std::to_string(k));
        }

        /* Capture ONLY the owner's output = the device main mix. */
        for (size_t s = 0; s < n; s++)
            for (int32 c = 0; c < owner_ch; c++)
                capture.push_back(insts[0]->pd.outputs[0].channelBuffers32[c][s]);

        if (bpm > 0) ctx.projectTimeMusic += (double)n * bpm / (60.0 * rate);
        done += n;
        ctx.projectTimeSamples += (TSamples)n;
    }

    for (int k = 0; k < N; k++) insts[k]->processor->setProcessing(false);

    /* Teardown in REVERSE order: attached aliases deactivate (releasing their
     * registry handle + event source) before the owner, so the owner — the last
     * holder — is the one that stops + destroys the shared runtime. This reverse
     * setActive(false) loop IS the teardown; the later destruction of the Inst
     * objects (vector unwind, forward order) only terminates the now-idle
     * component/controllers — the shared runtime is already gone, so that order
     * is immaterial. */
    for (int k = N - 1; k >= 0; k--) insts[k]->component->setActive(false);

    /* ---- output (owner main mix), same oracle as the single-instance path ---- */
    double rms = 0;
    for (float v : capture) rms += (double)v * v;
    rms = capture.empty() ? 0 : sqrt(rms / capture.size());
    printf("processed %zu samples x %d ch (owner main mix), rms=%.5f\n", done, owner_ch, rms);

    uint64_t hash = harp_fnv1a(capture.data(), capture.size() * sizeof(float));
    char hashhex[17];
    snprintf(hashhex, sizeof hashhex, "%016llx", (unsigned long long)hash);
    if (do_hash) printf("output-hash: %s\n", hashhex);
    if (do_json)
        printf("{\"frames\":%zu,\"channels\":%d,\"rate\":%u,\"rms\":%.6f,\"hash\":\"%s\","
               "\"instances\":%d}\n",
               capture.size() / (size_t)(owner_ch ? owner_ch : 1), owner_ch, rate, rms, hashhex, N);

    if (!out_path.empty()) {
        if (!harp_write_wav16(out_path, capture, (uint32_t)owner_ch, rate))
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
                "       [--diag-bundle FILE | --diag-bundle-anon FILE]\n"
                "       [--loopback IN,OUT]\n"
                "       [--instances N | --aliases ch0,ch1,..] [--serial SERIAL]\n"
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
                "             HARP_DEVICE_SERIAL or PI4B-0001); multi-instance only\n");
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
    /* --mpe-chord: classic RAW-MIDI MPE (Ableton Live on VST3). Each note plays on
     * its OWN lower-zone member channel (ch1, ch2, …) with the "MPE" toggle armed,
     * so the shell's mpe_zone collapses the zone onto the instance part; per-note
     * pitch/timbre/pressure ride as IMidiMapping param changes on that member
     * channel (the host's job in a real DAW). --mpe-bend SEMIS scaled over the
     * default ±48 member range, so the same SEMIS as CLAP/AU renders identically. */
    std::vector<int> mpe_chord;
    bool mpe_no_arm = false;   /* play mpe-chord notes WITHOUT arming the toggle,
                                  so MPE engages ONLY from a --load-state'd project
                                  (proves the toggle persists in the recall state) */
    bool has_mpe_bend = false, has_mpe_press = false, has_mpe_timbre = false;
    double mpe_bend = 0.0, mpe_press = 0.0, mpe_timbre = 0.0;
    int mpe_bend_idx = 0, mpe_press_idx = 0, mpe_timbre_idx = 0;
    int channel = 0;           /* MIDI channel 0..15 for emitted notes -> device part (P2.1) */
    int part = -1;             /* -1 = main mix (default); 0..15 = pull that part's stereo pair (P2.2) */
    /* multi-instance (P6): >1 plugin instances in ONE process, one per channel,
     * sharing the device via the P4 registry under a pinned serial. Empty =
     * single-instance, the byte-identical golden/timing/recall path. */
    std::vector<int> alias_channels; /* one entry per instance; channel == device part */
    std::string mt_serial;           /* serial to pin for the shared claim */
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
        else if (a == "--seconds") seconds = atof(next().c_str());
        else if (a == "--out") out_path = next();
        else if (a == "--save-state") save_state_path = next();
        else if (a == "--load-state") load_state_path = next();
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
        } else if (a == "--mpe-chord") {
            std::string list = argv[++i];
            size_t pos = 0;
            while (pos < list.size()) {
                mpe_chord.push_back(atoi(list.c_str() + pos));
                pos = list.find(',', pos);
                if (pos == std::string::npos) break;
                pos++;
            }
        } else if (a == "--mpe-bend") {
            mpe_bend = atof(argv[++i]);
            has_mpe_bend = true;
        } else if (a == "--mpe-bend-idx") {
            mpe_bend_idx = atoi(argv[++i]);
        } else if (a == "--mpe-press") {
            mpe_press = atof(argv[++i]);
            has_mpe_press = true;
        } else if (a == "--mpe-press-idx") {
            mpe_press_idx = atoi(argv[++i]);
        } else if (a == "--mpe-timbre") {
            mpe_timbre = atof(argv[++i]);
            has_mpe_timbre = true;
        } else if (a == "--mpe-timbre-idx") {
            mpe_timbre_idx = atoi(argv[++i]);
        } else if (a == "--mpe-no-arm") {
            mpe_no_arm = true;
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
        } else if (a == "--serial") { /* serial to pin for the shared claim (P6) */
            mt_serial = next();
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

    /* --mpe-chord arms the "MPE" toggle (id 97) at the first block — set before the
     * notes (at 0.1 s) so the zone is live when they arrive and they collapse.
     * --mpe-no-arm skips this, so MPE must come from a --load-state'd project (a
     * persistence test): the toggle rides bit 7 of the recall part byte. */
    if (!mpe_chord.empty() && !mpe_no_arm) sets.push_back({kMpeEnableParamId, 1.0});

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

    provider = owned(new PlugProvider(factory, audio_ci, true));
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
        /* classic RAW-MIDI MPE: each note on its OWN lower-zone member channel
         * (ch1, ch2, …), the "MPE" toggle already armed (via sets), so the shell
         * collapses the zone onto the instance part. Per-note expression rides as
         * IMidiMapping param changes on that member channel, queued in THIS block
         * at the note offset — the shell processes the note-on (events) before the
         * param changes, so the voice is minted before its bend lands on it. */
        if (!mpe_chord.empty()) {
            size_t onAt = (size_t)(0.1 * rate);
            bool last = done + n >= total;
            for (size_t ci = 0; ci < mpe_chord.size(); ci++) {
                int16 ch = (int16)((ci + 1) & 0xf); /* lower-zone member channel */
                if (onAt >= done && onAt < done + n) {
                    int32 soff = (int32)(onAt - done);
                    Event ev{};
                    ev.type = Event::kNoteOnEvent;
                    ev.sampleOffset = soff;
                    ev.noteOn.channel = ch;
                    ev.noteOn.pitch = (int16)mpe_chord[ci];
                    ev.noteOn.velocity = 0.8f; /* == --chord, for a matching mix */
                    ev.noteOn.noteId = -1;
                    evList.addEvent(ev);
                    auto addExpr = [&](uint32_t axis, double val) {
                        int32 qi = 0;
                        auto *q = pc.addParameterData(mpeMidiId((uint32_t)ch, axis), qi);
                        int32 pi = 0;
                        if (q) q->addPoint(soff, val, pi);
                    };
                    if (has_mpe_bend && (int)ci == mpe_bend_idx) {
                        /* SEMIS over the default ±48 member range -> 14-bit -> 0..1
                         * (the shell inverts: v14 = round(value*16383)). */
                        double v14 = 8192.0 + mpe_bend / 48.0 * 8192.0;
                        if (v14 < 0) v14 = 0;
                        if (v14 > 16383) v14 = 16383;
                        addExpr(kMpeAxisBend, v14 / 16383.0);
                    }
                    if (has_mpe_press && (int)ci == mpe_press_idx)
                        addExpr(kMpeAxisPressure, mpe_press); /* 0..1 -> CC value*/
                    if (has_mpe_timbre && (int)ci == mpe_timbre_idx)
                        addExpr(kMpeAxisTimbre, mpe_timbre); /* 0..1 -> CC74 */
                }
                if (last) {
                    Event ev{};
                    ev.type = Event::kNoteOffEvent;
                    ev.sampleOffset = (int32)(n > 0 ? n - 1 : 0);
                    ev.noteOff.channel = ch;
                    ev.noteOff.pitch = (int16)mpe_chord[ci];
                    ev.noteOff.velocity = 0;
                    ev.noteOff.noteId = -1;
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

        for (size_t s = 0; s < n; s++)
            for (int32 c = 0; c < out_ch; c++)
                capture.push_back(pd.outputs[0].channelBuffers32[c][s]);
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
