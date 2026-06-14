/* HARP RefDev — Audio Unit shell (AUv2 instrument, type 'aumu').
 *
 * The second shell over the same embedded HarpRuntime as the VST3: raw
 * AudioComponentPlugInInterface dispatch, no Apple utility classes (they
 * stopped shipping; the property surface below is what auval and hosts
 * actually exercise). Two things this shell has that VST3 cannot:
 *
 *   - kAudioUnitProperty_RenderContextObserver (macOS 11+): the host
 *     hands us its CoreAudio WORKGROUP, and the runtime's reader/pump/
 *     feeder threads join it — real realtime scheduling for the USB
 *     path, the lever VST3 has no API for.
 *   - identical recall: ClassInfo carries the same Recall Bundle bytes
 *     as the VST3 getState, so a project moves between formats with its
 *     hardware state intact.
 *
 * Same oracle: rendered through au-host with OfflineRender set, the
 * golden sequence must hash byte-identically to the VST3 shell — same
 * runtime, same device, same samples.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <AvailabilityMacros.h>
#include <atomic>
#include <cstring>
#include <new>
#include <vector>

#include "runtime.h"
#include "ump.h"

/* identity (also in Info.plist's AudioComponents entry — keep in sync) */
#define HARP_AU_TYPE 'aumu'
#define HARP_AU_SUBTYPE 'rfdv'
#define HARP_AU_MANU 'HARP'
#define HARP_AU_VERSION 0x00010000

/* mirrors the device's parameter set (ids 1..13, normalized 0..1) */
struct AuParam {
    AudioUnitParameterID id;
    const char *name;
};
static const AuParam kAuParams[] = {
    {1, "Osc Pitch"},   {2, "Osc Shape"},    {3, "Filter Cutoff"},
    {4, "Filter Reso"}, {5, "Env Attack"},   {6, "Env Release"},
    {7, "FX Send"},     {8, "Master Level"}, {9, "Arp Mode"},
    {10, "Arp Division"}, {11, "Arp Gate"},  {12, "Arp Octaves"},
    {13, "Glide"},
};
static constexpr UInt32 kNumAuParams = sizeof(kAuParams) / sizeof(kAuParams[0]);

struct HarpAU {
    AudioComponentPlugInInterface iface; /* MUST be first */
    AudioComponentInstance compInstance = nullptr;
    HarpRuntime rt; /* one device per AU instance — no singleton */

    bool initialized = false;
    bool offline = false;
    Float64 sampleRate = 48000;
    UInt32 maxFrames = 4096;
    AudioStreamBasicDescription outFormat{};
    HostCallbackInfo hostCallbacks{};
    AUPreset presentPreset{-1, nullptr};

    struct Listener {
        AudioUnitPropertyID prop;
        AudioUnitPropertyListenerProc proc;
        void *ud;
    };
    std::vector<Listener> listeners; /* host-thread only */

    /* controller-side shadow of the param values (the device owns truth;
     * echoes update this so hosts see front-panel moves) */
    float paramShadow[kNumAuParams];

    /* render scratch: backs ABL buffers when the host passes mData=NULL */
    float *scratch[2] = {nullptr, nullptr};
    float *interleaved = nullptr;

    HarpAU() {
        for (UInt32 i = 0; i < kNumAuParams; i++) paramShadow[i] = 0.5f;
        paramShadow[8] = 0.0f;  /* Arp Mode off */
        paramShadow[9] = 0.6f;  /* Division 1/16 */
        paramShadow[11] = 0.0f; /* Octaves 1 */
        paramShadow[12] = 0.0f; /* Glide off */
        outFormat.mSampleRate = 48000;
        outFormat.mFormatID = kAudioFormatLinearPCM;
        outFormat.mFormatFlags =
            kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
        outFormat.mBytesPerPacket = 4;
        outFormat.mFramesPerPacket = 1;
        outFormat.mBytesPerFrame = 4;
        outFormat.mChannelsPerFrame = 2;
        outFormat.mBitsPerChannel = 32;
    }
    ~HarpAU() {
        free(scratch[0]);
        free(scratch[1]);
        free(interleaved);
        if (presentPreset.presetName) CFRelease(presentPreset.presetName);
    }
};

/* ---------------- helpers ---------------- */

