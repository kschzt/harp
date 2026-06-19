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
 *           [--part N] [--save-state FILE] [--load-state FILE]
 *           [--mpe-chord N,..] [--mpe-members N] [--mpe-range SEMIS]
 *           [--mpe-bend SEMIS --mpe-bend-idx K] [--mpe-press 0..1 --mpe-press-idx K]
 *           [--mpe-timbre 0..1 --mpe-timbre-idx K]
 *
 * --mpe-* inject classic MPE as raw 16-channel MIDI (what Logic / Ableton Live
 * send to an AU): an MPE Configuration Message + each note on its own member
 * channel, so the shell's mpe_zone collapses the zone onto the instance part and
 * carries the per-note pitch/pressure/timbre as §9.5 per-voice mods. A neutral
 * --mpe-chord therefore hashes identically to the same notes via plain --chord.
 *
 * --part/--save-state/--load-state mirror tools/vst3-host so a project can be
 * MOVED across formats: the on-disk state file uses the exact same layout
 * ([u32 comp_len][comp][u32 ctrl_len][ctrl]) and the comp bytes are the same
 * 'HP1'+part+bundle both shells persist, so a file written by either host loads
 * into the other byte-for-byte.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "render_check.h"

static void die(const std::string &m) {
    fprintf(stderr, "au-host: %s\n", m.c_str());
    exit(1);
}

/* ---- AU identity (matches shell/au/harp_au.mm + its Info.plist) ---- */
static const OSType kHarpAuType = 'aumu';
static const OSType kHarpAuSubtype = 'rfdv';
static const OSType kHarpAuManu = 'HARP';
static constexpr AudioUnitParameterID kHarpPartParamId = 98; /* host-side router */

/* ---- on-disk state container: [u32 comp_len][comp][u32 ctrl_len][ctrl] ----
 *
 * IDENTICAL layout to tools/vst3-host's save_state_file/load_state_file. The
 * comp portion is the SAME 'HP1'+part+bundle payload both shells persist (the
 * VST3 component getState == the AU's ClassInfo 'harp-bundle' CFData), so a
 * file written by either host loads into the other byte-for-byte. The AU has no
 * separate controller object, so ctrl is always empty (ctrl_len == 0). */
static void save_state_file(const std::string &path, const std::vector<uint8_t> &comp) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) die("cannot write state file");
    uint32_t a = (uint32_t)comp.size(), b = 0; /* no controller in the AU */
    fwrite(&a, 4, 1, f);
    fwrite(comp.data(), 1, a, f);
    fwrite(&b, 4, 1, f);
    fclose(f);
}

static bool load_state_file(const std::string &path, std::vector<uint8_t> &comp) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t a = 0;
    bool ok = false;
    if (fread(&a, 4, 1, f) != 1) goto done;
    comp.resize(a);
    if (a && fread(comp.data(), 1, a, f) != a) goto done;
    ok = true; /* ctrl_len + ctrl follow but the AU ignores them */
done:
    fclose(f);
    return ok;
}

/* Pull the AU's component-state bytes — the 'harp-bundle' CFData payload, the
 * raw 'HP1'+part+bundle that is byte-identical to the VST3 component getState.
 * kAudioUnitProperty_ClassInfo hands back a CFDictionary (caller releases). */
static bool au_get_component_state(AudioComponentInstance au, std::vector<uint8_t> &out) {
    CFPropertyListRef plist = nullptr;
    UInt32 sz = sizeof plist;
    if (AudioUnitGetProperty(au, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global,
                             0, &plist, &sz) != noErr || !plist)
        return false;
    bool ok = false;
    if (CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
        CFDataRef data = (CFDataRef)CFDictionaryGetValue((CFDictionaryRef)plist,
                                                         CFSTR("harp-bundle"));
        if (data && CFGetTypeID(data) == CFDataGetTypeID()) {
            const uint8_t *p = CFDataGetBytePtr(data);
            CFIndex n = CFDataGetLength(data);
            out.assign(p, p + n);
            ok = true;
        }
    }
    CFRelease(plist);
    return ok;
}

