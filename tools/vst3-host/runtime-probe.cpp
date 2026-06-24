/* harp-runtime-probe — drive HarpRuntime headlessly (no VST3 host) to test the shell's
 * eth free-running audio path end to end: connect, audio.start, send a held note, pull
 * audio, report RMS. Generic harp test tool (the same runtime Live uses). Device address
 * via HARP_ETH_DEVICE (e.g. host:port or "mdns"). */
#include "runtime.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <cstring>

static void writeWav(const char *path, const std::vector<float> &mono, uint32_t sr) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t nd = (uint32_t)mono.size() * 2, rate = sr, br = sr * 2, chunk = 36 + nd;
    uint16_t ch = 1, bps = 16, ba = 2, fmt = 1;
    uint32_t fmtsz = 16;  /* fmt-chunk size is a uint32 — writing it from a uint16 (&af,4) corrupted the header */
    fwrite("RIFF", 1, 4, f); fwrite(&chunk, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&fmtsz, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&nd, 4, 1, f);
    for (float s : mono) { int v = (int)(s * 32767.0f); if (v > 32767) v = 32767; if (v < -32768) v = -32768;
        int16_t o = (int16_t)v; fwrite(&o, 2, 1, f); }
    fclose(f);
}

int main(int argc, char **argv) {
    /* echo-monitor mode: connect a HELD session and log inbound device param echoes
     * (Electra/front-panel edits -> evt.param.echo -> the SAME popEcho() the VST owner
     * drains into outputParameterChanges). Usage: harp-runtime-probe echo [secs] */
    if (argc > 1 && strcmp(argv[1], "echo") == 0) {
        int secs = argc > 2 ? atoi(argv[2]) : 8;
        HarpRuntime ert;
        ert.configure(44100, 256);
        ert.start(44100);
        for (int i = 0; i < 100 && !ert.connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(stderr, "runtime-probe(echo): connected=%d — logging param echoes for %ds\n",
                ert.connected(), secs);
        if (!ert.connected()) { ert.stop(); return 1; }
        int count = 0;
        for (int t = 0; t < secs * 20; t++) {
            uint32_t id; float v;
            while (ert.popEcho(0, id, v)) { printf("ECHO id=%u v=%.4f\n", id, v); fflush(stdout); count++; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        fprintf(stderr, "runtime-probe(echo): %d param echoes received\n", count);
        ert.stop();
        return count > 0 ? 0 : 2;
    }
    /* echoswitch mode: connect, then SWITCH the Engine param across several engines on this
     * session and log the inbound param echoes. A multi-engine device (the Jetson GPU synth)
     * LOADS the newly-selected engine's default param bank and echoes every automatable param
     * (via core.changed's companion evt.param.echo loop in device/session.c) so the VST's knobs
     * follow the engine's defaults. This proves that path end to end. Usage:
     *   harp-runtime-probe echoswitch [nengines]  (default 18) */
    if (argc > 1 && strcmp(argv[1], "echoswitch") == 0) {
        int neng = argc > 2 ? atoi(argv[2]) : 18;
        HarpRuntime rt;
        rt.configure(44100, 256);
        rt.start(44100);
        for (int i = 0; i < 100 && !rt.connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(stderr, "runtime-probe(echoswitch): connected=%d (switching across %d engines)\n",
                rt.connected(), neng);
        if (!rt.connected()) { rt.stop(); return 1; }
        /* keep the audio stream alive in a background thread (the device's session loop only
         * polls the param-map dirty flag + fires echoes while a stream is running) */
        std::atomic<bool> run{true};
        std::thread audio([&]{ float buf[256 * 2];
            while (run.load()) { rt.pullAudio(buf, 256);
                std::this_thread::sleep_for(std::chrono::microseconds(256 * 1000000LL / 44100)); } });
        int total = 0;
        for (int e = 0; e < neng; e++) {
            float v = (e + 0.5f) / (float)neng;          /* land squarely on engine e */
            rt.queueParamSet(rt.ownerSource(), 1, v, rt.streamPos() + rt.latencySamples());
            fprintf(stderr, "  -> Engine param=%.4f (engine %d)\n", v, e);
            int got = 0;
            for (int t = 0; t < 16; t++) {               /* drain echoes for ~0.8 s */
                uint32_t id; float ev;
                while (rt.popEcho(0, id, ev)) {
                    printf("ECHO engine=%d id=%u v=%.4f\n", e, id, ev); fflush(stdout); got++; total++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            fprintf(stderr, "     echoes after switch to engine %d: %d\n", e, got);
        }
        run.store(false); audio.join();
        fprintf(stderr, "runtime-probe(echoswitch): %d total param echoes across %d switches\n", total, neng);
        rt.stop();
        return total > 0 ? 0 : 2;
    }
    /* melody mode: play a real-time I-vi-IV-V chord progression through the SAME runtime
     * path Live uses, capture a WAV to listen to (proves polyphony + the synth voice end
     * to end). Notes are queued in real time (the bridge applies events at the current
     * render pos). Usage: harp-runtime-probe melody [sr] */
    if (argc > 1 && strcmp(argv[1], "melody") == 0) {
        uint32_t msr = argc > 2 ? (uint32_t)atoi(argv[2]) : 44100;
        HarpRuntime rt;
        rt.configure(msr, 256);
        rt.start(msr);
        for (int i = 0; i < 100 && !rt.connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(stderr, "runtime-probe(melody): connected=%d sr=%u\n", rt.connected(), msr);
        if (!rt.connected()) { rt.stop(); return 1; }
        const int chords[4][3] = {{60,64,67},{57,60,64},{53,57,60},{55,59,62}}; /* C  Am  F  G */
        const double chordDur = 1.6; const int nChords = 4; const double tail = 1.8;
        auto on  = [&](int nn){ rt.queueNote(rt.ownerSource(), 0x20900000u | ((uint32_t)(nn&0x7f)<<8) | 92u, rt.streamPos()+rt.latencySamples()); };
        auto off = [&](int nn){ rt.queueNote(rt.ownerSource(), 0x20800000u | ((uint32_t)(nn&0x7f)<<8) | 0u,  rt.streamPos()+rt.latencySamples()); };
        std::vector<float> capL; float buf[256 * 2]; int cur = -1;
        int blocks = (int)((double)msr / 256.0 * (chordDur * nChords + tail));
        auto m0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; b++) {
            double elapsed = b * 256.0 / msr; int ch = (int)(elapsed / chordDur);
            if (ch != cur && ch < nChords) {
                if (cur >= 0) for (int k = 0; k < 3; k++) off(chords[cur][k]);
                for (int k = 0; k < 3; k++) on(chords[ch][k]);
                cur = ch;
            } else if (elapsed >= chordDur * nChords && cur >= 0) {
                for (int k = 0; k < 3; k++) off(chords[cur][k]); cur = -1;
            }
            rt.pullAudio(buf, 256);
            for (int i = 0; i < 256; i++) capL.push_back(buf[i * 2]);
            std::this_thread::sleep_until(m0 + std::chrono::microseconds((long long)(b + 1) * 256 * 1000000LL / msr));
        }
        writeWav("/tmp/runtime-probe-melody.wav", capL, msr);
        double sq = 0; for (float s : capL) sq += (double)s * s;
        double rms = capL.size() ? std::sqrt(sq / capL.size()) : 0.0;
        fprintf(stderr, "runtime-probe(melody): wrote /tmp/runtime-probe-melody.wav (%zu samples), RMS=%.4f%s\n",
                capL.size(), rms, rms > 0.001 ? "  <-- AUDIBLE" : "  <-- SILENT");
        rt.stop();
        return 0;
    }
    /* engines mode: cycle the Engine picker (param id 1) through all five, playing a
     * C-major chord on each → WAV. Demonstrates the picker + each engine's voice via the
     * SAME queueParamSet/queueNote path the VST uses. Usage: harp-runtime-probe engines [sr] */
    if (argc > 1 && strcmp(argv[1], "engines") == 0) {
        uint32_t esr = argc > 2 ? (uint32_t)atoi(argv[2]) : 44100;
        HarpRuntime rt;
        rt.configure(esr, 256);
        rt.start(esr);
        for (int i = 0; i < 100 && !rt.connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(stderr, "runtime-probe(engines): connected=%d\n", rt.connected());
        if (!rt.connected()) { rt.stop(); return 1; }
        const float ev[5] = {0.1f, 0.3f, 0.5f, 0.7f, 0.9f};                 /* -> engines 0..4 */
        const char *en[5] = {"Additive","FM","Strings","Vox","Bell"};
        const int chord[3] = {60, 64, 67}; const double per = 1.7;
        auto on  = [&](int nn){ rt.queueNote(rt.ownerSource(), 0x20900000u | ((uint32_t)(nn&0x7f)<<8) | 96u, rt.streamPos()+rt.latencySamples()); };
        auto off = [&](int nn){ rt.queueNote(rt.ownerSource(), 0x20800000u | ((uint32_t)(nn&0x7f)<<8) | 0u,  rt.streamPos()+rt.latencySamples()); };
        std::vector<float> capL; float buf[256 * 2]; int cur = -1;
        int blocks = (int)((double)esr / 256.0 * (per * 5 + 0.8));
        auto e0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; b++) {
            double elapsed = b * 256.0 / esr; int e = (int)(elapsed / per);
            if (e != cur && e < 5) {
                if (cur >= 0) for (int k = 0; k < 3; k++) off(chord[k]);
                rt.queueParamSet(rt.ownerSource(), 1, ev[e], rt.streamPos()+rt.latencySamples()); /* Engine */
                for (int k = 0; k < 3; k++) on(chord[k]);
                fprintf(stderr, "  -> engine %d: %s\n", e, en[e]);
                cur = e;
            } else if (elapsed >= per * 5 && cur >= 0) {
                for (int k = 0; k < 3; k++) off(chord[k]); cur = -1;
            }
            rt.pullAudio(buf, 256);
            for (int i = 0; i < 256; i++) capL.push_back(buf[i * 2]);
            std::this_thread::sleep_until(e0 + std::chrono::microseconds((long long)(b + 1) * 256 * 1000000LL / esr));
        }
        writeWav("/tmp/runtime-probe-engines.wav", capL, esr);
        double sq = 0; for (float s : capL) sq += (double)s * s;
        double rms = capL.size() ? std::sqrt(sq / capL.size()) : 0.0;
        fprintf(stderr, "runtime-probe(engines): wrote /tmp/runtime-probe-engines.wav, RMS=%.4f%s\n",
                rms, rms > 0.001 ? "  <-- AUDIBLE" : "  <-- SILENT");
        rt.stop();
        return 0;
    }
    /* sparse mode: a slow, spaced phrase (single notes + one held interval + a final
     * sustained triad with a long tail) on a CHOSEN engine — exposes the per-note
     * character + field response that dense material (Avril) masks. Optional id/val
     * overrides apply AFTER the engine-select (which reloads that engine's defaults),
     * so a "stronger/shaped" variant is one CLI call. Usage:
     *   harp-runtime-probe sparse <engIdx> <numEng> <tag> [paramId val]... */
    if (argc > 1 && strcmp(argv[1], "sparse") == 0) {
        int engIdx = argc > 2 ? atoi(argv[2]) : 5;
        int numEng = argc > 3 ? atoi(argv[3]) : 18;
        const char *tag = argc > 4 ? argv[4] : "sparse";
        uint32_t ssr = 44100;
        HarpRuntime rt; rt.configure(ssr, 256); rt.start(ssr);
        for (int i = 0; i < 100 && !rt.connected(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fprintf(stderr, "runtime-probe(sparse): connected=%d engine=%d/%d tag=%s\n",
                rt.connected(), engIdx, numEng, tag);
        if (!rt.connected()) { rt.stop(); return 1; }
        uint64_t ts0 = rt.streamPos() + rt.latencySamples();
        rt.queueParamSet(rt.ownerSource(), 1, (engIdx + 0.5f) / numEng, ts0); /* select -> loads its defaults */
        for (int a = 5; a + 1 < argc; a += 2)                                  /* optional overrides, post-select */
            rt.queueParamSet(rt.ownerSource(), (uint32_t)atoi(argv[a]), (float)atof(argv[a + 1]), ts0);
        struct Ev { int pitch, vel; double on, off; };
        const Ev phrase[] = {
            {64, 102, 0.5, 1.6}, {71, 86, 2.1, 3.2}, {67, 95, 3.7, 5.4},   /* spaced singles */
            {64, 90, 5.8, 6.9}, {72, 90, 5.9, 7.0},                        /* a held two-note interval */
            {69, 66, 7.4, 8.2}, {65, 74, 8.6, 9.4},                        /* soft (dynamics) */
            {60, 112, 10.0, 15.5}, {64, 106, 10.05, 15.5}, {67, 102, 10.1, 15.5}, /* final triad + long tail */
        };
        const int nev = (int)(sizeof(phrase) / sizeof(phrase[0]));
        const double total = 16.0;
        bool onF[32] = {false}, offF[32] = {false};
        std::vector<float> capL; float buf[256 * 2];
        auto t0 = std::chrono::steady_clock::now();
        int settle = (int)(ssr / 256.0 * 0.5);                             /* let the engine switch + field reset settle */
        for (int b = 0; b < settle; b++) { rt.pullAudio(buf, 256);
            for (int i = 0; i < 256; i++) capL.push_back(buf[i * 2]);
            std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(b + 1) * 256 * 1000000LL / ssr)); }
        auto p0 = std::chrono::steady_clock::now();
        int pblocks = (int)((double)ssr / 256.0 * total);
        for (int b = 0; b < pblocks; b++) {
            double elapsed = b * 256.0 / ssr;
            for (int e = 0; e < nev; e++) {
                if (!onF[e] && elapsed >= phrase[e].on)  { rt.queueNote(rt.ownerSource(), 0x20900000u | ((uint32_t)(phrase[e].pitch & 0x7f) << 8) | (uint32_t)phrase[e].vel, rt.streamPos() + rt.latencySamples()); onF[e] = true; }
                if (!offF[e] && elapsed >= phrase[e].off) { rt.queueNote(rt.ownerSource(), 0x20800000u | ((uint32_t)(phrase[e].pitch & 0x7f) << 8) | 0u, rt.streamPos() + rt.latencySamples()); offF[e] = true; }
            }
            rt.pullAudio(buf, 256);
            for (int i = 0; i < 256; i++) capL.push_back(buf[i * 2]);
            std::this_thread::sleep_until(p0 + std::chrono::microseconds((long long)(b + 1) * 256 * 1000000LL / ssr));
        }
        char path[160]; snprintf(path, sizeof(path), "/tmp/sparse-%s.wav", tag);
        writeWav(path, capL, ssr);
        double sq = 0; for (float s : capL) sq += (double)s * s;
        double rms = capL.size() ? std::sqrt(sq / capL.size()) : 0.0;
        fprintf(stderr, "runtime-probe(sparse): wrote %s (%.1fs), RMS=%.4f\n", path, capL.size() / (double)ssr, rms);
        rt.stop();
        return 0;
    }
    int pitch = argc > 1 ? atoi(argv[1]) : 60; /* C4 */
    uint32_t sr = argc > 2 ? (uint32_t)atoi(argv[2]) : 48000; /* Live is usually 44100 */
    int nonblock = argc > 3 ? atoi(argv[3]) : 0; /* 1 = pullAudio (non-blocking) like Live */
    HarpRuntime rt;
    rt.configure(sr, 256);
    rt.start(sr); /* returns immediately; supervisor connects async */
    fprintf(stderr, "runtime-probe: sr=%u nonblock=%d\n", sr, nonblock);
    for (int i = 0; i < 100 && !rt.connected(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fprintf(stderr, "runtime-probe: connected=%d\n", rt.connected());
    if (!rt.connected()) { rt.stop(); return 1; }

    /* MIDI-1.0-in-UMP note-on, ch0, vel100, ts = streamPos()+latency EXACTLY like plugin.cpp */
    uint64_t nts = rt.streamPos() + rt.latencySamples();
    rt.queueNote(rt.ownerSource(), 0x20900000u | ((uint32_t)(pitch & 0x7f) << 8) | 100u, nts);
    fprintf(stderr, "runtime-probe: note-on pitch=%d queued at ts=%llu (plugin.cpp path)\n", pitch,
            (unsigned long long)nts);

    float buf[256 * 2];
    double sumsq = 0;
    long n = 0, framesPulled = 0;
    auto t0 = std::chrono::steady_clock::now();
    long padded = 0;
    std::vector<float> capL;
    for (int b = 0; b < (int)sr / 256 * 3; b++) { /* ~3 s */
        if (nonblock) rt.pullAudio(buf, 256); /* like Live's process() */
        else padded += (long)rt.pullAudioBlocking(buf, 256, 200);
        framesPulled += 256;
        for (int i = 0; i < 256; i++) { float l = buf[i * 2]; sumsq += (double)l * l; n++; capL.push_back(l); }
        std::this_thread::sleep_until(t0 + std::chrono::microseconds((long long)(b + 1) * 256 * 1000000LL / sr));
    }
    fprintf(stderr, "runtime-probe: %ld padded (underrun) frames of %ld\n", padded, framesPulled);
    writeWav("/tmp/runtime-probe-note.wav", capL, sr);
    fprintf(stderr, "runtime-probe: wrote /tmp/runtime-probe-note.wav (%zu samples)\n", capL.size());
    double rms = n ? std::sqrt(sumsq / n) : 0.0;
    fprintf(stderr, "runtime-probe: pulled %ld frames, RMS=%.5f%s\n", framesPulled, rms,
            rms > 0.001 ? "  <-- AUDIBLE" : "  <-- SILENT");
    rt.stop();
    return 0;
}
