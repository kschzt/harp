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
    fprintf(stderr, "tsan-host: %d instance(s), %d s, block %u\n", instances, seconds, block);

    std::vector<HarpRuntime *> rts;
    std::vector<std::thread> audio, control;
    for (int k = 0; k < instances; k++) rts.push_back(new HarpRuntime());
    for (auto *rt : rts) {
        rt->configure(48000, block);
        rt->start(48000); /* device-less is fine — supervisor retries, threads still run */
    }
    /* let the supervisors connect/claim before flooding */
    struct timespec s = {1, 0};
    nanosleep(&s, nullptr);
    for (auto *rt : rts) {
        audio.emplace_back(audio_thread, rt, block);
        control.emplace_back(control_thread, rt, seconds);
    }
    for (int t = 0; t < seconds; t++) nanosleep(&s, nullptr);
    g_run.store(false);
    for (auto &th : control) th.join();
    for (auto &th : audio) th.join();
    for (auto *rt : rts) {
        rt->stop();
        delete rt; /* dtor path too */
    }
    fprintf(stderr, "tsan-host: done\n");
    return 0;
}