static void au_notify(HarpAU *au, AudioUnitPropertyID prop, AudioUnitScope scope,
                      AudioUnitElement elem) {
    for (auto &l : au->listeners)
        if (l.prop == prop) l.proc(l.ud, au->compInstance, prop, scope, elem);
}

static OSStatus au_alloc_scratch(HarpAU *au) {
    free(au->scratch[0]);
    free(au->scratch[1]);
    free(au->interleaved);
    au->scratch[0] = (float *)calloc(au->maxFrames, sizeof(float));
    au->scratch[1] = (float *)calloc(au->maxFrames, sizeof(float));
    au->interleaved = (float *)calloc((size_t)au->maxFrames * 2, sizeof(float));
    return (au->scratch[0] && au->scratch[1] && au->interleaved) ? noErr
                                                                 : kAudio_MemFullError;
}

/* ---------------- lifecycle ---------------- */

static OSStatus au_Initialize(void *self) {
    HarpAU *au = (HarpAU *)self;
    if (au->initialized) return noErr;
    OSStatus rc = au_alloc_scratch(au);
    if (rc != noErr) return rc;
    auto &rt = au->rt;
    rt.configure((uint32_t)au->outFormat.mSampleRate, au->maxFrames);
    rt.start((uint32_t)au->outFormat.mSampleRate); /* deviceless = silence */
    au->initialized = true;
    return noErr;
}

static OSStatus au_Uninitialize(void *self) {
    HarpAU *au = (HarpAU *)self;
    if (!au->initialized) return noErr;
    au->rt.stop();
    au->initialized = false;
    return noErr;
}

static OSStatus au_Reset(void *self, AudioUnitScope, AudioUnitElement) {
    (void)self;
    return noErr;
}

/* ---------------- properties ---------------- */

static CFMutableDictionaryRef au_make_classinfo(HarpAU *au) {
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    auto put_int = [&](const char *k, SInt64 v) {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt64Type, &v);
        CFStringRef key =
            CFStringCreateWithCString(nullptr, k, kCFStringEncodingUTF8);
        CFDictionarySetValue(d, key, n);
        CFRelease(key);
        CFRelease(n);
    };
    put_int(kAUPresetVersionKey, 0);
    put_int(kAUPresetTypeKey, HARP_AU_TYPE);
    put_int(kAUPresetSubtypeKey, HARP_AU_SUBTYPE);
    put_int(kAUPresetManufacturerKey, HARP_AU_MANU);
    CFStringRef nameKey =
        CFStringCreateWithCString(nullptr, kAUPresetNameKey, kCFStringEncodingUTF8);
    CFDictionarySetValue(d, nameKey,
                         au->presentPreset.presetName ? au->presentPreset.presetName
                                                      : CFSTR("HARP RefDev"));
    CFRelease(nameKey);

    /* the Recall Bundle — identical bytes to the VST3 shell's getState */
    std::vector<uint8_t> bundle;
    if (au->rt.getStateBundle(bundle) && !bundle.empty()) {
        CFDataRef data = CFDataCreate(nullptr, bundle.data(), (CFIndex)bundle.size());
        CFDictionarySetValue(d, CFSTR("harp-bundle"), data);
        CFRelease(data);
    }
    return d;
}

static OSStatus au_apply_classinfo(HarpAU *au, CFPropertyListRef plist) {
    if (!plist || CFGetTypeID(plist) != CFDictionaryGetTypeID())
        return kAudioUnitErr_InvalidPropertyValue;
    CFStringRef nameKey =
        CFStringCreateWithCString(nullptr, kAUPresetNameKey, kCFStringEncodingUTF8);
    CFTypeRef nm = CFDictionaryGetValue((CFDictionaryRef)plist, nameKey);
    CFRelease(nameKey);
    if (nm && CFGetTypeID(nm) == CFStringGetTypeID()) {
        if (au->presentPreset.presetName) CFRelease(au->presentPreset.presetName);
        au->presentPreset.presetName = (CFStringRef)CFRetain(nm);
    }
    CFDataRef data = (CFDataRef)CFDictionaryGetValue((CFDictionaryRef)plist,
                                                     CFSTR("harp-bundle"));
    if (data && CFGetTypeID(data) == CFDataGetTypeID())
        au->rt.setStateBundle(CFDataGetBytePtr(data),
                                               (size_t)CFDataGetLength(data));
    return noErr; /* a preset without our key is fine: defaults stand */
}

