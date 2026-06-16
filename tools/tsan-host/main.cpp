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
 *   - optionally two runtimes at once (--instances 2): the multi-device
 *     path, two devices, two of every thread.
 *
 * Runtimes now come from the PROCESS-GLOBAL registry (P4), exactly as the
 * plugin obtains them — so --instances N exercises the registry's concurrent
 * acquire/release under TSan. With HARP_DEVICE_SERIAL set, all N instances
 * name the SAME serial: ONE owner runtime / ONE claim, N-1 attached handles
 * (the shared-session path). With no serial, each instance gets its OWN fresh
 * unregistered runtime (the multi-device path) — every instance is an owner.
 *
 * P5 EVENT MERGE: each instance drives its OWN event source on its OWN channel
 * (owner ch0, attached ch1..N-1), so a shared-serial --instances N exercises N
 * SPSC event sources MERGED onto one session under TSan — the new cross-thread
 * structure (the source registry: register on acquire, the owner's eventPump
 * iterating the set, remove on release) gets hammered exactly as a multitimbral
 * group of aliases would hammer it. AUDIO stays owner-only: only the owner
 * pulls the (one-consumer) main-mix ring; attached instances are audio-silent
 * (P5b adds per-part audio), so only the owner spawns an audio thread. EVERY
 * instance spawns a control thread that floods its source — that is the merge.
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
        while (rt->popEcho(id, v)) { /* drain */
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
        if (capture && g_sink_capture_cap && g_sink_capture.size() < g_sink_capture_cap) {
            size_t take = (size_t)block * 2;
            if (g_sink_capture.size() + take > g_sink_capture_cap)
                take = g_sink_capture_cap - g_sink_capture.size();
            g_sink_capture.insert(g_sink_capture.end(), buf.data(), buf.data() + take);
        }
        struct timespec ts = {0, (long)block * 1000000000L / 48000};
        nanosleep(&ts, nullptr);
    }
}

/* the simulated DAW main/control thread for ONE instance: flood THIS instance's
 * event SOURCE (the SPSC producer side it owns) and — owner only — periodically
 * run a getState/setState round-trip (the ctlMutex_ path). `src` is the
 * instance's source: the owner's built-in source or an attached instance's
 * registered one. `isOwner` gates the owner-only surface (transport anchor,
 * state ops); attached instances inject notes/params on their part, exactly as
 * an attached plugin's process() now does, while the OWNER's eventPump merges
 * every source onto the one session. */
