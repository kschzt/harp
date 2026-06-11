/* Unit + stress tests for the shell's lock-free SPSC rings (shell/ring.h).
 * The stress halves run a real producer/consumer pair and assert no loss,
 * no duplication, no reordering — the properties process() depends on. */
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "ring.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

static void test_float_ring_basic() {
    FloatRing r(16);
    CHECK(r.readAvailable() == 0);
    CHECK(r.writeAvailable() == 16);
    float in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(r.write(in, 8) == 8);
    CHECK(r.readAvailable() == 8);
    float out[8] = {};
    CHECK(r.read(out, 4) == 4);
    CHECK(out[0] == 1 && out[3] == 4);
    CHECK(r.read(out, 8) == 4); /* partial: only 4 left */
    CHECK(out[0] == 5 && out[3] == 8);
    /* overfill refuses the excess */
    float big[24];
    for (int i = 0; i < 24; i++) big[i] = (float)i;
    CHECK(r.write(big, 24) == 16);
    r.clear();
    CHECK(r.readAvailable() == 0);
    /* wraparound integrity */
    for (int round = 0; round < 100; round++) {
        float v[3] = {(float)round, (float)round + 0.5f, (float)round + 0.25f};
        CHECK(r.write(v, 3) == 3);
        float o[3];
        CHECK(r.read(o, 3) == 3);
        CHECK(o[0] == v[0] && o[1] == v[1] && o[2] == v[2]);
    }
}

static void test_float_ring_stress() {
    FloatRing r(1 << 10);
    constexpr size_t N = 1 << 20;
    std::atomic<bool> ok{true};
    std::thread producer([&] {
        size_t sent = 0;
        while (sent < N) {
            float chunk[37];
            size_t n = std::min(sizeof chunk / sizeof(float), N - sent);
            for (size_t i = 0; i < n; i++) chunk[i] = (float)(sent + i);
            size_t w = r.write(chunk, n);
            sent += w;
        }
    });
    size_t got = 0;
    while (got < N) {
        float chunk[53];
        size_t n = r.read(chunk, sizeof chunk / sizeof(float));
        for (size_t i = 0; i < n; i++)
            if (chunk[i] != (float)(got + i)) {
                ok = false;
                break;
            }
        got += n;
        if (!ok) break;
    }
    producer.join();
    CHECK(ok.load());
    CHECK(got == N);
}

static void test_param_ring_stress() {
    ParamRing r;
    constexpr size_t N = 1 << 20;
    std::thread producer([&] {
        size_t sent = 0;
        while (sent < N)
            if (r.push({(uint32_t)sent, (float)(sent & 0xff)})) sent++;
    });
    size_t got = 0;
    bool ok = true;
    while (got < N) {
        ParamChange c;
        if (!r.pop(c)) continue;
        if (c.id != (uint32_t)got || c.value != (float)(got & 0xff)) {
            ok = false;
            break;
        }
        got++;
    }
    producer.join();
    CHECK(ok);
    CHECK(got == N);
}

static void test_timed_ring_stress() {
    TimedRing r;
    constexpr size_t N = 1 << 20;
    std::thread producer([&] {
        size_t sent = 0;
        while (sent < N)
            if (r.push({(uint8_t)(sent % 3), (uint32_t)sent, 0.5f, sent, sent + 7}))
                sent++;
    });
    size_t got = 0;
    bool ok = true;
    while (got < N) {
        TimedEv e;
        if (!r.pop(e)) continue;
        if (e.a != (uint32_t)got || e.kind != (uint8_t)(got % 3) || e.ts != got ||
            e.end != got + 7) {
            ok = false;
            break;
        }
        got++;
    }
    producer.join();
    CHECK(ok);
    CHECK(got == N);
}

int main() {
    test_float_ring_basic();
    test_float_ring_stress();
    test_param_ring_stress();
    test_timed_ring_stress();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