static OSStatus au_GetPropertyInfo(void *self, AudioUnitPropertyID prop,
                                   AudioUnitScope scope, AudioUnitElement elem,
                                   UInt32 *outSize, Boolean *outWritable) {
    HarpAU *au = (HarpAU *)self;
    UInt32 size = 0;
    Boolean writable = false;
    switch (prop) {
        case kAudioUnitProperty_ClassInfo:
            size = sizeof(CFPropertyListRef);
            writable = true;
            break;
        case kAudioUnitProperty_SampleRate:
            size = sizeof(Float64);
            writable = !au->initialized;
            break;
        case kAudioUnitProperty_StreamFormat:
            if (scope == kAudioUnitScope_Input) return kAudioUnitErr_InvalidElement;
            size = sizeof(AudioStreamBasicDescription);
            writable = !au->initialized;
            break;
        case kAudioUnitProperty_ElementCount:
            size = sizeof(UInt32);
            break;
        case kAudioUnitProperty_MaximumFramesPerSlice:
            size = sizeof(UInt32);
            writable = !au->initialized;
            break;
        case kAudioUnitProperty_Latency:
        case kAudioUnitProperty_TailTime:
            if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
            size = sizeof(Float64);
            break;
        case kAudioUnitProperty_SupportedNumChannels:
            size = sizeof(AUChannelInfo);
            break;
        case kAudioUnitProperty_ParameterList:
            size = scope == kAudioUnitScope_Global
                       ? kNumAuParams * sizeof(AudioUnitParameterID)
                       : 0;
            break;
        case kAudioUnitProperty_ParameterInfo:
            size = sizeof(AudioUnitParameterInfo);
            break;
        case kAudioUnitProperty_PresentPreset:
            size = sizeof(AUPreset);
            writable = true;
            break;
        case kAudioUnitProperty_HostCallbacks:
            size = sizeof(HostCallbackInfo);
            writable = true;
            break;
        case kAudioUnitProperty_OfflineRender:
            size = sizeof(UInt32);
            writable = true;
            break;
        case kAudioUnitProperty_InPlaceProcessing:
            return kAudioUnitErr_InvalidProperty;
#if defined(MAC_OS_VERSION_11_0)
        case kAudioUnitProperty_RenderContextObserver:
            size = sizeof(AURenderContextObserver);
            break;
#endif
        default:
            (void)elem;
            return kAudioUnitErr_InvalidProperty;
    }
    if (outSize) *outSize = size;
    if (outWritable) *outWritable = writable;
    return noErr;
}