/* Wrap raw component-state bytes ('HP1'+part+bundle, from au-host OR vst3-host)
 * into a minimal ClassInfo plist and push it via kAudioUnitProperty_ClassInfo so
 * au_apply_classinfo strips the header, adopts the Part, and stages the bundle.
 * Carries the standard component-type/subtype/manufacturer keys the AU's apply
 * tolerates; the load-bearing key is 'harp-bundle' = the comp bytes verbatim. */
static bool au_set_component_state(AudioComponentInstance au,
                                   const std::vector<uint8_t> &comp) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto put_int = [&](CFStringRef k, SInt64 v) {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt64Type, &v);
        CFDictionarySetValue(d, k, n);
        CFRelease(n);
    };
    put_int(CFSTR(kAUPresetVersionKey), 0);
    put_int(CFSTR(kAUPresetTypeKey), (SInt64)kHarpAuType);
    put_int(CFSTR(kAUPresetSubtypeKey), (SInt64)kHarpAuSubtype);
    put_int(CFSTR(kAUPresetManufacturerKey), (SInt64)kHarpAuManu);
    CFDataRef data = CFDataCreate(nullptr, comp.data(), (CFIndex)comp.size());
    CFDictionarySetValue(d, CFSTR("harp-bundle"), data);
    CFRelease(data);
    OSStatus rc = AudioUnitSetProperty(au, kAudioUnitProperty_ClassInfo,
                                       kAudioUnitScope_Global, 0, &d, sizeof d);
    CFRelease(d);
    return rc == noErr;
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
    std::vector<int> notes, chord, mpe_chord;
    /* classic-MPE injection (raw 16-channel MIDI — what Logic / Ableton Live send
     * to an AU): --mpe-chord plays each note on its own LOWER-zone member channel
     * (ch1, ch2, …) after an MPE Configuration Message, so the shell's mpe_zone
     * collapses the zone onto the instance part. Per-note expression rides the
     * member channel: --mpe-bend SEMIS (14-bit pitch bend scaled by the member PB
     * range), --mpe-press 0..1 (channel pressure), --mpe-timbre 0..1 (CC74). */
    int mpe_members = 0;       /* MCM member count; 0 = derive from --mpe-chord size */
    double mpe_range = 48.0;   /* member PB range (semitones) sent via RPN 0 */
    bool has_mpe_bend = false, has_mpe_press = false, has_mpe_timbre = false;
    double mpe_bend = 0, mpe_press = 0, mpe_timbre = 0;
    int mpe_bend_idx = 0, mpe_press_idx = 0, mpe_timbre_idx = 0;
    bool has_mpe_master_bend = false; /* a MASTER-channel (zone-wide) pitch bend */
    double mpe_master_bend = 0;       /* semitones, scaled by the master PB range */
    double seconds = 2, bpm = 0, loop_a = -1, loop_b = -1, note_period = 0.6;
    uint32_t block = 256, rate = 48000;
    std::string out_path, save_state_path, load_state_path;
    bool do_hash = false;
    int instances = 1; /* >1: prove N AU instances coexist in one process */
    int part = -1; /* 0..15: set the host-side Part param (id 98); -1 = leave default */

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
        } else if (a == "--notes" || a == "--chord" || a == "--mpe-chord") {
            std::string list = need(a.c_str());
            auto &dst = a == "--notes" ? notes : (a == "--chord" ? chord : mpe_chord);
            size_t pos = 0;
            while (pos < list.size()) {
                dst.push_back(atoi(list.c_str() + pos));
                pos = list.find(',', pos);
                if (pos == std::string::npos) break;
                pos++;
            }
        } else if (a == "--mpe-members")
            mpe_members = atoi(need("--mpe-members").c_str());
        else if (a == "--mpe-range")
            mpe_range = atof(need("--mpe-range").c_str());
        else if (a == "--mpe-bend") { mpe_bend = atof(need("--mpe-bend").c_str()); has_mpe_bend = true; }
        else if (a == "--mpe-bend-idx")
            mpe_bend_idx = atoi(need("--mpe-bend-idx").c_str());
        else if (a == "--mpe-press") { mpe_press = atof(need("--mpe-press").c_str()); has_mpe_press = true; }
        else if (a == "--mpe-press-idx")
            mpe_press_idx = atoi(need("--mpe-press-idx").c_str());
        else if (a == "--mpe-timbre") { mpe_timbre = atof(need("--mpe-timbre").c_str()); has_mpe_timbre = true; }
        else if (a == "--mpe-timbre-idx")
            mpe_timbre_idx = atoi(need("--mpe-timbre-idx").c_str());
        else if (a == "--mpe-master-bend") { mpe_master_bend = atof(need("--mpe-master-bend").c_str()); has_mpe_master_bend = true; }
        else if (a == "--bpm")
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
        else if (a == "--save-state")
            save_state_path = need("--save-state");
        else if (a == "--load-state")
            load_state_path = need("--load-state");
        else if (a == "--part") {
            /* host-side multitimbral router (id 98), mirroring vst3-host --part.
             * 0..15: the per-instance part this AU owns, persisted in the state. */
            part = atoi(need("--part").c_str());
            if (part < 0 || part > 15) die("--part wants 0..15");
        } else if (a == "--hash")
            do_hash = true;
        else if (a == "--instances")
            instances = atoi(need("--instances").c_str());
        else
            die("unknown arg: " + a);
    }

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp) die("AU not found (is harp.component installed?)");

    if (instances > 1) {
        /* N plugin instances in ONE process — what a DAW with N tracks
         * does. Pre-singleton-kill they shared a runtime and a device;
         * now each must claim its own. Init each, render a touch so the
         * supervisor settles, report the bound serial per instance. */
        std::vector<AudioComponentInstance> aus(instances, nullptr);
        for (int k = 0; k < instances; k++) {
            if (AudioComponentInstanceNew(comp, &aus[k]) != noErr) die("instantiate failed");
            AudioUnitSetProperty(aus[k], kAudioUnitProperty_MaximumFramesPerSlice,
                                 kAudioUnitScope_Global, 0, &block, sizeof block);
            if (AudioUnitInitialize(aus[k]) != noErr)
                fprintf(stderr, "au-host: instance %d initialize failed\n", k);
        }
        struct timespec ts = {1, 0};
        nanosleep(&ts, nullptr); /* let each supervisor claim + hello */
        for (int k = 0; k < instances; k++) {
            AudioUnitUninitialize(aus[k]);
            AudioComponentInstanceDispose(aus[k]);
        }
        return 0; /* the "claimed ... serial" lines on stderr are the result */
    }

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

    /* --load-state: restore as a DAW project-open would — BEFORE Initialize, so
     * the AU stages the bundle (and adopts the recalled Part) exactly the way it
     * stages a setState pre-activate, and the owner applies it at start(). The
     * file's comp bytes are 'HP1'+part+bundle whether it came from au-host OR
     * vst3-host, so a VST3-saved project loads here unchanged. */
    if (!load_state_path.empty()) {
        std::vector<uint8_t> comp;
        if (!load_state_file(load_state_path, comp)) die("cannot read state file");
        if (!au_set_component_state(au, comp)) die("ClassInfo setState failed");
        fprintf(stderr, "au-host: state restored from %s (%zu bytes)\n",
                load_state_path.c_str(), comp.size());
    }

    /* --part: set the host-side Part router (id 98) BEFORE Initialize so the
     * owner source is pinned to this part at start() (mirrors vst3-host, which
     * sets it before setActive). au_SetParameter's Part path just stores part +
     * applyPart() (a no-op pre-activate), so this is safe before Initialize. A
     * recalled Part from --load-state is overridden here only if --part is given. */
    if (part >= 0)
        AudioUnitSetParameter(au, kHarpPartParamId, kAudioUnitScope_Global, 0,
                              (AudioUnitParameterValue)part, 0);

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

    /* AudioBufferList declares mBuffers as a flexible array [1], so a bare
     * `AudioBufferList abl` reserves room for ONE buffer only. Writing mBuffers[1]
     * for stereo overran the stack struct and clobbered the adjacent `capture`
     * vector's header — a layout-dependent corruption that surfaced as
     * std::length_error in capture.push_back on the CI runner (benign locally).
     * Back it with storage sized for two buffers. */
    uint8_t ablStorage[sizeof(AudioBufferList) + sizeof(AudioBuffer)] = {};
    AudioBufferList &abl = *reinterpret_cast<AudioBufferList *>(ablStorage);
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
        /* classic-MPE chord: configure the zone (MCM + member PB range) then play
         * the chord ACROSS member channels with optional per-note expression. The
         * setup CCs are injected in this SAME block right before the note-ons; the
         * shell's mpe_zone state updates synchronously in MIDIEvent call order, so
         * the zone is live by the time the notes arrive regardless of when the
         * device claim settles. A neutral --mpe-chord collapses onto part_ and
         * therefore hashes IDENTICALLY to the same notes via plain --chord. */
        if (!mpe_chord.empty() || has_mpe_master_bend) {
            size_t onAt = (size_t)(0.1 * rate);
            /* members from --mpe-members, else one per chord note; a master-bend-only
             * render (no chord) still needs an engaged zone, so default to a full
             * lower zone there. */
            int members = mpe_members > 0 ? mpe_members
                          : (mpe_chord.empty() ? 15 : (int)mpe_chord.size());
            if (members < 1) members = 1;
            if (members > 15) members = 15;
            if (onAt >= done && onAt < done + n) {
                UInt32 off = (UInt32)(onAt - done);
                /* MPE Configuration Message on the lower-zone master (ch0): RPN 6,
                 * data = member count — engages the shell's auto-detect. */
                MusicDeviceMIDIEvent(au, 0xB0, 101, 0, off);   /* RPN MSB = 0 */
                MusicDeviceMIDIEvent(au, 0xB0, 100, 6, off);   /* RPN LSB = 6 (MCM) */
                MusicDeviceMIDIEvent(au, 0xB0, 6, (UInt32)members, off);
                /* RPN 0 (member PB range, semitones) on member ch1 — AFTER the MCM,
                 * which re-seeds the ±48 default; the host scales --mpe-bend by it. */
                int range = (int)(mpe_range + 0.5);
                if (range < 1) range = 1;
                if (range > 96) range = 96;
                MusicDeviceMIDIEvent(au, 0xB1, 101, 0, off);   /* RPN MSB = 0 */
                MusicDeviceMIDIEvent(au, 0xB1, 100, 0, off);   /* RPN LSB = 0 (PB range) */
                MusicDeviceMIDIEvent(au, 0xB1, 6, (UInt32)range, off);
                /* master PB range (RPN 0 on the lower master ch0) — only when a
                 * master bend follows, so a zone-wide bend has a usable range. */
                if (has_mpe_master_bend) {
                    MusicDeviceMIDIEvent(au, 0xB0, 101, 0, off);
                    MusicDeviceMIDIEvent(au, 0xB0, 100, 0, off);
                    MusicDeviceMIDIEvent(au, 0xB0, 6, (UInt32)range, off);
                }
                for (size_t ci = 0; ci < mpe_chord.size(); ci++) {
                    UInt32 ch = (UInt32)((ci + 1) & 0x0f); /* lower-zone member ch1.. */
                    MusicDeviceMIDIEvent(au, 0x90 | ch, (UInt32)mpe_chord[ci],
                                         (UInt32)(0.8f * 127.f + 0.5f) /* == --chord */, off);
                    /* per-note expression on THIS note's member channel, queued
                     * right after its note-on (same ts; the voice is already
                     * minted, so the §9.5 mod lands on it). */
                    if (has_mpe_bend && (int)ci == mpe_bend_idx) {
                        /* scale by the SAME rounded range the RPN carried (not the
                         * raw --mpe-range), so the host's encoding and the device's
                         * reconstruction agree exactly even for a fractional range. */
                        double r = (double)range;
                        int v14 = (int)(8192.0 + mpe_bend / r * 8192.0 + 0.5);
                        if (v14 < 0) v14 = 0;
                        if (v14 > 16383) v14 = 16383;
                        MusicDeviceMIDIEvent(au, 0xE0 | ch, (UInt32)(v14 & 0x7f),
                                             (UInt32)((v14 >> 7) & 0x7f), off);
                    }
                    if (has_mpe_press && (int)ci == mpe_press_idx) {
                        int v = (int)(mpe_press * 127.0 + 0.5);
                        if (v < 0) v = 0;
                        if (v > 127) v = 127;
                        MusicDeviceMIDIEvent(au, 0xD0 | ch, (UInt32)v, 0, off);
                    }
                    if (has_mpe_timbre && (int)ci == mpe_timbre_idx) {
                        int v = (int)(mpe_timbre * 127.0 + 0.5);
                        if (v < 0) v = 0;
                        if (v > 127) v = 127;
                        MusicDeviceMIDIEvent(au, 0xB0 | ch, 74, (UInt32)v, off);
                    }
                }
                /* a MASTER-channel (ch0) pitch bend AFTER the note-ons: zone-wide,
                 * applied to EVERY sounding voice on the part (voiceKey 0 =
                 * part-wide). Proves a part-wide mod reaches THIS instance's part,
                 * not always part 0 (the old limitation). */
                if (has_mpe_master_bend) {
                    double r = (double)range;
                    int v14 = (int)(8192.0 + mpe_master_bend / r * 8192.0 + 0.5);
                    if (v14 < 0) v14 = 0;
                    if (v14 > 16383) v14 = 16383;
                    MusicDeviceMIDIEvent(au, 0xE0 | 0, (UInt32)(v14 & 0x7f),
                                         (UInt32)((v14 >> 7) & 0x7f), off);
                }
            }
            if (done + n >= total)
                for (size_t ci = 0; ci < mpe_chord.size(); ci++)
                    MusicDeviceMIDIEvent(au, 0x80 | (UInt32)((ci + 1) & 0x0f),
                                         (UInt32)mpe_chord[ci], 0, (UInt32)(n - 1));
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

    /* --save-state: persist as a DAW project-save would. The component state is
     * the ClassInfo 'harp-bundle' CFData — the raw 'HP1'+part+bundle, byte-
     * identical to what vst3-host's component getState writes — and we frame it
     * in the SAME [u32 comp_len][comp][u32 ctrl_len][ctrl] file (ctrl_len = 0,
     * the AU has no controller). Done after render so the saved bundle reflects
     * the --set params, mirroring vst3-host's post-process-loop getState. */
    if (!save_state_path.empty()) {
        std::vector<uint8_t> comp;
        if (!au_get_component_state(au, comp)) die("ClassInfo getState failed");
        save_state_file(save_state_path, comp);
        printf("state: saved to %s (%zu+0 bytes)\n", save_state_path.c_str(),
               comp.size());
    }

    double acc = 0;
    for (float v : capture) acc += (double)v * v;
    printf("processed %zu samples x 2 ch, rms=%.5f\n", done,
           sqrt(acc / (double)capture.size()));
    if (do_hash)
        printf("output-hash: %016llx\n",
               (unsigned long long)harp_fnv1a(capture.data(), capture.size() * sizeof(float)));
    if (!out_path.empty()) {
        if (!harp_write_wav16(out_path, capture, 2, rate)) die("cannot write " + out_path);
        printf("-> %s\n", out_path.c_str());
    }
    AudioUnitUninitialize(au);
    AudioComponentInstanceDispose(au);
    return 0;
}
