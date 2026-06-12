/* au-host — minimal CLI Audio Unit host for automated testing of the HARP
 * AU shell (and any aumu). The AU twin of tools/vst3-host: params, MIDI,
 * transport via HostCallbacks, block rendering, WAV + hash. Hashing is
 * the SAME fnv1a-over-float32 as vst3-host, deliberately: the two shells
 * share a runtime and a device, so the same drive MUST produce the same
 * hash — cross-format determinism as a one-line assertion.
 *
 *   au-host [--type aumu --subtype rfdv --manu HARP]
 *           [--set ID=V]... [--notes N,..] [--chord N,..] [--bpm B]
 *           [--loop A:B] [--block N] [--seconds S] [--out f.wav] [--hash]
 */
#include <AudioToolbox/AudioToolbox.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void die(const std::string &m) {
    fprintf(stderr, "au-host: %s\n", m.c_str());
    exit(1);
}

static void write_wav16(const std::string &path, const std::vector<float> &interleaved,
                        uint32_t channels, uint32_t rate) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) die("cannot write " + path);
    uint32_t nsamp = (uint32_t)interleaved.size();
    uint32_t data_len = nsamp * 2;
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t riff = 36 + data_len;
    memcpy(hdr + 4, &riff, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16;
    uint16_t pcm = 1, bits = 16;
    uint16_t ch = (uint16_t)channels;
    uint32_t brate = rate * channels * 2;
    uint16_t balign = (uint16_t)(channels * 2);
    memcpy(hdr + 16, &fmtlen, 4);
    memcpy(hdr + 20, &pcm, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &brate, 4);
    memcpy(hdr + 32, &balign, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &data_len, 4);
    fwrite(hdr, 1, 44, f);
    for (float v : interleaved) {
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        int16_t s = (int16_t)lrintf(v * 32767.f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static uint64_t fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ---- transport simulation handed to the AU via HostCallbacks ---- */
struct SimTransport {
    bool active = false;
    double bpm = 0;
    double rate = 48000;
    double ppq = 0;        /* musical position at block start */
    uint64_t samples = 0;  /* sample position at block start */
    double loop_a = -1, loop_b = -1;
};

static OSStatus sim_beat_tempo(void *ud, Float64 *outBeat, Float64 *outTempo) {
    SimTransport *t = (SimTransport *)ud;
    if (!t->active) return kAudioUnitErr_CannotDoInCurrentContext;
    if (outBeat) *outBeat = t->ppq;
    if (outTempo) *outTempo = t->bpm;
    return noErr;
}

static OSStatus sim_transport_state(void *ud, Boolean *outPlaying, Boolean *outChanged,
                                    Float64 *outSamplePos, Boolean *outLooping,
                                    Float64 *outLoopStart, Float64 *outLoopEnd) {
    SimTransport *t = (SimTransport *)ud;
    if (outPlaying) *outPlaying = t->active;
    if (outChanged) *outChanged = false;
    if (outSamplePos) *outSamplePos = (Float64)t->samples;
    if (outLooping) *outLooping = t->loop_b > t->loop_a && t->loop_a >= 0;
    if (outLoopStart) *outLoopStart = t->loop_a;
    if (outLoopEnd) *outLoopEnd = t->loop_b;
    return noErr;
}

int main(int argc, char **argv) {
    AudioComponentDescription desc{};
    desc.componentType = 'aumu';
    desc.componentSubType = 'rfdv';
    desc.componentManufacturer = 'HARP';
    std::vector<std::pair<uint32_t, float>> sets;
    std::vector<int> notes, chord;
    double seconds = 2, bpm = 0, loop_a = -1, loop_b = -1, note_period = 0.6;
    uint32_t block = 256, rate = 48000;
    std::string out_path;
    bool do_hash = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char *what) {
            if (i + 1 >= argc) die(std::string("missing value for ") + what);
            return std::string(argv[++i]);
        };
        if (a == "--set") {
            std::string v = need("--set");
            uint32_t id;
            float val;
            if (sscanf(v.c_str(), "%u=%f", &id, &val) != 2) die("--set wants ID=V");
            sets.push_back({id, val});
        } else if (a == "--notes" || a == "--chord") {
            std::string list = need(a.c_str());
            auto &dst = a == "--notes" ? notes : chord;
            size_t pos = 0;
            while (pos < list.size()) {
                dst.push_back(atoi(list.c_str() + pos));
                pos = list.find(',', pos);
                if (pos == std::string::npos) break;
                pos++;
            }
        } else if (a == "--bpm")
            bpm = atof(need("--bpm").c_str());
        else if (a == "--loop") {
            if (sscanf(need("--loop").c_str(), "%lf:%lf", &loop_a, &loop_b) != 2)
                die("--loop wants A:B (PPQ)");
        } else if (a == "--seconds")
            seconds = atof(need("--seconds").c_str());
        else if (a == "--block")
            block = (uint32_t)atoi(need("--block").c_str());
        else if (a == "--note-period")
            note_period = atof(need("--note-period").c_str());
        else if (a == "--out")
            out_path = need("--out");
        else if (a == "--hash")
            do_hash = true;
        else
            die("unknown arg: " + a);
    }

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) die("AU not found (is harp.component installed?)");
    AudioComponentInstance au;
    if (AudioComponentInstanceNew(comp, &au) != noErr) die("instantiate failed");

    UInt32 maxf = block;
    AudioUnitSetProperty(au, kAudioUnitProperty_MaximumFramesPerSlice,
                         kAudioUnitScope_Global, 0, &maxf, sizeof maxf);
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 32;
    if (AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 0, &f, sizeof f) != noErr)
        die("set format failed");
    UInt32 offline = 1; /* deterministic: block on the wire, never pad */
    AudioUnitSetProperty(au, kAudioUnitProperty_OfflineRender,
                         kAudioUnitScope_Global, 0, &offline, sizeof offline);

    SimTransport sim;
    sim.active = bpm > 0;
    sim.bpm = bpm;
    sim.rate = rate;
    sim.loop_a = loop_a;
    sim.loop_b = loop_b;
    HostCallbackInfo cb{};
    cb.hostUserData = &sim;
    cb.beatAndTempoProc = sim_beat_tempo;
    cb.transportStateProc = sim_transport_state;
    AudioUnitSetProperty(au, kAudioUnitProperty_HostCallbacks,
                         kAudioUnitScope_Global, 0, &cb, sizeof cb);

    if (AudioUnitInitialize(au) != noErr) die("initialize failed");

    Float64 latency = 0;
    UInt32 lsz = sizeof latency;
    AudioUnitGetProperty(au, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
                         &latency, &lsz);
    fprintf(stderr, "au-host: latency %.1f ms\n", latency * 1000.0);

    for (auto &s : sets)
        AudioUnitSetParameter(au, s.first, kAudioUnitScope_Global, 0, s.second, 0);

    size_t total = (size_t)(seconds * rate);
    std::vector<float> capture;
    capture.reserve(total * 2);
    std::vector<float> bufL(block), bufR(block);

    AudioBufferList abl;
    abl.mNumberBuffers = 2;
    abl.mBuffers[0] = {1, (UInt32)(block * sizeof(float)), bufL.data()};
    abl.mBuffers[1] = {1, (UInt32)(block * sizeof(float)), bufR.data()};

    size_t done = 0;
    while (done < total) {
        size_t n = std::min((size_t)block, total - done);

        if (sim.active && sim.loop_b > sim.loop_a && sim.loop_a >= 0 &&
            sim.ppq + 1e-9 >= sim.loop_b)
            sim.ppq = sim.loop_a + (sim.ppq - sim.loop_b);

        /* schedules MIRROR tools/vst3-host EXACTLY (offsets and the
         * float->MIDI velocity rounding), so the two shells produce the
         * same wire traffic and therefore the same hash */
        for (size_t ni = 0; ni < notes.size(); ni++) {
            int64_t onAt = (int64_t)((double)ni * note_period * rate);
            int64_t offAt = onAt + (int64_t)(0.75 * note_period * rate);
            if (onAt >= (int64_t)done && onAt < (int64_t)(done + n))
                MusicDeviceMIDIEvent(au, 0x90, (UInt32)notes[ni],
                                     (UInt32)(0.9f * 127.f + 0.5f) /* = vst3 0.9f */,
                                     (UInt32)(onAt - (int64_t)done));
            if (offAt >= (int64_t)done && offAt < (int64_t)(done + n))
                MusicDeviceMIDIEvent(au, 0x80, (UInt32)notes[ni], 0,
                                     (UInt32)(offAt - (int64_t)done));
        }
        for (int cn : chord) {
            size_t onAt = (size_t)(0.1 * rate);
            if (onAt >= done && onAt < done + n)
                MusicDeviceMIDIEvent(au, 0x90, (UInt32)cn,
                                     (UInt32)(0.8f * 127.f + 0.5f) /* = vst3 0.8f */,
                                     (UInt32)(onAt - done));
            if (done + n >= total)
                MusicDeviceMIDIEvent(au, 0x80, (UInt32)cn, 0, (UInt32)(n - 1));
        }

        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp ts{};
        ts.mSampleTime = (Float64)done;
        ts.mFlags = kAudioTimeStampSampleTimeValid;
        abl.mBuffers[0].mDataByteSize = (UInt32)(n * sizeof(float));
        abl.mBuffers[1].mDataByteSize = (UInt32)(n * sizeof(float));
        OSStatus rc = AudioUnitRender(au, &flags, &ts, 0, (UInt32)n, &abl);
        if (rc != noErr) die("render failed: " + std::to_string(rc));
        for (size_t s = 0; s < n; s++) {
            capture.push_back(bufL[s]);
            capture.push_back(bufR[s]);
        }
        if (sim.active) {
            sim.samples += n;
            sim.ppq += (double)n * sim.bpm / (60.0 * rate);
        }
        done += n;
    }

    double acc = 0;
    for (float v : capture) acc += (double)v * v;
    printf("processed %zu samples x 2 ch, rms=%.5f\n", done,
           sqrt(acc / (double)capture.size()));
    if (do_hash)
        printf("output-hash: %016llx\n",
               (unsigned long long)fnv1a(capture.data(), capture.size() * sizeof(float)));
    if (!out_path.empty()) {
        write_wav16(out_path, capture, 2, rate);
        printf("-> %s\n", out_path.c_str());
    }
    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(au);
    return 0;
}