static OSStatus au_GetProperty(void *self, AudioUnitPropertyID prop,
                               AudioUnitScope scope, AudioUnitElement elem,
                               void *outData, UInt32 *ioSize) {
    HarpAU *au = (HarpAU *)self;
    auto &rt = au->rt;
    switch (prop) {
        case kAudioUnitProperty_ClassInfo: {
            if (*ioSize < sizeof(CFPropertyListRef)) return kAudioUnitErr_InvalidParameter;
            *(CFPropertyListRef *)outData = au_make_classinfo(au); /* caller releases */
            *ioSize = sizeof(CFPropertyListRef);
            return noErr;
        }
        case kAudioUnitProperty_SampleRate:
            *(Float64 *)outData = au->outFormat.mSampleRate;
            *ioSize = sizeof(Float64);
            return noErr;
        case kAudioUnitProperty_StreamFormat:
            if (scope == kAudioUnitScope_Input) return kAudioUnitErr_InvalidElement;
            *(AudioStreamBasicDescription *)outData = au->outFormat;
            *ioSize = sizeof(AudioStreamBasicDescription);
            return noErr;
        case kAudioUnitProperty_ElementCount:
            *(UInt32 *)outData = scope == kAudioUnitScope_Input ? 0 : 1;
            *ioSize = sizeof(UInt32);
            return noErr;
        case kAudioUnitProperty_MaximumFramesPerSlice:
            *(UInt32 *)outData = au->maxFrames;
            *ioSize = sizeof(UInt32);
            return noErr;
        case kAudioUnitProperty_Latency:
            if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
            *(Float64 *)outData =
                (Float64)rt.latencySamples() / au->outFormat.mSampleRate;
            *ioSize = sizeof(Float64);
            return noErr;
        case kAudioUnitProperty_TailTime:
            if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
            *(Float64 *)outData = 3.0; /* release knob max ~3 s */
            *ioSize = sizeof(Float64);
            return noErr;
        case kAudioUnitProperty_SupportedNumChannels: {
            AUChannelInfo ci = {0, 2}; /* instrument: no input, stereo out */
            *(AUChannelInfo *)outData = ci;
            *ioSize = sizeof(AUChannelInfo);
            return noErr;
        }
        case kAudioUnitProperty_ParameterList: {
            if (scope != kAudioUnitScope_Global) {
                *ioSize = 0;
                return noErr;
            }
            UInt32 n = *ioSize / sizeof(AudioUnitParameterID);
            if (n > kNumAuParams) n = kNumAuParams;
            for (UInt32 i = 0; i < n; i++)
                ((AudioUnitParameterID *)outData)[i] = kAuParams[i].id;
            *ioSize = n * sizeof(AudioUnitParameterID);
            return noErr;
        }
        case kAudioUnitProperty_ParameterInfo: {
            if (elem < 1 || elem > kNumAuParams) return kAudioUnitErr_InvalidElement;
            auto *info = (AudioUnitParameterInfo *)outData;
            memset(info, 0, sizeof *info);
            const AuParam &p = kAuParams[elem - 1];
            info->flags = kAudioUnitParameterFlag_IsReadable |
                          kAudioUnitParameterFlag_IsWritable |
                          kAudioUnitParameterFlag_HasCFNameString |
                          kAudioUnitParameterFlag_CFNameRelease;
            info->cfNameString =
                CFStringCreateWithCString(nullptr, p.name, kCFStringEncodingUTF8);
            strncpy(info->name, p.name, sizeof info->name - 1);
            info->unit = kAudioUnitParameterUnit_Generic;
            info->minValue = 0;
            info->maxValue = 1;
            info->defaultValue = au->paramShadow[elem - 1];
            *ioSize = sizeof(AudioUnitParameterInfo);
            return noErr;
        }
        case kAudioUnitProperty_PresentPreset: {
            AUPreset *p = (AUPreset *)outData;
            p->presetNumber = au->presentPreset.presetNumber;
            p->presetName = au->presentPreset.presetName
                                ? (CFStringRef)CFRetain(au->presentPreset.presetName)
                                : CFSTR("Default");
            *ioSize = sizeof(AUPreset);
            return noErr;
        }
#if defined(MAC_OS_VERSION_11_0)
        case kAudioUnitProperty_RenderContextObserver: {
            if (*ioSize < sizeof(AURenderContextObserver))
                return kAudioUnitErr_InvalidParameter;
            /* THE WORKGROUP HANDOFF: the host tells us which CoreAudio
             * workgroup its render thread belongs to; the runtime joins
             * its reader/pump/feeder threads so the USB path is scheduled
             * with the audio graph, not against it. */
            AURenderContextObserver obs = ^(const AudioUnitRenderContext *ctx) {
              au->rt.setWorkgroup(ctx ? ctx->workgroup : nullptr);
            };
            *(AURenderContextObserver *)outData = [obs copy];
            *ioSize = sizeof(AURenderContextObserver);
            return noErr;
        }
#endif
        default:
            return kAudioUnitErr_InvalidProperty;
    }
}

