/* tsan-host — a ThreadSanitizer harness for the embedded shell runtime
 * (debt #17). The CLI hosts (au-host/vst3-host) load the plugin
 * dynamically, so TSan in the loaded bundle never initializes — TSan must
 * be present in the main image. So this harness STATICALLY links
 * shell/runtime.cpp (+ usb_io, client, core) and drives the runtime's
 * threads directly, the way a DAW's process()/main threads do, under
 * -fsanitize=thread against a real device.
 *
 * What it exercises (the thread interleavings TSan watches):
 *   - the runtime's own threads: supervisor/feeder, reader, event pump,
 *     libusb's async event thread.
 *   - a simulated DAW audio thread: pullAudio() + streamPos() + popEcho()
 *     in a block-cadence loop.
 *   - the DAW main thread: queueParamSet/Ramp/Note/Transport flooding +
 *     periodic getStateBundle/setStateBundle (the control ops that take
 *     ctlMutex_ — where the last real race lived).
 *   - the multi-out MAIN (--instances N): ONE owner instance owning the whole
 *     device and N parts, exactly as the M3 multi-out shell does.
 *
 * MULTI-OUT MODEL (M3): the registry's share-by-serial / owner+attached model is
 * gone. ONE owner runtime is acquired (runtime_acquire — the empty-serial PRIVATE
 * owner path, or HARP_DEVICE_SERIAL to pin the unit). --instances N is the number
 * of ACTIVE PARTS the single owner drives on channels 0..N-1; parts 1..N-1 each
 * get a per-part demux SINK registered on the owner BEFORE start (the audio.start
 * union), exactly as the multi-out shell registers its active part buses.
 *
 * ONE control thread is the SOLE producer of the owner source's SPSC ring (there
 * is no per-instance source merge any more — that is deleted in M3): it floods
 * notes/params across ALL N channels, the channel carried per-event (queueNote
 * bakes it in the UMP word; queueParamSet via the §9.4-key-5 channel arg), so the
 * device demuxes each part. The owner's audio thread pulls + (--out) captures the
 * summed main mix; a designated part's sink thread captures that part's demuxed
 * audio (the non-silent-demux proof). That is the multi-out main's real
 * concurrency under TSan: supervisor/feeder/reader/eventPump + the audio + sink
 * pulls + the one control producer + the part sinks.
 *
 * Clean exit code 0 + no "WARNING: ThreadSanitizer" in stderr = pass.
 */
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "runtime.h"
#include "runtime_registry.h"
#include "render_check.h" /* harp_fnv1a / harp_write_wav16 — the SAME oracle
                           * vst3-host/au-host use, so a tsan-host main-mix hash
                           * is comparable to the golden-/multitimbral-test ones */

static std::atomic<bool> g_run{true};

/* P5 play-proof instrumentation (harness only — no shell/device change): when
 * --out is given the OWNER's audio thread CAPTURES the device main mix it pulls
 * (instead of only draining it), so the harness can emit an FNV-1a content hash
 * of that mix on exit. The main mix SUMS every active part, so a shared-serial
 * --instances N run (siblings injecting notes on ch1..) yields a DIFFERENT hash
 * than a ch0-only run — direct proof the device rendered MORE THAN ONE part.
 * Capture is owner-only (the one main-mix consumer) and bounded so a long soak
 * cannot grow unbounded; off (empty path) it is a no-op and the byte path is
 * exactly the drain-only loop the TSan run has always exercised. */
static std::vector<float> g_capture; /* owner main mix, interleaved L/R */
static size_t g_capture_cap = 0;     /* max floats to retain (0 = capture off) */
/* P5b: ONE designated attached instance's demuxed part audio, so the play-proof
 * can show a sibling alias actually HEARS its part (a non-silent demux), not just
 * that the structure is race-free. Same bound/oracle as the owner capture. */
static std::vector<float> g_sink_capture;
static size_t g_sink_capture_cap = 0;
/* --late-sink: gate the designated sink's CAPTURE so it begins only AFTER the
 * P5b re-negotiation has actually streamed the wider union. The sink's audio
 * thread starts (its sole consumer — SPSC) the moment the sink is registered, but
 * retains samples into g_sink_capture only once main() arms this (it polls the
 * owner's renegCount() and a short settle). That makes the non-silent proof
 * DETERMINISTIC: the capture never accumulates the pre-re-neg silence that used
 * to dilute the RMS run-to-run. Default true (every other config captures from
 * the start, unchanged); --late-sink sets it false until the re-neg lands. */
