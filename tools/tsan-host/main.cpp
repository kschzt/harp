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
 * Only OWNER instances configure/start and run the drive threads, matching
 * plugin.cpp: an attached instance is dormant in P4 and must not touch the
 * shared runtime's SPSC rings/queues.
 *
 * Clean exit code 0 + no "WARNING: ThreadSanitizer" in stderr = pass.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "runtime.h"
#include "runtime_registry.h"

static std::atomic<bool> g_run{true};

/* the simulated DAW audio thread: pull blocks at ~block cadence, and do
 * exactly what the plugin's process() does around the pull (timestamp
 * base, drain echoes). RT mode pads on underrun, never blocks. */
static void audio_thread(HarpRuntime *rt, uint32_t block) {
    std::vector<float> buf((size_t)block * 2);
    while (g_run.load(std::memory_order_relaxed)) {
        uint64_t base = rt->streamPos() + rt->latencySamples();
        (void)base;
        rt->pullAudio(buf.data(), block);
        uint32_t id;
        float v;
        while (rt->popEcho(id, v)) { /* drain */
        }
        struct timespec ts = {0, (long)block * 1000000000L / 48000};
        nanosleep(&ts, nullptr);
    }
}

/* the simulated DAW main/control thread: flood the event plane and
 * periodically run a getState/setState round-trip (the locked path). */
static void control_thread(HarpRuntime *rt, int seconds) {
    uint32_t n = 0;
    std::vector<uint8_t> bundle;
    for (int t = 0; t < seconds * 1000 && g_run.load(); t++) {
        uint64_t base = rt->streamPos() + rt->latencySamples();
        /* params, ramps, notes — the whole queue* surface */
        rt->queueParamSet(1 + (n % 8), (float)(n % 100) / 100.f, base + (n % 256));
        rt->queueRamp(3, (float)(n % 50) / 50.f, base, base + 256);
        if (n % 7 == 0) rt->queueNote(0x20900000u | ((60 + (n % 12)) << 8) | 0x50, base);
        if (n % 11 == 0) rt->queueNote(0x20800000u | ((60 + (n % 12)) << 8) | 0x40, base);
        rt->queueTransport(0x29, 120.0, (double)n * 0.01, base); /* playing+tempo+pos */
        if (n % 200 == 0 && rt->connected()) { /* control op under ctlMutex_ */
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
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--instances") && i + 1 < argc) instances = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = (uint32_t)atoi(argv[++i]);
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
    std::vector<std::thread> audio, control;
    /* Acquire all handles first so the concurrent-acquire bookkeeping (and, for
     * a shared serial, the owner/attached split) is established before driving.
     * The owner of a shared serial is the first acquire; the rest attach. */
    for (int k = 0; k < instances; k++) handles.push_back(runtime_acquire(wantSerial));
    for (auto &h : handles) {
        if (!h.owner) continue; /* attached: dormant in P4 — do not drive it */
        h.rt->configure(48000, block);
        h.rt->start(48000); /* device-less is fine — supervisor retries, threads still run */
    }
    /* let the supervisors connect/claim before flooding */
    struct timespec s = {1, 0};
    nanosleep(&s, nullptr);
    /* Drive ONLY owner runtimes: an attached instance must not touch the shared
     * runtime's SPSC audio ring or event queue (single-producer/consumer), so
     * it spawns no audio/control thread — matching plugin.cpp's process(). */
    for (auto &h : handles) {
        if (!h.owner) continue;
        audio.emplace_back(audio_thread, h.rt, block);
        control.emplace_back(control_thread, h.rt, seconds);
    }
    for (int t = 0; t < seconds; t++) nanosleep(&s, nullptr);
    g_run.store(false);
    for (auto &th : control) th.join();
    for (auto &th : audio) th.join();
    /* Release every handle: the last release of the shared runtime stops+
     * destroys it (joining its threads); a private runtime is torn down here. */
    for (auto &h : handles) runtime_release(h);
    fprintf(stderr, "tsan-host: done\n");
    return 0;
}