static OSStatus au_SetProperty(void *self, AudioUnitPropertyID prop,
                               AudioUnitScope scope, AudioUnitElement elem,
                               const void *inData, UInt32 inSize) {
    HarpAU *au = (HarpAU *)self;
    (void)elem;
    switch (prop) {
        case kAudioUnitProperty_ClassInfo:
            if (inSize < sizeof(CFPropertyListRef))
                return kAudioUnitErr_InvalidParameter;
            return au_apply_classinfo(au, *(CFPropertyListRef *)inData);
        case kAudioUnitProperty_SampleRate:
            if (au->initialized) return kAudioUnitErr_Initialized;
            au->outFormat.mSampleRate = *(const Float64 *)inData;
            au_notify(au, prop, scope, 0);
            return noErr;
        case kAudioUnitProperty_StreamFormat: {
            if (scope == kAudioUnitScope_Input) return kAudioUnitErr_InvalidElement;
            if (au->initialized) return kAudioUnitErr_Initialized;
            const auto *f = (const AudioStreamBasicDescription *)inData;
            if (f->mFormatID != kAudioFormatLinearPCM ||
                f->mChannelsPerFrame != 2 || f->mBitsPerChannel != 32 ||
                !(f->mFormatFlags & kAudioFormatFlagIsFloat) ||
                !(f->mFormatFlags & kAudioFormatFlagIsNonInterleaved))
                return kAudioUnitErr_FormatNotSupported;
            au->outFormat = *f;
            au_notify(au, prop, scope, 0);
            return noErr;
        }
        case kAudioUnitProperty_MaximumFramesPerSlice:
            if (au->initialized) return kAudioUnitErr_Initialized;
            au->maxFrames = *(const UInt32 *)inData;
            au_notify(au, prop, scope, 0);
            return noErr;
        case kAudioUnitProperty_PresentPreset: {
            const AUPreset *p = (const AUPreset *)inData;
            if (au->presentPreset.presetName) CFRelease(au->presentPreset.presetName);
            au->presentPreset.presetNumber = p->presetNumber;
            au->presentPreset.presetName =
                p->presetName ? (CFStringRef)CFRetain(p->presetName) : nullptr;
            return noErr;
        }
        case kAudioUnitProperty_HostCallbacks: {
            UInt32 n = inSize < sizeof(HostCallbackInfo) ? inSize
                                                         : sizeof(HostCallbackInfo);
            memset(&au->hostCallbacks, 0, sizeof au->hostCallbacks);
            memcpy(&au->hostCallbacks, inData, n);
            return noErr;
        }
        case kAudioUnitProperty_OfflineRender:
            au->offline = *(const UInt32 *)inData != 0;
            return noErr;
        default:
            return kAudioUnitErr_InvalidProperty;
    }
}

/* listeners / render notify: hosts and auval add these; we accept and,
 * since every property they could observe changes only at their own
 * request, never need to fire them in v0 */
static OSStatus au_AddPropertyListener(void *self, AudioUnitPropertyID prop,
                                       AudioUnitPropertyListenerProc proc, void *ud) {
    ((HarpAU *)self)->listeners.push_back({prop, proc, ud});
    return noErr;
}
static OSStatus au_RemovePropertyListener(void *self, AudioUnitPropertyID prop,
                                          AudioUnitPropertyListenerProc proc) {
    auto &ls = ((HarpAU *)self)->listeners;
    for (size_t i = ls.size(); i-- > 0;)
        if (ls[i].prop == prop && ls[i].proc == proc) ls.erase(ls.begin() + (long)i);
    return noErr;
}
static OSStatus au_RemovePropertyListenerWithUserData(void *self, AudioUnitPropertyID prop,
                                                      AudioUnitPropertyListenerProc proc,
                                                      void *ud) {
    auto &ls = ((HarpAU *)self)->listeners;
    for (size_t i = ls.size(); i-- > 0;)
        if (ls[i].prop == prop && ls[i].proc == proc && ls[i].ud == ud)
            ls.erase(ls.begin() + (long)i);
    return noErr;
}
static OSStatus au_AddRenderNotify(void *, AURenderCallback, void *) { return noErr; }
static OSStatus au_RemoveRenderNotify(void *, AURenderCallback, void *) {
    return noErr;
}

/* ---------------- parameters ---------------- */

static OSStatus au_GetParameter(void *self, AudioUnitParameterID param,
                                AudioUnitScope scope, AudioUnitElement,
                                AudioUnitParameterValue *out) {
    HarpAU *au = (HarpAU *)self;
    if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    if (param < 1 || param > kNumAuParams) return kAudioUnitErr_InvalidParameter;
    /* drain device echoes into the shadow so hosts see panel moves */
    uint32_t id;
    float v;
    while (au->rt.popEcho(id, v))
        if (id >= 1 && id <= kNumAuParams) au->paramShadow[id - 1] = v;
    *out = au->paramShadow[param - 1];
    return noErr;
}

static OSStatus au_SetParameter(void *self, AudioUnitParameterID param,
                                AudioUnitScope scope, AudioUnitElement,
                                AudioUnitParameterValue value, UInt32 offset) {
    HarpAU *au = (HarpAU *)self;
    if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    if (param < 1 || param > kNumAuParams) return kAudioUnitErr_InvalidParameter;
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    au->paramShadow[param - 1] = value;
    auto &rt = au->rt;
    rt.queueParamSet(param, value,
                     offset ? rt.streamPos() + rt.latencySamples() + offset : 0);
    return noErr;
}