static std::atomic<bool> g_capture_armed{true};
/* HARP_ISO_LEVELS="l0,l1,.." — the param-isolation e2e: instead of the random
 * param flood, each instance drives ONLY a controlled, audible voice on ITS part
 * (fixed tone+env, level = its entry here). So an attached sink's energy tracks
 * exactly ONE part's level, and we can prove a part's level param routes to that
 * part's audio and ONLY that part (no cross-talk from a sibling). Empty = the
 * normal flood (every other config). Parsed once in main, read-only after. */
static std::vector<float> g_iso_levels;

/* --no-state-stress: skip the owner's periodic getState/setState round-trip (a
 * ctlMutex_ stress kept for the TSan race configs). The per-part-AUDIO tests
 * measure demuxed audio, not state churn, and post-§11.4-reconcile each setState
 * runs a CAS-to-store Push that starves the stream into underrun — so those tests
 * opt out. Set once in main before threads start; read-only after. */
static bool g_state_stress = true;

/* the simulated DAW audio thread: pull blocks at ~block cadence, and do
 * exactly what the plugin's process() does around the pull (timestamp
 * base, drain echoes). RT mode pads on underrun, never blocks. */
static void audio_thread(HarpRuntime *rt, uint32_t block, bool capture) {
    std::vector<float> buf((size_t)block * 2);
    while (g_run.load(std::memory_order_relaxed)) {
        uint64_t base = rt->streamPos() + rt->latencySamples();
        (void)base;
        rt->pullAudio(buf.data(), block);
        /* --out: keep the device main mix for the play-proof hash. Only ONE
         * audio thread captures (the first owner): with several owners (no
         * pinned serial) g_capture would otherwise have multiple writers. */
        if (capture && g_capture_cap && g_capture.size() < g_capture_cap) {
            size_t take = (size_t)block * 2;
            if (g_capture.size() + take > g_capture_cap) take = g_capture_cap - g_capture.size();
            g_capture.insert(g_capture.end(), buf.data(), buf.data() + take);
        }
        uint32_t id;
        float v;
        /* drain the echo ring so it can't fill — tsan-host tests AUDIO routing, not
         * echoes, so the values are discarded. popEcho(part) drains the whole ring
         * regardless of part (non-matching entries are popped + dropped), so part 0 is
         * fine here. (§9.4 popEcho now takes the consuming part — see shell/runtime.h.) */
        while (rt->popEcho(0, id, v)) {
        }
        struct timespec ts = {0, (long)block * 1000000000L / 48000};
        nanosleep(&ts, nullptr);
    }
}

/* P5b per-part AUDIO thread (opt-in, --part-audio): an ATTACHED instance's
 * simulated DAW audio thread pulling its OWN demuxed sink, exactly as an opted-in
 * plugin's process() does. This puts the new SPSC structure under TSan — the
 * owner's reader() demuxes each device frame into every sink (one producer), and
 * each of these threads is the SOLE consumer of its sink. Pads to silence on
 * underrun, never blocks; no capture (only the owner main mix is the play-proof
 * signal). NOT advancing ssiRead_ (the sink pull doesn't), so the owner's stream
 * clock stays single-writer. */
static void part_audio_thread(HarpRuntime *rt, AudioSink *sink, uint32_t block, bool capture) {
    std::vector<float> buf((size_t)block * 2);
    while (g_run.load(std::memory_order_relaxed)) {
        rt->pullAudio(sink, buf.data(), block);
        /* --part-audio play-proof: keep ONE designated attached sink's demuxed
         * audio so the script can show it is non-silent (the alias hears its
         * part). One producer here, sole consumer of g_sink_capture. */
        if (capture && g_capture_armed.load(std::memory_order_acquire) &&
            g_sink_capture_cap && g_sink_capture.size() < g_sink_capture_cap) {
            size_t take = (size_t)block * 2;
            if (g_sink_capture.size() + take > g_sink_capture_cap)
                take = g_sink_capture_cap - g_sink_capture.size();
            g_sink_capture.insert(g_sink_capture.end(), buf.data(), buf.data() + take);
        }
        struct timespec ts = {0, (long)block * 1000000000L / 48000};
        nanosleep(&ts, nullptr);
    }
}

