/* HARP RefDev — VST3 shell (§15): presents a HARP device to a DAW.
 *
 * One stereo output, host-paced audio via the embedded runtime, the 12
 * device params as automatable parameters (automation becomes §9.4
 * ramps, notes travel as §9.10 UMP, ProcessContext becomes §9.7
 * transport), getState/setState = Recall Bundle. The AU shell
 * (shell/au) wraps the same runtime; both render byte-identically.
 * Remaining spec deviation, by design: no four-actions UI yet —
 * mismatches auto-resolve by Push-with-archive, which is loss-free.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstnoteexpression.h" /* kBrightnessTypeID (§9.4 mod) */
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"

#include "note_voice_map.h"
#include "runtime.h"
#include "runtime_registry.h"
#include "shell_config.h" /* per-product identity/params/device-filter (default = refdev) */
#include "shell_constants.h"
#include "ump.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

/* Frozen identity — see docs/vst3-shell-plan.md. NEVER change. Values come from
 * shell_config.h (default = these refdev UIDs; a downstream product overrides). */
static const FUID kHarpProcessorUID(HARP_SHELL_PROC_FUID);
static const FUID kHarpControllerUID(HARP_SHELL_CTRL_FUID);

/* The device parameter set (shell_config.h HARP_SHELL_PARAMS; default = refdev).
 * `labels` is nullptr for a plain param, or a "A|B|C" pipe-delimited enum for a
 * named picker (registered as a StringListParameter when HARP_SHELL_LABELED_PARAMS
 * is defined). Replaced by evt.params descriptors once the event plane lands. */
struct DevParam {
    uint32_t id;
    const char *name;
    int32 stepCount;   /* 0 = continuous (VST3: stepCount = steps - 1) */
    double defaultVal; /* must mirror the device defaults (recall sanity) */
    const char *labels; /* nullptr, or "A|B|C" enum labels (stepCount+1 of them) */
};
/* The refdev default set lives in shell_config.h's HARP_SHELL_PARAMS (a downstream
 * product overrides it). It MUST mirror the device's contiguous 1..12 ids (engine
 * 2.1.0: the drone's old id 7 / phantom "FX Send" is gone, the set renumbered with
 * no hole) so id == array index + 1 holds — see process()'s inputParameterChanges. */
static const DevParam kParams[] = { HARP_SHELL_PARAMS };
static constexpr int kNumParams = sizeof(kParams) / sizeof(kParams[0]);
#ifdef HARP_SHELL_ENGINE_TABLES
/* Per-engine slot labels (a product that defines HARP_SHELL_ENGINE_TABLES): row e =
 * engine e's titles, column k = the title for param id (k+1); "—" hides the slot. The
 * controller re-titles ids 2..kNumParams from this when the Engine param changes, so
 * the UI shows the selected engine's REAL controls. Mirrors the device's per-engine
 * param map; harp ships none — the refdev keeps its single static name set. */
static const char *const kEngineTables[][kNumParams] = { HARP_SHELL_ENGINE_TABLES };
static constexpr int kNumEngineTables = (int)(sizeof(kEngineTables) / sizeof(kEngineTables[0]));
static inline bool engSlotHidden(const char *s) { return s && strcmp(s, "—") == 0; }
#endif
/* HOST-SIDE routing parameter (NOT a device param): which multitimbral PART
 * (§9.4 channel 0..15) this plugin instance owns. Stepped+automatable so a DAW
 * can show/persist it per-instance and several aliases each pick a distinct
 * part (the P5c gap — see setActive). It must NOT enter the device param-set
 * path (process() special-cases it, exactly like kPanicParamId) and must NOT
 * affect param-map-hash (it isn't a device param).
 *
 * kPartParamId / kPartStepCount and the recall component-state header
 * (kStateHeaderMagic / kStateHeaderLen) are SHARED with the AU shell via
 * shell_constants.h — both formats must agree for a project to move between them
 * (cross-format-recall-test.sh). See that header for why 'H'=0x48 can never start
 * a recall bundle (so an old header-less state migrates to Part 0). */
/* hidden parameter the DAW's panic (CC 120/123) maps onto via IMidiMapping
 * (VST3-only — the AU surfaces panic through MIDI CC directly, no param) */
static constexpr uint32_t kPanicParamId = 99;

/* §9.9 OUTPUT METERS. The device's readonly per-part + main-mix peak/RMS meters
 * (id range 0x1000+, see shell_constants.h) are surfaced as READONLY host
 * parameters: registered kIsReadOnly (which per the VST3 SDK implies NOT
 * automatable), so a DAW shows live meters in its generic UI but cannot offer or
 * record them as automation (§9.9). They are NEVER written by the shell — the
 * device echoes their values through the SAME evt 'param' path the front-panel
 * echo uses, and process() routes those echoes to outputParameterChanges exactly
 * like a panel knob move (the id self-encodes the part/slot/metric). Because they
 * are additive + readonly + never on the render/event path, the single-instance
 * golden render is byte-identical (the determinism gate). */
static_assert(kMeterIdBase == 0x1000u && kNumMeterParams == 34,
              "meter id scheme must mirror device/device.h (METER_ID_BASE/NSLOTS)");