static OSStatus au_ScheduleParameters(void *self, const AudioUnitParameterEvent *ev,
                                      UInt32 n) {
    for (UInt32 i = 0; i < n; i++) {
        if (ev[i].eventType == kParameterEvent_Immediate)
            au_SetParameter(self, ev[i].parameter, ev[i].scope, ev[i].element,
                            ev[i].eventValues.immediate.value,
                            ev[i].eventValues.immediate.bufferOffset);
        else { /* ramp: start/end values over a frame span -> §9.4 ramp */
            HarpAU *au = (HarpAU *)self;
            auto &rt = au->rt;
            uint64_t base = rt.streamPos() + rt.latencySamples();
            const auto &r = ev[i].eventValues.ramp;
            rt.queueRamp(ev[i].parameter, r.endValue,
                         base + (uint64_t)r.startBufferOffset,
                         base + (uint64_t)r.startBufferOffset +
                             (uint64_t)r.durationInFrames);
            if (ev[i].parameter >= 1 && ev[i].parameter <= kNumAuParams)
                au->paramShadow[ev[i].parameter - 1] = r.endValue;
        }
    }
    return noErr;
}

/* ---------------- MIDI ---------------- */

static OSStatus au_MIDIEvent(void *self, UInt32 status, UInt32 data1, UInt32 data2,
                             UInt32 offset) {
    HarpAU *au = (HarpAU *)self;
    auto &rt = au->rt;
    uint64_t ts = rt.streamPos() + rt.latencySamples() + offset;
    UInt32 kind = status & 0xF0;
    if (kind == 0x90 && data2 > 0) {
        rt.queueNote(ump_note_on(data1, data2), ts);
    } else if (kind == 0x80 || (kind == 0x90 && data2 == 0)) {
        rt.queueNote(ump_note_off(data1), ts);
    } else if (kind == 0xB0 && (data1 == 120 || data1 == 123)) {
        rt.queueNote(ump_all_notes_off(), 0); /* panic, now */
    }
    return noErr;
}

static OSStatus au_SysEx(void *, const UInt8 *, UInt32) { return noErr; }

static OSStatus au_StartNote(void *self, MusicDeviceInstrumentID,
                             MusicDeviceGroupID, NoteInstanceID *outID,
                             UInt32 offset, const MusicDeviceNoteParams *params) {
    if (!params) return kAudio_ParamError;
    UInt32 note = (UInt32)params->mPitch & 0x7f;
    UInt32 vel = (UInt32)params->mVelocity & 0x7f;
    if (outID) *outID = note;
    return au_MIDIEvent(self, 0x90, note, vel ? vel : 64, offset);
}

static OSStatus au_StopNote(void *self, MusicDeviceGroupID, NoteInstanceID id,
                            UInt32 offset) {
    return au_MIDIEvent(self, 0x80, (UInt32)id & 0x7f, 0, offset);
}

/* ---------------- render ---------------- */