/* the simulated DAW main/control thread: flood the multi-out main's ONE event
 * SOURCE (the owner source — single producer of its SPSC ring, exactly as the
 * shell's process() is) across ALL `nParts` channels/parts. A DAW with N part
 * tracks routed to the one HARP instance delivers their MIDI on channels 0..N-1
 * through the single plugin input; the eventPump consumes them in order and the
 * device demuxes per part (§9.4 key 5 = channel). queueNote bakes the channel
 * into the UMP word; queueParamSet carries it explicitly (M2). One thread = one
 * producer keeps the SPSC contract — there is no per-instance source merge any
 * more (the multi-out main owns all parts on one source). Also runs the periodic
 * getState/setState round-trip (the ctlMutex_ stress) + anchors transport. */
static void control_thread(HarpRuntime *rt, EventSource *src, int nParts,
                           int seconds) {
    (void)seconds; /* flood for the whole RUN lifetime (gate on g_run), not a fixed
                    * seconds*1000 counter — see the loop note below. */
    uint32_t n = 0;
    std::vector<uint8_t> bundle;
    /* Flood until the run ends (g_run cleared after the capture). The old
     * `t < seconds*1000` bound made the flood stop after a fixed wall time, but the
     * --late-sink CAPTURE is armed only AFTER connect + settle + the re-neg-stable
     * poll (up to ~6 s) + armSettle — so on a slow re-neg the flood could EXPIRE
     * before/under the armed window, leaving the captured part undriven. The device
     * clears every part voice on each re-neg's audio.start (evq_reset_for_new_stream),
     * so an undriven part renders pure zeros => the captured late sink reads FULL
     * silence (the ~1-in-5 hw flake). Gating purely on g_run keeps the captured part
     * receiving note-ons across the entire capture, so it is reliably non-silent;
     * the thread is joined at run end, so g_run always bounds it. */
    while (g_run.load(std::memory_order_relaxed)) {
        uint64_t base = rt->streamPos() + rt->latencySamples();
        /* Drive EACH active part on its OWN channel from this ONE source (single
         * producer — the SPSC contract). A part's params/notes route by §9.4 key 5
         * = the channel: queueParamSet carries it per-event (M2 channel arg) and
         * queueNote bakes it into the UMP word. This is exactly what the multi-out
         * main does when a DAW routes N part tracks to the one HARP instance. */
        for (int p = 0; p < nParts; p++) {
            uint8_t chan = (uint8_t)(p & 0xf);
            float iso = chan < g_iso_levels.size() ? g_iso_levels[chan] : -1.f;
            if (iso >= 0.f) {
                /* param-isolation e2e: a CONTROLLED, audible voice on THIS part —
                 * fixed tone + fast env so each struck note sounds, and level = this
                 * part's HARP_ISO_LEVELS entry, carried on `chan` (§9.4 key 5). So the
                 * captured part sink's energy tracks THIS part's level alone, isolating
                 * per-part param routing. */
                rt->queueParamSet(src, 3, 0.7f, base, chan);  /* tone */
                rt->queueParamSet(src, 5, 0.05f, base, chan); /* fast attack */
                rt->queueParamSet(src, 6, 0.1f, base, chan);  /* fast decay */
                rt->queueParamSet(src, 7, iso, base, chan);   /* level (Master Level id 7) — under test */
            } else {
                /* the CONTINUOUS-param flood (ids 1..7: Osc/Filter/Env/Master), on THIS
                 * part's channel. Keep off the arp/control band (8..12): id 8 is the
                 * STEPPED Arp Mode and flooding it would randomly retrigger notes into a
                 * non-deterministic mix that collapses per-part separation. */
                rt->queueParamSet(src, 1 + (n % 7), (float)(n % 100) / 100.f, base + (n % 256), chan);
                rt->queueRamp(src, 3, (float)(n % 50) / 50.f, base, base + 256, chan);
            }
            /* notes carry their channel in the UMP word (§9.4 — the eventPump does not
             * restamp note channels), so each part SOUNDS and the device demuxes it. */
            uint32_t nch = (uint32_t)chan << 16;
            if (n % 7 == 0)
                rt->queueNote(src, 0x20900000u | nch | ((60 + (n % 12)) << 8) | 0x50, base);
            if (n % 11 == 0)
                rt->queueNote(src, 0x20800000u | nch | ((60 + (n % 12)) << 8) | 0x40, base);
        }
        /* transport is global (ONE stream, the multi-out main anchors it). */
        rt->queueTransport(src, 0x29, 120.0, (double)n * 0.01, base);
        if (g_state_stress && n % 200 == 0 && rt->connected()) { /* op under ctlMutex_ */
            rt->getStateBundle(bundle);
            if (!bundle.empty()) rt->setStateBundle(bundle.data(), bundle.size());
        }
        n++;
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, nullptr);
    }
}