static void control_thread(HarpRuntime *rt, EventSource *src, bool isOwner,
                           int seconds) {
    uint32_t n = 0;
    std::vector<uint8_t> bundle;
    for (int t = 0; t < seconds * 1000 && g_run.load(); t++) {
        uint64_t base = rt->streamPos() + rt->latencySamples();
        /* params, ramps, notes — the whole queue* surface, on OUR source */
        rt->queueParamSet(src, 1 + (n % 8), (float)(n % 100) / 100.f, base + (n % 256));
        rt->queueRamp(src, 3, (float)(n % 50) / 50.f, base, base + 256);
        /* Bake THIS source's channel (part) into the note's UMP word, exactly as
         * a DAW/shell does (the eventPump does NOT restamp note channels — §9.4
         * "notes already carry their channel in the word"). Without this every
         * instance played part 0 and the attached per-part audio sinks (P5b)
         * captured pure silence; on its own channel each attached instance now
         * SOUNDS its part, so the demux carries real signal. The owner is ch0, so
         * its notes are unchanged (golden-irrelevant: tsan-host isn't the oracle). */
        uint32_t nch = (uint32_t)(src->chan.load(std::memory_order_relaxed) & 0xf) << 16;
        if (n % 7 == 0)
            rt->queueNote(src, 0x20900000u | nch | ((60 + (n % 12)) << 8) | 0x50, base);
        if (n % 11 == 0)
            rt->queueNote(src, 0x20800000u | nch | ((60 + (n % 12)) << 8) | 0x40, base);
        /* transport is global (owner-anchored): the owner drives it, exactly as
         * plugin.cpp gates feedTransport to the owner. queueTransport pins it to
         * the owner source whatever we pass, but only the owner SHOULD call it. */
        if (isOwner) rt->queueTransport(src, 0x29, 120.0, (double)n * 0.01, base);
        if (isOwner && n % 200 == 0 && rt->connected()) { /* op under ctlMutex_ */
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
    bool partAudio = false; /* --part-audio: each attached instance pulls its OWN
                             * demuxed part sink (P5b), not silence — exercises the
                             * per-part sink registry + reader() demux under TSan */
    std::string outPath; /* --out: write the owner main-mix WAV + emit its hash */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--instances") && i + 1 < argc) instances = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "--part-audio")) partAudio = true;
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
    /* The registry key, exactly as the plugin computes it: HARP_DEVICE_SERIAL
     * pins a unit. Set it (with --instances >1) to exercise SHARING — N
     * instances on one serial, one owner runtime, one claim. Unset -> each
     * instance auto-selects its own fresh runtime (the multi-device path). */
    const char *envSerial = getenv("HARP_DEVICE_SERIAL");
    std::string wantSerial = (envSerial && envSerial[0]) ? envSerial : std::string();
    fprintf(stderr, "tsan-host: %d instance(s), %d s, block %u, serial=\"%s\"\n", instances,
            seconds, block, wantSerial.c_str());

    std::vector<RuntimeHandle> handles;
    std::vector<EventSource *> sources; /* one per instance, parallel to handles */
    std::vector<AudioSink *> sinks;     /* one per instance (nullptr unless --part-audio attached) */
    std::vector<std::thread> audio, control;
    /* Acquire all handles first so the concurrent-acquire bookkeeping (and, for
     * a shared serial, the owner/attached split) is established before driving.
     * The owner of a shared serial is the first acquire; the rest attach. */
    for (int k = 0; k < instances; k++) handles.push_back(runtime_acquire(wantSerial));
    sinks.assign(handles.size(), nullptr);
    /* P5b: register every attached instance's PER-PART AUDIO SINK on the owner
     * runtime BEFORE the owner starts, so its slots enter the audio.start UNION
     * (computeUnionSlotsLocked, called from start()->audioStart). Registered
     * after start they'd be in the registry but not the live union (the P5b
     * fixed-at-start limitation) — so the harness, like a DAW activating tracks
     * together at load, registers them up front. Owner-only --part-audio is
     * harmless: with one instance there is no attached sink, the union stays
     * {0,1}, and the owner pulls the byte-identical main mix. For a shared serial
     * every attached handle's rt is the owner runtime, so the sink lands there. */
    if (partAudio)
        for (size_t k = 0; k < handles.size(); k++)
            if (!handles[k].owner) {
                std::vector<uint32_t> slots = {2u + 2u * (uint32_t)(k & 0xf),
                                               3u + 2u * (uint32_t)(k & 0xf)};
                sinks[k] = handles[k].rt->registerAudioSink(slots);
            }
    /* Only OWNER runtimes are configured/started — an attached instance shares
     * the owner's already-started session, exactly as plugin.cpp does. */
    for (auto &h : handles) {
        if (!h.owner) continue;
        h.rt->configure(48000, block);
        h.rt->start(48000); /* device-less is fine — supervisor retries, threads still run */
    }
    /* P5: bind each instance to its OWN event source on its OWN channel — the
     * owner uses the runtime's built-in source (ch0, set by start's HARP_CHANNEL
     * read; we leave it at 0 here), an attached instance REGISTERS a source for
     * its part (ch1..N-1). This is the merge: N SPSC sources on one session.
     * (registerSource must run after the owner has started — the source array
     * lives in the owner's runtime; for a shared serial every handle's rt is
     * that same owner runtime, so the attached registers there.) */
    for (size_t k = 0; k < handles.size(); k++) {
        RuntimeHandle &h = handles[k];
        if (h.owner)
            sources.push_back(h.rt->ownerSource());
        else
            sources.push_back(h.rt->registerSource((uint8_t)(k & 0xf)));
    }
    /* let the supervisors connect/claim before flooding */
    struct timespec s = {1, 0};
    nanosleep(&s, nullptr);
    /* Drive EVERY instance's event source (the merge under TSan): each control
     * thread is the SOLE producer of its source's ring, the owner's eventPump
     * the sole consumer of all of them. AUDIO stays owner-only — the main-mix
     * ring has one consumer (P5b adds per-part audio), so only owners spawn an
     * audio thread; attached instances are audio-silent, matching process(). */
    bool capturerAssigned = false;
    bool sinkCapturerAssigned = false;
    for (size_t k = 0; k < handles.size(); k++) {
        RuntimeHandle &h = handles[k];
        if (h.owner) {
            audio.emplace_back(audio_thread, h.rt, block, !capturerAssigned); /* first owner captures */
            capturerAssigned = true;
        } else if (sinks[k]) {
            /* P5b opt-in: this attached instance pulls its OWN demuxed part sink —
             * its sole consumer, fed by the owner's reader demux. The new SPSC
             * structure under TSan. The FIRST attached sink also captures, so the
             * play-proof can show it is non-silent (the alias hears its part). */
            audio.emplace_back(part_audio_thread, h.rt, sinks[k], block, !sinkCapturerAssigned);
            sinkCapturerAssigned = true;
        }
        control.emplace_back(control_thread, h.rt, sources[k], h.owner, seconds);
    }
    for (int t = 0; t < seconds; t++) nanosleep(&s, nullptr);
    g_run.store(false);
    for (auto &th : control) th.join();
    for (auto &th : audio) th.join();
    /* Teardown order matches plugin.cpp: an attached instance removes its event
     * source AND its per-part audio sink BEFORE releasing its handle (a last
     * release destroys the runtime; unregistering after would touch a freed
     * runtime). unregisterSource on the owner source / nullptr and
     * unregisterAudioSink on nullptr are no-ops. */
    for (size_t k = 0; k < handles.size(); k++) {
        if (!handles[k].owner && sinks[k]) handles[k].rt->unregisterAudioSink(sinks[k]);
        if (!handles[k].owner && sources[k]) handles[k].rt->unregisterSource(sources[k]);
        runtime_release(handles[k]);
    }
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
        fflush(stdout);
        harp_write_wav16(outPath, g_capture, 2, 48000);
        fprintf(stderr, "tsan-host: captured %zu owner main-mix samples -> %s\n",
                g_capture.size() / 2, outPath.c_str());
    }
    fprintf(stderr, "tsan-host: done\n");
    return 0;
}