/* Human-readable meter param name into `buf` (e.g. "Meter Part 3 Peak",
 * "Meter Main RMS"). slot 0..15 = parts, slot 16 = the summed main mix; the low
 * id bit picks peak (0) vs rms (1). */
static void meterParamName(uint32_t id, char *buf, size_t n) {
    uint32_t k = id - kMeterIdBase;
    uint32_t slot = k / 2, metric = k & 1;
    const char *mname = metric ? "RMS" : "Peak";
    if (slot == kMeterMainSlot)
        snprintf(buf, n, "Meter Main %s", mname);
    else
        snprintf(buf, n, "Meter Part %u %s", slot, mname);
}

/* ---------------- processor ---------------- */

class HarpProcessor : public AudioEffect {
public:
    HarpProcessor() { setControllerClass(kHarpControllerUID); }

    /* Defensive teardown: the host calls setActive(false) before destroying us
     * (which resets rt_), but if it doesn't, tear down here so the private
     * runtime is not leaked — the same net cleanup the old by-value member's
     * ~HarpRuntime gave. */
    ~HarpProcessor() override {
        releaseSource(); /* drop our audio sinks before the runtime */
        rt_.reset();
    }

    static FUnknown *createInstance(void *) {
        return (IAudioProcessor *)new HarpProcessor();
    }

    tresult PLUGIN_API initialize(FUnknown *context) override {
        tresult r = AudioEffect::initialize(context);
        if (r != kResultOk) return r;
        /* MULTI-OUT (M1): a Kontakt/Overbridge-style multi-out instrument. Bus 0 is the
         * summed MAIN MIX (kMain, default-active — the byte-identical single-output path);
         * buses 1..16 are the per-part stereo pairs (kAux, NOT default-active, so a host
         * lights up only the parts it routes). process() writes bus 0 from the main-mix
         * ring and each active part bus from its demux sink. */
        addAudioOutput(STR16("Main Mix"), SpeakerArr::kStereo);
        for (int k = 0; k < kNumParts; k++) {
            char ascii[16];
            snprintf(ascii, sizeof ascii, "Part %d", k + 1);
            String128 nm;
            UString(nm, 128).fromAscii(ascii);
            addAudioOutput(nm, SpeakerArr::kStereo, kAux, 0); /* aux, activatable on demand */
        }
        /* Live refuses Instrument-category plugins without an event input
         * ("no valid event input bus"). Notes are ignored until §9.10 UMP
         * carriage lands — but the bus must exist. */
        addEventInput(STR16("MIDI In"), 16);
        return kResultOk;
    }

    tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index,
                                   TBool state) override {
        /* MULTI-OUT (M1): track which per-part output bus the host routes (buses 1..16);
         * registerActivePartSinks (at setActive, before start) registers a demux sink for
         * each active one, so only routed parts stream. Bus 0 (main mix) is always present. */
        if (type == kAudio && dir == kOutput && index >= 1 && index <= kNumParts)
            partBusActive_[index - 1] = (state != 0);
        return AudioEffect::activateBus(type, dir, index, state);
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override {
        rate_ = (uint32_t)setup.sampleRate;
        maxBlock_ = (uint32_t)setup.maxSamplesPerBlock;
        offline_ = setup.processMode == kOffline;
        /* The ring cushion (and thus reported latency) scales with the DAW's
         * block size — a 1024-sample pull needs a deeper ring than a 64-sample
         * one. We CAPTURE the params here and apply them via rt->configure() at
         * acquire time (setActive), so the single-instance sequence
         * configure()->start() stays byte-identical. setupProcessing runs while
         * inactive (no runtime yet) for a fresh instance; if a runtime is already
         * acquired (a re-setup), configure it now too. */
        if (runtime()) runtime()->configure(rate_, maxBlock_);
        /* §8.3-over-§8.7: tell the runtime BEFORE the session starts whether this is
         * an offline bounce, so an Ethernet binding negotiates host-paced
         * (deterministic) instead of free-running RTP. No-op on USB. */
        if (runtime()) runtime()->setOffline(offline_);
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) override {
        if (state) {
            /* Idempotent against a redundant setActive(true) (some hosts double-
             * activate): tear down any runtime we already hold before re-acquiring,
             * so we never leak one. Drop our audio sinks first (same ordering rule
             * as release). */
            if (rt_) {
                releaseSource();
                rt_.reset();
            }
            /* Construct THIS instance's private runtime. There is no sharing —
             * every instance owns its own. Device SELECTION is the runtime's own
             * job (HarpRuntime::selectDevice): HARP_DEVICE_SERIAL env, else the
             * loaded bundle's usb serial (applied via setStateBundle below), else
             * auto-select + singleton-kill — so two instances on different units
             * bind different devices with no shared registry to coordinate. */
            rt_ = runtime_acquire();
            /* Drive it exactly as the old by-value runtime did — configure, stage
             * the project's bundle (stage-before-start, as setState did), then
             * start. The event source is the runtime's built-in ownerSource_ (its
             * channel is set inside start() from HARP_CHANNEL — byte-identical
             * single-instance path). */
            runtime()->configure(rate_, maxBlock_);
            /* §8.3-over-§8.7: apply the offline flag captured at setupProcessing
             * (which runs before the runtime is acquired) BEFORE start(), so
             * selectDevice negotiates host-paced for an Ethernet offline bounce. */
            runtime()->setOffline(offline_);
            if (!pendingState_.empty())
                runtime()->setStateBundle(pendingState_.data(), pendingState_.size());
            /* MULTI-OUT (M1): per-part sinks are registered per ACTIVATED output bus
             * (registerActivePartSinks, driven by the host's activateBus) BEFORE start()
             * — so a host that routes only the main mix keeps the 2-slot union (golden
             * byte-identical) and a host routing N parts streams 2+2N slots. Registering
             * all 16 unconditionally is wrong: it forces a 34-channel union even for the
             * main-mix-only case, and a 34-ch free-running RTP frame (256·34·4 = 34 KB)
             * exceeds the datagram size. Activatable = only routed parts stream. */
            registerActivePartSinks();
            runtime()->start(rate_);
            source_ = runtime()->ownerSource();
            /* P6: pin the source to THIS instance's part. start() seeds the channel
             * from HARP_CHANNEL (the headless --channel path); part_ was seeded from
             * the SAME env (or recalled by setState), so for the env/golden path this
             * is a no-op (part 0 / the env value), and for a recalled/automated Part
             * it asserts the saved part. */
            applyPart();
        } else {
            /* Release: drop our audio sinks (releaseSource, below) before
             * rt_.reset() tears down the runtime — the reader must stop demuxing
             * into a ring before it is freed. rt_.reset() stops+destroys the
             * runtime outright (joining its threads), like the old by-value
             * member's ~HarpRuntime. */
            /* §14.4 host-context-A TRIGGER (test/diagnostic, OPT-IN by env): the
             * out-of-process host's --diag-bundle flag sets HARP_DIAG_BUNDLE_OUT
             * to a file path. We capture the runtime's diag bundle HERE — after
             * the render is complete (the host calls setActive(false) post-
             * process) and while the session is still up — and write the bytes.
             * getDiagBundle() is READ-ONLY off the control path (no audio-path
             * effect), so this is invisible to the render; an unset env is the
             * no-op golden path. The runtime reaches this from the in-process
             * plugin module (the .vst3), not the host. */
            if (const char *p = getenv("HARP_DIAG_BUNDLE_OUT");
                p && p[0] && runtime()) {
                bool anon = false;
                if (const char *a = getenv("HARP_DIAG_BUNDLE_ANON"); a && a[0] && a[0] != '0')
                    anon = true;
                std::vector<uint8_t> db = runtime()->getDiagBundle(anon);
                if (FILE *f = fopen(p, "wb")) {
                    if (!db.empty()) fwrite(db.data(), 1, db.size(), f);
                    fclose(f);
                }
            }
            /* §14.3 host LoopbackMeasurer TRIGGER (OPT-IN by env, mirrors --diag-
             * bundle). The out-of-process host's --loopback flag set HARP_LOOPBACK_IN
             * / HARP_LOOPBACK_OUT; the runtime armed itself at start() (so audio.start
             * declared the in-slot in key 3). Run the measurement HERE — after the
             * render, while the live host-paced session is still up — and print the
             * measured RTT + the §6.4 expected + the delta. The runtime gates the
             * impulse on the device's loopback_on atomic, so this NEVER affects the
             * render (the offline goldens stay byte-identical). Only the OWNER (which
             * drives the live session) measures; an unset env is the no-op golden path. */
            if (const char *li = getenv("HARP_LOOPBACK_IN");
                li && li[0] && runtime() && runtime()->loopbackArmed()) {
                HarpRuntime::LoopbackResult lr = runtime()->measureLoopback();
                fprintf(stdout,
                        "loopback: in=%d out=%d rate=%u armed=%d echo=%d ok=%d "
                        "rtt-samples=%.1f expected-samples=%.1f delta-ms=%.3f (%s)\n",
                        lr.in_slot, lr.out_slot, lr.rate, lr.armed ? 1 : 0,
                        lr.echo_found ? 1 : 0, lr.ok ? 1 : 0, lr.rtt_samples,
                        lr.expected_samples, lr.delta_ms, lr.detail.c_str());
                fflush(stdout);
            }
            releaseSource();
            rt_.reset();
        }
        return AudioEffect::setActive(state);
    }

    uint32 PLUGIN_API getLatencySamples() override {
        /* The reported latency is a pure function of the DAW block size, so it
         * is byte-identical whether we ask the live runtime or compute it
         * statically (the runtime is constructed at activate-time, so a host
         * querying latency in the inactive window has nothing to ask). A live
         * instance reports its runtime's value; otherwise compute it. */
        if (runtime()) return runtime()->latencySamples();
        return HarpRuntime::latencyFor(maxBlock_);
    }

    tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override {
        return symbolicSampleSize == kSample32 ? kResultTrue : kResultFalse;
    }

    /* Zero the output bus and flag it silent. Routed here by a pre-acquire call
     * (no runtime yet / queried before activate). No runtime touch, no event
     * queueing — just clean silence so the host bus is well-defined. */
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
        /* One private runtime per instance (the sharing registry is retired), so
         * this instance is always its own owner: it drives transport, pulls the
         * main mix + every per-part demux sink, and drains the echo ring. */

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
        if (data.processContext) {
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
                    /* MIDI channel C -> device part C (§9.4 multitimbral): the note
                     * routes to its own channel's part directly. (Byte-identical to
                     * the retired MPE-zone-inactive pass-through.) */
                    uint8_t part = (uint8_t)(chan & 0xf);
                    rt.queueNote(source_, ump_note_on(note, vel, part),
                                 base + (uint64_t)ev.sampleOffset);
                    /* remember the §9.5 voice key the device will mint ((part<<8)|note)
                     * so a later Note Expression can target this exact voice. */
                    noteVoices_.noteOn(ev.noteOn.noteId, ((uint32_t)part << 8) | note);
                } else if (ev.type == Event::kNoteOffEvent) {
                    uint32_t note = (uint32_t)(ev.noteOff.pitch & 0x7f);
                    uint32_t chan = (uint32_t)ev.noteOff.channel & 0xf;
                    uint8_t part = (uint8_t)(chan & 0xf);
                    rt.queueNote(source_, ump_note_off(note, part),
                                 base + (uint64_t)ev.sampleOffset);
                    noteVoices_.noteOff(((uint32_t)part << 8) | note);
                } else if (ev.type == Event::kNoteExpressionValueEvent) {
                    /* §9.4/§9.5 non-destructive per-voice modulation from VST3
                     * Note Expression (Cubase MPE + the per-note expression UI),
                     * mapped to the addressed voice (voiceFor(noteId); 0 =
                     * part-wide). All neutral at rest -> golden-identical:
                     *   kBrightnessTypeID (timbre, 0..1) -> Filter Cutoff offset
                     *     (0.5 = no change; the device clamps the sum, never the
                     *     stored base, so recall is unaffected).
                     *   kTuningTypeID (pitch, normalized about 0.5; ±120 semis
                     *     full range per the SDK) -> per-voice pitch bend, in
                     *     semitones = (value-0.5)*240.
                     * Other expression types are ignored for now. */
                    const auto &nx = ev.noteExpressionValue;
                    uint32_t voice = noteVoices_.voiceFor(nx.noteId);
                    uint64_t ts = base + (uint64_t)ev.sampleOffset;
                    if (nx.typeId == kBrightnessTypeID)
                        rt.queueMod(source_, /*Filter Cutoff*/ 3, (float)(nx.value - 0.5), voice, ts);
                    else if (nx.typeId == kTuningTypeID)
                        rt.queueMod(source_, kHarpModPitchBend, (float)((nx.value - 0.5) * 240.0),
                                    voice, ts);
                }
            }
        }

        /* §15.5 offline editing: replay edits made while the device was ABSENT, on the
         * device's RECONNECT edge — "a mismatch resolved by Push". The apply loop records
         * curVal_ for every param and flags dirtyOffline_ for ones edited during an offline
         * gap; here we replay ONLY those, ONLY on a TRUE reconnect (everConnected_), AFTER
         * sessionUp drained the stale event ring (connected_ flips true only then). The FIRST
         * connect replays NOTHING — sessionUp's bundle + the live flow carry the initial
         * state, so the goldens/recall stay byte-untouched (a blind replay-all-on-connect
         * here clobbered recalled state with defaults — the bug this guards). */
        if (!curValInit_) {
            for (size_t i = 0; i < kNumParams; i++) curVal_[i] = kParams[i].defaultVal;
            curValInit_ = true;
        }
        bool nowConn = rt.connected();
        if (nowConn && !wasConnected_) {
            if (everConnected_)
                for (size_t i = 0; i < kNumParams; i++)
                    if (dirtyOffline_[i]) {
                        rt.queueParamSet(source_, (uint32_t)i + 1, curVal_[i], 0);
                        dirtyOffline_[i] = false;
                    }
            everConnected_ = true;
        }
        wasConnected_ = nowConn;

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
                    if (id == kPartParamId) {
                        /* HOST-SIDE routing only: re-part THIS instance live and
                         * do NOT queue a device param-set (id 98 is not a device
                         * param). Stepped 0..15 => part = round(norm * 15). The
                         * source channel is re-read per event, so the change
                         * applies from the next event with no restart. */
                        part_ = (uint8_t)(v * (double)kPartStepCount + 0.5);
                        applyPart();
                        continue;
                    }
                    if (isPerChanParamId(id)) {
                        /* M2 PER-CHANNEL PARAM: a satellite track's MIDI CC on channel N, host-
                         * mapped via IMidiMapping (getMidiControllerAssignment) to this synthetic
                         * id. Decode (channel, device param) and queue a param-set with §9.4 key 5
                         * = channel, so ONE main instance drives every part's params. Immediate
                         * set (CC is stepped at block boundaries; no per-(channel,param) ramp). */                        rt.queueParamSet(source_, perChanParamDevId(id), (float)v, ts,
                                         perChanParamChannel(id));
                        continue;
                    }
                    if (idx == SIZE_MAX) {
                        rt.queueParamSet(source_, id, (float)v, ts);
                        continue;
                    }
                    curVal_[idx] = (float)v;                                   /* §15.5: track current value */
                    if (everConnected_ && !nowConn) dirtyOffline_[idx] = true; /* edited during an offline gap -> replay on reconnect */
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

        /* AUDIO. This instance pulls its session's MAIN MIX off bus 0 (audioRing_,
         * the one consumer — it sums every part injected above) and each routed
         * per-part bus off its demux sink (partSinks_, each its own SPSC ring the
         * reader is the sole producer of). It is the sole consumer of all of them
         * and of the echo ring — there is no second instance to contend. */
        if (data.numSamples <= 0) return kResultOk;

        /* MULTI-OUT (M1) audio write. renderBus pulls `data.numSamples` frames for ONE
         * source — the main-mix ring (mainMix=true, the byte-identical no-sink path) or a
         * per-part demux sink — and deinterleaves into output bus `busIdx` if the host gave
         * it a valid buffer; a mono bus gets L+R summed. It ALWAYS consumes the ring (the
         * reader is the sole producer of every registered sink, so an inactive bus must still
         * be drained or its ring overflows). RT-safe: stack `tmp`, no allocation, no lock. */
        float tmp[4096 * 2];
        auto renderBus = [&](int32 busIdx, AudioSink *sk, bool mainMix) {
            float *L = nullptr, *R = nullptr;
            bool haveBus = busIdx < (int32)data.numOutputs &&
                           data.outputs[busIdx].numChannels >= 1 &&
                           data.outputs[busIdx].channelBuffers32 &&
                           data.outputs[busIdx].channelBuffers32[0];
            if (haveBus) {
                L = data.outputs[busIdx].channelBuffers32[0];
                R = data.outputs[busIdx].numChannels > 1
                        ? data.outputs[busIdx].channelBuffers32[1]
                        : nullptr;
            }
            int32 remaining = data.numSamples, written = 0;
            while (remaining > 0) {
                int32 chunk = remaining > 4096 ? 4096 : remaining;
                if (mainMix) {
                    if (offline_) /* offline bounce: waiting for the wire is correct */
                        rt.pullAudioBlocking(tmp, (size_t)chunk, 1000);
                    else
                        rt.pullAudio(tmp, (size_t)chunk); /* RT: silence on underrun */
                } else {
                    if (offline_)
                        rt.pullAudioBlocking(sk, tmp, (size_t)chunk, 1000);
                    else
                        rt.pullAudio(sk, tmp, (size_t)chunk);
                }
                if (L)
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
            if (haveBus) data.outputs[busIdx].silenceFlags = 0;
        };

        /* Bus 0: the summed main mix (no-sink ring, byte-identical to the shipped
         * single-output path). */
        renderBus(0, nullptr, true);
        /* Buses 1..16: per-part demux. Drain EVERY registered sink each block (the
         * reader produces into all 16 rings whether or not their bus is routed), write
         * the active part buses, discard the rest — no overflow, no bleed between parts. */
        for (int k = 0; k < kNumParts; k++)
            if (partSinks_[k]) renderBus(k + 1, partSinks_[k], false);

        /* device front-panel echoes (§9.4) -> output parameter changes (the echo
         * ring is single-consumer; this instance is its sole owner). */
        {
            uint32_t id;
            float v;
            while (rt.popEcho(part_, id, v)) { /* only THIS instance's part (§9.4) */
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
     * guarded READ of the device state — it reports this instance's project
     * state. With no runtime yet (queried before activate) there is nothing
     * to pull. */
    tresult PLUGIN_API getState(IBStream *state) override {
        if (!runtime()) return kResultFalse;
        std::vector<uint8_t> bundle;
        if (!runtime()->getStateBundle(bundle)) return kResultFalse;
        /* P6 recall-safe Part: write the versioned header (magic + part byte)
         * AHEAD of the unchanged bundle bytes. The device still receives the
         * SAME bundle on reload (setState strips the header before
         * setStateBundle), so the recall round-trip is byte-transparent; the
         * header just carries this instance's per-project Part. */
        uint8_t header[kStateHeaderLen];
        memcpy(header, kStateHeaderMagic, sizeof kStateHeaderMagic);
        /* part byte: low nibble = part. (Bit 7 was the retired raw-MPE toggle; it
         * is now always 0 — an MPE-off instance always wrote 0 there, so the
         * cross-format-recall oracle is byte-identical.) */
        header[sizeof kStateHeaderMagic] = (uint8_t)(part_ & kStatePartMask);
        int32 written = 0;
        if (state->write(header, (int32)sizeof header, &written) != kResultOk)
            return kResultFalse;
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
        /* P6 recall-safe Part: a NEW component state begins with the versioned
         * header (magic + part byte); strip it and adopt the Part. An OLD state
         * is a raw recall bundle (first byte a CBOR map, never the header magic):
         * detected by the magic mismatch, it loads byte-compatibly with Part=0
         * (left at its env/default seed). Everything past the header is the SAME
         * bundle bytes the device round-trips, so getState/setState stays
         * byte-transparent to the recall tests. */
        std::vector<uint8_t> bundle;
        if (raw.size() >= kStateHeaderLen &&
            memcmp(raw.data(), kStateHeaderMagic, sizeof kStateHeaderMagic) == 0) {
            uint8_t pb = raw[sizeof kStateHeaderMagic];
            part_ = (uint8_t)(pb & kStatePartMask);
            /* bit 7 (the retired raw-MPE toggle) is ignored — old MPE-on projects
             * still load, MPE just no longer engages. */
            applyPart(); /* live restore (usually a no-op pre-activate; setActive re-applies) */
            bundle.assign(raw.begin() + kStateHeaderLen, raw.end());
        } else {
            bundle = std::move(raw); /* MIGRATION: header-less old state -> Part=0 */
        }
        if (bundle.empty()) return kResultFalse;
        /* ALWAYS stash the raw bundle: it is this instance's project state. (On
         * project-open setState lands before setupProcessing/setActive, so the
         * runtime usually does not exist yet — staging here reproduces today's
         * stage-before-start; it is applied just before start().) */
        pendingState_ = bundle;

        if (runtime())
            /* Live instance: push now, exactly as before. */
            return runtime()->setStateBundle(bundle.data(), bundle.size()) ? kResultOk
                                                                      : kResultFalse;
        /* No runtime yet: staged above; it is applied just before start(). */
        return kResultOk;
    }

private:
    /* The runtime this instance drives — its OWN, private, owned by unique_ptr
     * (the process-global sharing registry is retired: one multi-out main
     * instance per device claim, so there is nothing to share). Behaves
     * BYTE-IDENTICALLY to the old by-value member / the old empty-serial owner:
     * this instance configures it, pulls the main mix + every per-part demux
     * sink, anchors transport, get/sets state, and queues events on the
     * runtime's built-in owner source. Two instances on different units bind
     * different devices because HarpRuntime::selectDevice() does, not a table.
     * nullptr until acquired at setActive (process() emits silence then). */
    std::unique_ptr<HarpRuntime> rt_;
    /* This instance's event SOURCE: always the runtime's built-in owner source
     * (the one and only source — the per-instance attached-source merge is
     * retired with the sharing registry). All queue* route through it so this
     * instance is the SOLE producer of its SPSC ring. nullptr until acquired
     * (process() emits silence then). */
    EventSource *source_ = nullptr;
    /* RETIRED (was the P5b attached per-part audio opt-in): the attached-instance
     * sink is gone with the owner/attached model — a satellite is MIDI-only now,
     * and the main instance pulls every part's audio through partSinks_ below.
     * Kept as a permanently-null vestige so process()'s pull path is unchanged. */
    AudioSink *sink_ = nullptr;
    /* MULTI-OUT (M1): the OWNER/main instance is a Kontakt/Overbridge-style multi-out
     * synth — it exposes 17 stereo buses (bus 0 = main mix, buses 1..16 = the per-part
     * pairs) and owns the whole device. partSinks_[k] is the demux sink for part k's
     * stereo pair (device slots {2+2k, 3+2k}); registered BEFORE start() so all 16 enter
     * the fixed audio.start union (no mid-session re-neg, no late-attach silence race).
     * Every block we DRAIN all 16 (the reader is the sole producer) and write the active
     * output buses; an inactive bus is drained-and-discarded so its ring can't overflow. */
    static constexpr int kNumParts = 16;
    AudioSink *partSinks_[kNumParts] = {nullptr};
    /* Which per-part output bus the host has routed (activateBus). A bus is registered
     * as a demux sink only when active, so a main-mix-only host keeps the 2-slot union
     * (golden byte-identical) — the activatable Kontakt/Overbridge model. */
    bool partBusActive_[kNumParts] = {false};
    /* Register a demux sink for each ACTIVE per-part bus that doesn't have one yet.
     * Called before start() so the slots enter the fixed audio.start union. */
    void registerActivePartSinks() {
        if (!runtime()) return;
        for (int k = 0; k < kNumParts; k++)
            if (partBusActive_[k] && !partSinks_[k]) {
                std::vector<uint32_t> slots = {2u + 2u * (uint32_t)k, 3u + 2u * (uint32_t)k};
                partSinks_[k] = runtime()->registerAudioSink(slots);
            }
    }
    /* Drop our audio sinks before the runtime is torn down (after which the reader
     * never demuxes into a freed ring). The event source_ is the runtime's own
     * owner source — it is freed WITH the runtime, so we just forget it here. MUST
     * run before rt_.reset() (which destroys the runtime). Idempotent. */
    void releaseSource() {
        /* MULTI-OUT (M1): drop the 16 per-part sinks (after which the reader never
         * demuxes into a freed ring), before forgetting the source and resetting rt_. */
        for (int k = 0; k < kNumParts; k++) {
            if (partSinks_[k] && runtime()) runtime()->unregisterAudioSink(partSinks_[k]);
            partSinks_[k] = nullptr;
        }
        if (sink_ && runtime()) runtime()->unregisterAudioSink(sink_);
        sink_ = nullptr;
        source_ = nullptr;
    }
    HarpRuntime *runtime() const { return rt_.get(); }
    uint32_t rate_ = 48000;
    uint32_t maxBlock_ = 1024; /* captured in setupProcessing; the owner feeds
                                  it to rt->configure() at acquire/start time
                                  (the ring cushion scales with it) */
    /* Raw recall bundle staged by setState BEFORE a runtime exists (state is
     * restored on project-open, before setActive/acquire). It is applied via
     * setStateBundle() right before start(), reproducing today's
     * stage-before-start ordering. */
    std::vector<uint8_t> pendingState_;
    /* This instance's multitimbral PART (§9.4 channel, 0..15) — the per-instance
     * routing the "Part" param (id 98) drives. DEFAULT = HARP_CHANNEL env if set,
     * else 0: the headless out-of-process host (harp-vst3-host --channel) still
     * pins the part through the env exactly as before, and a fresh in-DAW instance
     * starts on part 0. setState may override it (recall), and a live param edit
     * re-parts the source mid-session. Drives the instance's event-source channel
     * at activate and on change (see setActive / process). */
    uint8_t part_ = envChannelDefault();
    /* Phase 3: VST3 Note Expression -> §9.5 per-voice modulation. A host noteId
     * names the note; the device addresses a voice by its §9.5 key. The bridge
     * (noteId -> voice key) is shared with the CLAP shell — note_voice_map.h. */
    NoteVoiceMap noteVoices_;
    /* The Part default the env pins: HARP_CHANNEL (the headless --channel path)
     * clamped 0..15, else 0. Read once to seed part_ so the env path is unchanged
     * (start() ALSO reads HARP_CHANNEL into the owner source, so applyPart()
     * below is a no-op vs the env). */
    static uint8_t envChannelDefault() {
        if (const char *e = getenv("HARP_CHANNEL"); e && e[0]) {
            int v = atoi(e);
            if (v >= 0 && v <= 15) return (uint8_t)v;
        }
        return 0;
    }
    /* Drive THIS instance's event-source channel to part_ (its part) — the
     * runtime's built-in owner source. Storing into EventSource::chan (re-read
     * per event on drain) re-parts without a restart. nullptr source
     * (unacquired) = no-op. */
    void applyPart() {
        if (source_) source_->chan.store(part_ & 0xf, std::memory_order_relaxed);
    }
    bool offline_ = false;
    /* per-param ramp-synthesis state: last emitted point + pending folded
     * point awaiting the 256-sample thinning interval */
    uint64_t lastTs_[kNumParams] = {};
    bool hasLast_[kNumParams] = {};
    bool pendHas_[kNumParams] = {};
    uint64_t pendTs_[kNumParams] = {};
    float pendVal_[kNumParams] = {};
    /* §15.5 offline editing: the current value of each device param, recorded by the apply
     * loop whether or not a device is connected. On the (re)connect EDGE the process loop
     * replays these so an edit made while the device was ABSENT reaches it — the host's live
     * state winning, "a mismatch resolved by Push" (§11.4). Seeded to the device defaults so
     * an unedited param re-asserts its true value (idempotent). */
    float curVal_[kNumParams];
    bool curValInit_ = false;
    bool wasConnected_ = false;
    bool everConnected_ = false;         /* gate: replay only on a TRUE reconnect, never the first connect */
    bool dirtyOffline_[kNumParams] = {}; /* params edited during an offline gap — ONLY these get replayed */


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
#ifdef HARP_SHELL_LABELED_PARAMS
            if (p.labels) { /* a NAMED picker: register the enum labels so the DAW shows them */
                auto *sl = new Steinberg::Vst::StringListParameter(UString256(p.name), p.id);
                const char *s = p.labels;
                while (*s) {
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
#endif
            UString256 title(p.name);
            parameters.addParameter(title, nullptr, p.stepCount, p.defaultVal,
                                    ParameterInfo::kCanAutomate, p.id);
        }
        /* "Part" (id 98): the host-side multitimbral-part router. Stepped over
         * 16 parts (stepCount 15), default 0 => normalized 0 => part 0, the
         * single-instance / golden default. Automatable + per-instance so each
         * shell alias in a DAW can own a distinct part and the choice persists
         * with the project (see HarpProcessor::getState/setState). */
        parameters.addParameter(STR16("Part"), nullptr, kPartStepCount, 0,
                                ParameterInfo::kCanAutomate, kPartParamId);
        parameters.addParameter(STR16("Panic"), nullptr, 0, 0,
                                ParameterInfo::kIsHidden, kPanicParamId);
        /* §9.9 OUTPUT METERS: the device's readonly per-part + main-mix peak/RMS
         * meters (ids 0x1000+). Registered kIsReadOnly so a DAW shows the live
         * values its meter UI/generic editor but offers them as neither
         * automation nor a writable target (kIsReadOnly implies NOT kCanAutomate
         * per the SDK). The shell never writes them; their values arrive via the
         * device echo -> outputParameterChanges path (see process()). */
        for (uint32_t slot = 0; slot < kMeterSlots; slot++) {
            for (uint32_t metric = 0; metric < 2; metric++) {
                uint32_t id = kMeterIdBase + slot * 2 + metric;
                char name[64];
                meterParamName(id, name, sizeof name);
                UString256 title(name);
                parameters.addParameter(title, nullptr, 0, 0.0,
                                        ParameterInfo::kIsReadOnly, id);
            }
        }
        return kResultOk;
    }

    /* MIDI controllers routed to params (the host turns member-channel MIDI into
     * these). DAW panic (CC 120/123, any channel) -> the hidden Panic param; the
     * M2 per-channel CCs -> per-channel device params. */
    tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                   CtrlNumber cc,
                                                   ParamID &id) override {
        if (busIndex != 0) return kResultFalse;
        if (cc == kCtrlAllSoundsOff || cc == kCtrlAllNotesOff) {
            id = kPanicParamId;
            return kResultTrue;
        }
        if (channel < 0 || channel > 15) return kResultFalse;
        /* M2 PER-CHANNEL DEVICE PARAMS: GP CC kPerChanCcBase+i on channel N -> part N's device
         * param (i+1). A satellite MIDI track routes its CC to the main on its channel; the
         * processor decodes the synthetic id and queues a param-set with §9.4 key 5 = N. */
        if (cc >= (CtrlNumber)kPerChanCcBase && cc < (CtrlNumber)(kPerChanCcBase + kNumParams)) {
            id = perChanParamId((uint32_t)channel, (uint32_t)(cc - (CtrlNumber)kPerChanCcBase) + 1u);
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
        std::vector<uint8_t> raw;
        uint8_t buf[8192];
        int32 got = 0;
        while (state->read(buf, sizeof buf, &got) == kResultOk && got > 0) {
            raw.insert(raw.end(), buf, buf + got);
            if (got < (int32)sizeof buf) break;
        }
        if (raw.empty()) return kResultFalse;
        /* The controller gets the SAME component blob as the processor's
         * setState — so it must strip the P6 header too (NEW state) before
         * parsing the bundle, and surface the recalled Part on its param so the
         * DAW shows it. An OLD header-less state parses whole, Part stays 0. */
        const uint8_t *bundle = raw.data();
        size_t blen = raw.size();
        if (raw.size() >= kStateHeaderLen &&
            memcmp(raw.data(), kStateHeaderMagic, sizeof kStateHeaderMagic) == 0) {
            uint8_t pb = raw[sizeof kStateHeaderMagic];
            setParamNormalized(kPartParamId, (double)(pb & kStatePartMask) / (double)kPartStepCount);
            bundle += kStateHeaderLen;
            blen -= kStateHeaderLen;
        }
        if (blen == 0) return kResultFalse;
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
        if (HarpRuntime::bundleParams(bundle, blen, &store, params))
            for (auto &kv : params)
                setParamNormalized(kv.first, kv.second);
        return kResultOk;
    }
#ifdef HARP_SHELL_ENGINE_TABLES
    /* PER-ENGINE PARAM RELABEL. The device's param NAMES change with the selected
     * engine (slot IDs stay fixed, so automation/recall survive). The controller tracks
     * the Engine param and re-titles ids 2..kNumParams from kEngineTables[engine]; on a
     * change it asks the host to re-read titles. The engine index uses the device's
     * (int)(v*N) select so the shown labels match the rendered engine. */
    int currentEngine_ = 0;
    static int engineIndexFor(double norm) {
        int e = (int)(norm * kNumEngineTables);
        return e < 0 ? 0 : (e >= kNumEngineTables ? kNumEngineTables - 1 : e);
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
        tresult r = EditController::setParamNormalized(id, value);
        if (id == (ParamID)(HARP_SHELL_ENGINE_PARAM_ID)) {
            int e = engineIndexFor(value);
            if (e != currentEngine_) {
                currentEngine_ = e;
                if (componentHandler)
                    componentHandler->restartComponent(Steinberg::Vst::kParamTitlesChanged);
            }
        }
        return r;
    }
    tresult PLUGIN_API getParameterInfo(int32 paramIndex, ParameterInfo &info) override {
        tresult r = EditController::getParameterInfo(paramIndex, info);
        if (r != kResultOk) return r;
        if (info.id >= 2 && info.id <= (ParamID)kNumParams && currentEngine_ < kNumEngineTables) {
            const char *t = kEngineTables[currentEngine_][info.id - 1];
            if (t) {
                /* Always relabel + keep the slot VISIBLE. Dynamically toggling kIsHidden on an
                 * engine switch makes hosts (Live) leave a "phantom"/removed-param ghost in the
                 * automation lane (the more->fewer-params case). A fixed-size bank that only ever
                 * RELABELS avoids that; an unused slot just shows its "—" name instead of vanishing. */
                UString(info.title, 128).fromAscii(t);
                info.flags &= ~ParameterInfo::kIsHidden;
            }
        }
        return r;
    }
#endif
};

/* ---------------- factory ---------------- */

#define stringPluginName HARP_SHELL_PLUGIN_NAME

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