int main(int argc, char **argv) {
    int instances = 1, seconds = 12;
    uint32_t block = 256;
    int rmsWindows = 1; /* --rms-windows N: with --out, also emit per-window main/sink
                         * rms (N equal time-slices of ONE run), so a hw script gets N
                         * samples from ONE device claim. The rig wedges on repeated
                         * multi-instance --part-audio RE-claims; one claim sidesteps it. */
    bool partAudio = false; /* --part-audio: each attached instance pulls its OWN
                             * demuxed part sink (P5b), not silence — exercises the
                             * per-part sink registry + reader() demux under TSan */
    bool lateSink = false; /* --late-sink: register the attached sinks AFTER the
                            * owner has started + run a few seconds, exercising the
                            * P5b RE-NEGOTIATION path — the late sink is in the
                            * registry but NOT the live audio.start union until the
                            * feeder re-streams it. Implies --part-audio. */
    std::string outPath; /* --out: write the owner main-mix WAV + emit its hash */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--instances") && i + 1 < argc) instances = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "--part-audio")) partAudio = true;
        else if (!strcmp(argv[i], "--no-state-stress")) g_state_stress = false;
        else if (!strcmp(argv[i], "--rms-windows") && i + 1 < argc) rmsWindows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--late-sink")) { lateSink = true; partAudio = true; }
    }
    /* Bound the play-proof capture to the run length (+1 s slack), so --out on a
     * long soak never balloons memory; only the owner audio thread fills it. */
    if (!outPath.empty()) {
        g_capture_cap = (size_t)(seconds + 1) * 48000 * 2;
        g_capture.reserve(g_capture_cap);
        if (partAudio) { /* also keep one attached sink's part audio (P5b proof) */
            g_sink_capture_cap = g_capture_cap;
            g_sink_capture.reserve(g_sink_capture_cap);
        }
    }
    /* HARP_ISO_LEVELS="l0,l1,..": per-part level for the param-isolation e2e
     * (parsed once here, read-only by every control thread). Empty -> normal flood. */
    if (const char *iso = getenv("HARP_ISO_LEVELS"); iso && iso[0]) {
        const char *p = iso;
        while (*p) {
            g_iso_levels.push_back((float)atof(p));
            const char *c = strchr(p, ',');
            if (!c) break;
            p = c + 1;
        }
    }
    /* HARP_DEVICE_SERIAL pins a unit — the runtime's selectDevice() reads it
     * directly; unset -> auto-select. The registry's share-by-serial role is gone
     * (M3): every runtime is private now (see runtime_acquire). */
    const char *envSerial = getenv("HARP_DEVICE_SERIAL");
    fprintf(stderr, "tsan-host: %d part(s), %d s, block %u, serial=\"%s\"\n", instances,
            seconds, block, envSerial ? envSerial : "");

    /* MULTI-OUT: ONE owner instance owns the whole device and every part — the
     * registry's share-by-serial / owner+attached model is gone (M3). `instances`
     * is now the number of ACTIVE PARTS the single owner drives (channels 0..N-1);
     * parts 1..N-1 each get a demux sink registered on the owner, exactly as the
     * multi-out shell registers its active part buses. The flag name is kept so the
     * scripts pass --instances unchanged. */
    int nParts = instances < 1 ? 1 : instances;
    std::unique_ptr<HarpRuntime> owner = runtime_acquire(); /* a fresh PRIVATE owner runtime */
    HarpRuntime *ort = owner.get();
    std::vector<AudioSink *> sinks(nParts, nullptr);   /* sinks[p] = part p's demux sink (p>=1) */
    int capturePart = nParts > 1 ? 1 : 0;              /* the part whose sink the play-proof captures */
    std::vector<std::thread> audio, control;

    /* Register parts 1..N-1's demux sinks BEFORE start so their slots enter the
     * audio.start UNION (computeUnionSlotsLocked) — the before-start path a DAW
     * activating its part tracks at load takes. --late-sink defers this to exercise
     * the RE-NEGOTIATION. With N==1 there is no part sink: the union stays {0,1} and
     * the owner pulls the byte-identical main mix. */
    if (partAudio && !lateSink)
        for (int p = 1; p < nParts; p++) {
            std::vector<uint32_t> slots = {2u + 2u * (uint32_t)p, 3u + 2u * (uint32_t)p};
            sinks[p] = ort->registerAudioSink(slots);
        }
    ort->configure(48000, block);
    ort->start(48000); /* device-less is fine — supervisor retries, threads still run */
    EventSource *src = ort->ownerSource();

    /* let the supervisor connect/claim before flooding */
    struct timespec s = {1, 0};
    nanosleep(&s, nullptr);

    /* The owner's main-mix audio thread (captures g_capture for --out); the
     * designated part's sink thread (captures g_sink_capture, the non-silent demux
     * proof); and ONE control thread driving all N parts' channels on the owner
     * source — single producer, exactly as the shell's process() is. */
    audio.emplace_back(audio_thread, ort, block, !outPath.empty());
    if (partAudio && !lateSink && capturePart >= 1 && sinks[capturePart])
        audio.emplace_back(part_audio_thread, ort, sinks[capturePart], block, true);
    control.emplace_back(control_thread, ort, src, nParts, seconds);
    if (!lateSink) {
        for (int t = 0; t < seconds; t++) nanosleep(&s, nullptr);
    } else {
        /* P5b RE-NEGOTIATION exercise: let the owner's session stream the INITIAL
         * union (no per-part sink yet -> just {0,1}) for a few seconds, THEN
         * register every attached instance's sink. Each registerAudioSink lands a
         * sink whose slots are NOT in the live union, raising audioRenegPending_;
         * the feeder then audio.stop -> new union -> audio.start + fence reset, and
         * the late sink begins hearing its part. We spawn each late sink's
         * part_audio_thread the moment it is registered (its sole consumer, fed by
         * the owner's reader demux on the NEW wider frames). The FIRST late sink
         * captures, so the play-proof can show it is non-silent AFTER the re-neg —
         * proof the feeder added its slots mid-session. */
        /* Wait for the owner's session to be LIVE before the late add, so the
         * sink registers DURING a connected stream and the feeder runs the
         * re-negotiation while the reader + eventPump are active (the whole point
         * under TSan). Poll the owner runtime's connected() up to ~5 s; if it
         * never connects (device absent / claim race) we register anyway — the
         * flag is set and a later sessionUp's audio.start absorbs the sink from
         * the registry — but on a healthy rig this lands a true mid-session
         * re-negotiation. A short settle lets the initial {0,1} union stream a
         * bit first so the re-neg is visibly a CHANGE. */
        HarpRuntime *ownerRt = ort;
        for (int t = 0; t < 50 && ownerRt && !ownerRt->connected(); t++) {
            struct timespec ms100 = {0, 100000000L};
            nanosleep(&ms100, nullptr);
        }
        /* Brief settle so the initial {0,1} union streams a little (the re-neg is
         * then visibly a CHANGE). HARP_LATE_SETTLE_MS overrides it: under TSan the
         * device link can time the slow instrumented host out within ~1 s, so the
         * tsan run sets it to 0 to register the sink the INSTANT the session is up
         * and catch the brief live window; the hardware (non-TSan) run leaves the
         * default so the re-neg is a clear mid-stream change. */
        long settleMs = 300;
        if (const char *e = getenv("HARP_LATE_SETTLE_MS")) settleMs = atol(e);
        struct timespec settle = {settleMs / 1000, (settleMs % 1000) * 1000000L};
        nanosleep(&settle, nullptr);
        /* DISARM the designated capture until the re-neg has streamed the wider
         * union: the sink threads spawn now (each its sole consumer, SPSC) and
         * pull/drain immediately, but the capturer retains nothing until we arm it
         * below. So the proof can never accumulate the pre-re-neg silence — it is
         * deterministic, not 7/8. */
        g_capture_armed.store(false, std::memory_order_release);
        uint32_t baseReneg = ownerRt ? ownerRt->renegCount() : 0;
        /* register parts 1..N-1's sinks NOW (mid-session) on the OWNER runtime -> each
         * raises audioRenegPending_; the designated capturePart's sink thread captures
         * the post-re-neg audio (the non-silent proof the feeder added its slots live). */
        for (int p = 1; p < nParts; p++) {
            std::vector<uint32_t> slots = {2u + 2u * (uint32_t)p, 3u + 2u * (uint32_t)p};
            sinks[p] = ownerRt->registerAudioSink(slots); /* -> re-negotiation */
            if (sinks[p])
                audio.emplace_back(part_audio_thread, ownerRt, sinks[p], block, p == capturePart);
        }
        /* Wait for the re-negotiation(s) to FULLY SETTLE before arming. With
         * several late sinks registering near-simultaneously the feeder may run
         * the re-neg in ONE pass or TWO (coalescing is best-effort); a SECOND
         * re-neg's per-sink ring reset (syncSinkEpoch) would momentarily blank a
         * capture armed after only the FIRST — the ~1% intermittent silence. So
         * poll renegCount() until it has occurred AND been STABLE (no further
         * re-neg) for ~500 ms, then arm: the capture is steady-state, after the
         * LAST re-neg, deterministically non-silent. Poll up to ~6 s; if the
         * device drops (no re-neg) we arm anyway so a real silence is a REPORTED
         * fail, not a hang. */
        uint32_t lastReneg = baseReneg;
        int stable = 0;
        for (int t = 0; t < 60 && ownerRt && g_run.load(); t++) {
            struct timespec ms100 = {0, 100000000L};
            nanosleep(&ms100, nullptr);
            uint32_t rc = ownerRt->renegCount();
            if (rc != lastReneg) { lastReneg = rc; stable = 0; }       /* a (further) re-neg */
            else if (rc != baseReneg && ++stable >= 5) break;          /* re-neg'd + stable 500 ms */
        }
        struct timespec armSettle = {0, 200000000L}; /* 200 ms: demux fills the ring */
        nanosleep(&armSettle, nullptr);
        g_capture_armed.store(true, std::memory_order_release);
        /* run the remainder with the wider union streaming + the late sinks pulling */
        for (int t = 0; t < seconds; t++) nanosleep(&s, nullptr);
    }
    g_run.store(false);
    for (auto &th : control) th.join();
    for (auto &th : audio) th.join();
    /* Teardown order matches plugin.cpp: unregister the part sinks BEFORE releasing
     * the runtime (release destroys it; unregistering after would touch freed memory).
     * unregisterAudioSink on nullptr is a no-op; the owner source is the runtime's
     * built-in one, so there is nothing to unregister for it. */
    for (int p = 1; p < nParts; p++)
        if (sinks[p]) ort->unregisterAudioSink(sinks[p]);
    owner.reset(); /* unique_ptr dtor (~HarpRuntime) stops + joins the threads */
    /* --out: emit the owner main-mix content hash (and a listening-aid WAV) on
     * the SAME oracle vst3-host uses. The play-proof script compares this across
     * a ch0-only run and an N-channel alias-group run: a different hash means the
     * device summed sibling parts into its main mix. Hash on stdout, banner on
     * stderr (matching tsan-host's existing stderr-only logging). */
    if (!outPath.empty()) {
        uint64_t h = harp_fnv1a(g_capture.data(), g_capture.size() * sizeof(float));
        printf("output-hash: %016llx\n", (unsigned long long)h);
        auto rms = [](const std::vector<float> &v) {
            if (v.empty()) return 0.0;
            double s = 0;
            for (float x : v) s += (double)x * x;
            return sqrt(s / v.size());
        };
        printf("main-rms: %.6f\n", rms(g_capture));
        if (g_sink_capture_cap) /* --part-audio: the designated attached part's audio */
            printf("sink-rms: %.6f\n", rms(g_sink_capture));
        /* --rms-windows N: N equal time-slices of this ONE run, so the hw script gets
         * N samples from a single claim (no flaky multi-instance re-claim). Each slice
         * is the same iso config, so per-window rms is clean (no boundary blur). */
        if (rmsWindows > 1) {
            auto wrms = [](const std::vector<float> &v, int w, int n) {
                size_t a = (size_t)w * v.size() / n, b = (size_t)(w + 1) * v.size() / n;
                a &= ~size_t(1); b &= ~size_t(1); /* keep L/R stereo pairs intact */
                if (b <= a) return 0.0;
                double s = 0;
                for (size_t k = a; k < b; k++) s += (double)v[k] * v[k];
                return sqrt(s / (double)(b - a));
            };
            for (int w = 0; w < rmsWindows; w++)
                printf("sample %d: main-rms=%.6f sink-rms=%.6f\n", w, wrms(g_capture, w, rmsWindows),
                       g_sink_capture_cap ? wrms(g_sink_capture, w, rmsWindows) : 0.0);
        }
        fflush(stdout);
        harp_write_wav16(outPath, g_capture, 2, 48000);
        fprintf(stderr, "tsan-host: captured %zu owner main-mix samples -> %s\n",
                g_capture.size() / 2, outPath.c_str());
    }
    fprintf(stderr, "tsan-host: done\n");
    return 0;
}