static OSStatus au_Render(void *self, AudioUnitRenderActionFlags *ioFlags,
                          const AudioTimeStamp *ts, UInt32 bus, UInt32 nFrames,
                          AudioBufferList *ioData) {
    HarpAU *au = (HarpAU *)self;
    (void)ioFlags;
    (void)ts;
    if (bus != 0) return kAudioUnitErr_InvalidElement;
    if (!au->initialized) return kAudioUnitErr_Uninitialized;
    if (nFrames > au->maxFrames) return kAudioUnitErr_TooManyFramesToProcess;
    if (!ioData || ioData->mNumberBuffers < 1) return kAudio_ParamError;

    auto &rt = au->rt;

    /* §9.7: the host's musical clock, via HostCallbacks */
    if (au->hostCallbacks.beatAndTempoProc) {
        Float64 beat = 0, tempo = 0;
        bool posV = au->hostCallbacks.beatAndTempoProc(au->hostCallbacks.hostUserData,
                                                       &beat, &tempo) == noErr;
        Boolean playing = false, changed = false, looping = false;
        Float64 inLoopStart = 0, inLoopEnd = 0, samplePos = 0;
        if (au->hostCallbacks.transportStateProc)
            au->hostCallbacks.transportStateProc(au->hostCallbacks.hostUserData,
                                                 &playing, &changed, &samplePos,
                                                 &looping, &inLoopStart, &inLoopEnd);
        rt.feedTransport(playing, posV && tempo > 0, tempo, posV, beat, nFrames,
                         rt.streamPos() + rt.latencySamples());
    }

    /* pull interleaved, deinterleave into the host's buffers (or ours,
     * when the host passes mData = NULL) */
    if (au->offline)
        rt.pullAudioBlocking(au->interleaved, nFrames, 1000);
    else
        rt.pullAudio(au->interleaved, nFrames);

    float *dst[2];
    for (UInt32 b = 0; b < 2; b++) {
        AudioBuffer *ab = &ioData->mBuffers[b < ioData->mNumberBuffers ? b : 0];
        if (!ab->mData) {
            ab->mData = au->scratch[b];
            ab->mDataByteSize = nFrames * sizeof(float);
        }
        dst[b] = (float *)ab->mData;
    }
    if (ioData->mNumberBuffers >= 2) {
        for (UInt32 s = 0; s < nFrames; s++) {
            dst[0][s] = au->interleaved[2 * s];
            dst[1][s] = au->interleaved[2 * s + 1];
        }
    } else { /* mono host bus: sum */
        for (UInt32 s = 0; s < nFrames; s++)
            dst[0][s] =
                0.5f * (au->interleaved[2 * s] + au->interleaved[2 * s + 1]);
    }
    return noErr;
}

/* ---------------- dispatch ---------------- */

static OSStatus au_Open(void *self, AudioComponentInstance inst) {
    ((HarpAU *)self)->compInstance = inst;
    return noErr;
}

static OSStatus au_Close(void *self) {
    HarpAU *au = (HarpAU *)self;
    if (au->initialized) au_Uninitialize(self);
    delete au;
    return noErr;
}

static AudioComponentMethod au_Lookup(SInt16 selector) {
    switch (selector) {
        case kAudioUnitInitializeSelect: return (AudioComponentMethod)au_Initialize;
        case kAudioUnitUninitializeSelect: return (AudioComponentMethod)au_Uninitialize;
        case kAudioUnitGetPropertyInfoSelect: return (AudioComponentMethod)au_GetPropertyInfo;
        case kAudioUnitGetPropertySelect: return (AudioComponentMethod)au_GetProperty;
        case kAudioUnitSetPropertySelect: return (AudioComponentMethod)au_SetProperty;
        case kAudioUnitAddPropertyListenerSelect: return (AudioComponentMethod)au_AddPropertyListener;
        case kAudioUnitRemovePropertyListenerSelect: return (AudioComponentMethod)au_RemovePropertyListener;
        case kAudioUnitRemovePropertyListenerWithUserDataSelect: return (AudioComponentMethod)au_RemovePropertyListenerWithUserData;
        case kAudioUnitAddRenderNotifySelect: return (AudioComponentMethod)au_AddRenderNotify;
        case kAudioUnitRemoveRenderNotifySelect: return (AudioComponentMethod)au_RemoveRenderNotify;
        case kAudioUnitGetParameterSelect: return (AudioComponentMethod)au_GetParameter;
        case kAudioUnitSetParameterSelect: return (AudioComponentMethod)au_SetParameter;
        case kAudioUnitScheduleParametersSelect: return (AudioComponentMethod)au_ScheduleParameters;
        case kAudioUnitRenderSelect: return (AudioComponentMethod)au_Render;
        case kAudioUnitResetSelect: return (AudioComponentMethod)au_Reset;
        case kMusicDeviceMIDIEventSelect: return (AudioComponentMethod)au_MIDIEvent;
        case kMusicDeviceSysExSelect: return (AudioComponentMethod)au_SysEx;
        case kMusicDeviceStartNoteSelect: return (AudioComponentMethod)au_StartNote;
        case kMusicDeviceStopNoteSelect: return (AudioComponentMethod)au_StopNote;
        default: return nullptr;
    }
}

extern "C" void *HarpAUFactory(const AudioComponentDescription *) {
    HarpAU *au = new (std::nothrow) HarpAU();
    if (!au) return nullptr;
    au->iface.Open = au_Open;
    au->iface.Close = au_Close;
    au->iface.Lookup = au_Lookup;
    au->iface.reserved = nullptr;
    return au;
}
