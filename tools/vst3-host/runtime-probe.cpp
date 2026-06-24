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

static void writeWav(const char *path, const std::vector<float> &mono, uint32_t sr) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t nd = (uint32_t)mono.size() * 2, rate = sr, br = sr * 2, chunk = 36 + nd;
    uint16_t ch = 1, bps = 16, ba = 2, fmt = 1, af = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&chunk, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); fwrite(&af, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
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
