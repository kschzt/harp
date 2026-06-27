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
#include <memory>
#include <new>
#include <vector>

#include "runtime.h"
#include "runtime_registry.h"
#include "shell_constants.h"
#include "ump.h"

#include <string>

/* identity (also in Info.plist's AudioComponents entry — keep in sync) */
#define HARP_AU_TYPE 'aumu'
#define HARP_AU_SUBTYPE 'rfdv'
#define HARP_AU_MANU 'HARP'
#define HARP_AU_VERSION 0x00010000

/* mirrors the device's parameter set (ids 1..12, CONTIGUOUS since engine 2.1.0,
 * normalized 0..1). The drone's old id 7 — briefly mis-shipped here as a phantom
 * "FX Send" — is gone and the set was renumbered with no hole, so the id<->index
 * map (id == element index, param `elem`/`param` is 1-based) holds exactly. */
struct AuParam {
    AudioUnitParameterID id;
    const char *name;
};
static const AuParam kAuParams[] = {
    {1, "Osc Pitch"},   {2, "Osc Shape"},    {3, "Filter Cutoff"},
    {4, "Filter Reso"}, {5, "Env Attack"},   {6, "Env Release"},
    {7, "Master Level"}, {8, "Arp Mode"},   /* Master Level was id 8 */
    {9, "Arp Division"}, {10, "Arp Gate"},  {11, "Arp Octaves"},
    {12, "Glide"}, /* was id 13 */
};
static constexpr UInt32 kNumAuParams = sizeof(kAuParams) / sizeof(kAuParams[0]);

/* §9.9 OUTPUT METERS. The device's readonly per-part + main-mix peak/RMS meters
 * (id range 0x1000+, shared scheme in shell_constants.h, mirroring
 * device/device.h) are surfaced as READONLY AudioUnitParameters: reported in the
 * ParameterList AFTER the 12 device params and the Part router, with ParameterInfo
 * flags that are READABLE but NOT WRITABLE (the AU read-only equivalent of VST3's
 * kIsReadOnly), so a host shows live meters but cannot write/automate them (§9.9).
 * The shell never sets them — their values arrive through the SAME device echo
 * path the front-panel echo uses; au_GetParameter drains meter echoes into a
 * meterShadow and returns them. Additive + readonly + never on the render/event
 * path => the single-instance golden render is byte-identical (the determinism
 * gate). The id self-encodes the part/slot/metric so the echo path is unchanged. */
static_assert(kMeterIdBase == 0x1000u && kNumMeterParams == 34,
              "meter id scheme must mirror device/device.h (METER_ID_BASE/NSLOTS)");

/* Human-readable meter param name into `buf` (mirrors the VST3 shell's
 * meterParamName so both formats show identical labels). */
static void au_meter_name(AudioUnitParameterID id, char *buf, size_t n) {
    uint32_t k = (uint32_t)id - kMeterIdBase;
    uint32_t slot = k / 2, metric = k & 1;
    const char *mname = metric ? "RMS" : "Peak";
    if (slot == kMeterMainSlot)
        snprintf(buf, n, "Meter Main %s", mname);
    else
        snprintf(buf, n, "Meter Part %u %s", slot, mname);
}

/* The recall component-state header (kStateHeaderMagic/kStateHeaderLen) is SHARED
 * with the VST3 + CLAP shells via shell_constants.h — all formats must agree for
 * a project's component state to move between them byte-for-byte
 * (cross-format-recall-test.sh). Its part byte is now FROZEN 0x00 (the multi-out
 * main owns all 16 parts; the retired per-instance "Part" param is gone). */

struct HarpAU {
    AudioComponentPlugInInterface iface; /* MUST be first */
    AudioComponentInstance compInstance = nullptr;

    /* The runtime this AU instance drives — its OWN, private, owned by unique_ptr
     * (the process-global sharing registry is retired: one multi-out main instance
     * per device claim, nothing to share). Behaves BYTE-IDENTICALLY to the old
     * by-value member / the old empty-serial owner: this instance configures it,
     * pulls the main mix + every per-part demux sink, anchors transport, get/sets
     * state, and queues events on the runtime's built-in owner source. Two
     * instances on different units bind different devices because
     * HarpRuntime::selectDevice() does, not a shared table. */
    std::unique_ptr<HarpRuntime> rt_;
    /* This instance's event SOURCE (P5): the runtime's built-in owner source for
     * an owner, a per-instance registered source for an attached one. All queue*
     * route through it so each instance is the SOLE producer of its own SPSC ring.
     * nullptr until acquired (au_Render emits silence, queue* drop). */
    EventSource *source = nullptr;
    /* P5b per-part AUDIO sink: an ATTACHED instance that OPTED IN (HARP_PART_AUDIO)
     * registers a sink for its part's stereo pair; the owner's reader demuxes the
     * shared stream into it and au_Render pulls IT instead of emitting silence.
     * nullptr for the owner / default audio-silent attached instance. */
    AudioSink *sink = nullptr;

    /* MULTI-OUT (M4): the Kontakt/Overbridge-style multi-out main — ONE instance owns
     * the whole device and exposes 17 stereo output elements: element 0 = the summed
     * MAIN MIX (byte-identical to the single-output path), elements 1..16 = the per-part
     * pairs. Exactly mirrors the VST3 shell (HarpProcessor) and the CLAP plugin.
     *   partBusActive_[k]  — the host SET a stream format on output element k+1 (the AU
     *                        analogue of VST3's activateBus). DEFAULT false, so a normal
     *                        single-output instance (only element 0 touched) registers NO
     *                        part sinks and streams the 2-slot union — BYTE-IDENTICAL to
     *                        the shipped golden. Set in au_SetProperty(StreamFormat).
     *   partSinks_[k]      — the owner's demux sink for part k (slots {2+2k,3+2k}); the
     *                        runtime reader demuxes the shared stream into each. Registered
     *                        BEFORE start() for the active buses (registerActivePartSinks).
     *   partBuf_[k]        — per-element interleaved L/R cache. AU renders ONE element per
     *                        call (unlike VST3/CLAP's single all-bus call), and the MAIN
     *                        pull is the session clock (advances ssiRead_), so element 0
     *                        pulls the main mix AND drains every active part sink into its
     *                        cache; elements 1..16 then serve their cached block. This keeps
     *                        the single device clock and drains every registered ring once
     *                        per cycle (no overflow), order-independent across elements. */
    static constexpr int kNumParts = 16;
    bool partBusActive_[kNumParts] = {false};
    AudioSink *partSinks_[kNumParts] = {nullptr};
    float *partBuf_[kNumParts] = {nullptr};   /* maxFrames*2 each (interleaved) */
    UInt32 cachedFrames_ = 0;                  /* frames in partBuf_ from the last element-0 pull */

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
    /* §9.9 readonly meter shadow: latest echoed peak/RMS per slot+metric (the
     * device owns truth; the meter echo updates this so hosts read live values).
     * Indexed by (id - kMeterIdBase); silent floor (0) until the first echo. */
    float meterShadow[kNumMeterParams];

    /* Raw recall bundle staged by au_apply_classinfo (setState) BEFORE a runtime
     * exists — ClassInfo is restored on project-open, before AudioUnitInitialize.
     * The OWNER applies it via setStateBundle() right before start(), reproducing
     * the VST3 shell's stage-before-start ordering; it is also the source of the
     * wanted serial used as the registry key when no HARP_DEVICE_SERIAL env pins
     * one. */
    std::vector<uint8_t> pendingState;

    /* render scratch: backs ABL buffers when the host passes mData=NULL */
    float *scratch[2] = {nullptr, nullptr};
    float *interleaved = nullptr;

    HarpAU() {
        for (UInt32 i = 0; i < kNumAuParams; i++) paramShadow[i] = 0.5f;
        for (UInt32 i = 0; i < kNumMeterParams; i++) meterShadow[i] = 0.0f; /* silent floor */
        paramShadow[7] = 0.0f;  /* Arp Mode off (id 8 -> idx 7, post-2.1.0 renumber) */
        paramShadow[8] = 0.6f;  /* Division 1/16 (id 9 -> idx 8) */
        paramShadow[10] = 0.0f; /* Octaves 1 (id 11 -> idx 10) */
        paramShadow[11] = 0.0f; /* Glide off (id 12 -> idx 11; was [12] — an OOB write past paramShadow[12]) */
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
        /* Defensive teardown: au_Close calls au_Uninitialize first (which drops the
         * source then releases the handle), but if a host disposes us without
         * uninitializing, release here so the shared refcount is correct and a
         * private runtime is not leaked — the same net cleanup the old by-value
         * member's ~HarpRuntime gave. Both calls are idempotent. */
        releaseSource(); /* drop our audio sinks before the runtime */
        rt_.reset();
        free(scratch[0]);
        free(scratch[1]);
        free(interleaved);
        for (int k = 0; k < kNumParts; k++) free(partBuf_[k]);
        if (presentPreset.presetName) CFRelease(presentPreset.presetName);
    }

    HarpRuntime *runtime() const { return rt_.get(); }

    /* Drop our audio sinks before the runtime is torn down (after which the reader
     * never demuxes into a freed ring). The event source is the runtime's own owner
     * source — freed with the runtime. MUST run before rt_.reset(). Idempotent.
     * Mirrors HarpProcessor::releaseSource. */
    void releaseSource() {
        if (sink && runtime()) runtime()->unregisterAudioSink(sink);
        sink = nullptr;
        /* MULTI-OUT (M4): drop the per-part demux sinks (sinks before the source). */
        for (int k = 0; k < kNumParts; k++) {
            if (partSinks_[k] && runtime()) runtime()->unregisterAudioSink(partSinks_[k]);
            partSinks_[k] = nullptr;
        }
        /* source is the runtime's own owner source — freed with the runtime; just
         * forget it here (run before rt_.reset()). */
        source = nullptr;
    }

    /* MULTI-OUT (M4): register a demux sink for each ACTIVE part output element, BEFORE
     * start() so the per-part slot pairs enter the audio.start union (the P5b mid-attach
     * limitation — a sink added after start needs a re-negotiation). Mirrors the VST3
     * shell's registerActivePartSinks exactly: only buses the host gave a stream format
     * (partBusActive_) stream, so a single-output instance keeps the 2-slot main-mix union
     * (byte-identical golden). Owner only; idempotent. */
    void registerActivePartSinks() {
        if (!runtime()) return;
        for (int k = 0; k < kNumParts; k++) {
            if (partBusActive_[k] && !partSinks_[k]) {
                std::vector<uint32_t> slots = {2u + 2u * (uint32_t)k, 3u + 2u * (uint32_t)k};
                partSinks_[k] = runtime()->registerAudioSink(slots);
            }
        }
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
    bool ok = au->scratch[0] && au->scratch[1] && au->interleaved;
    /* MULTI-OUT (M4): one interleaved L/R cache per part element, sized to maxFrames.
     * Element 0's pull drains every active part sink into these; elements 1..16 serve
     * their cache. Reallocated here whenever maxFrames changes (host re-config). */
    for (int k = 0; k < HarpAU::kNumParts; k++) {
        free(au->partBuf_[k]);
        au->partBuf_[k] = (float *)calloc((size_t)au->maxFrames * 2, sizeof(float));
        ok = ok && au->partBuf_[k];
    }
    au->cachedFrames_ = 0;
    return ok ? noErr : kAudio_MemFullError;
}

/* ---------------- lifecycle ---------------- */

static OSStatus au_Initialize(void *self) {
    HarpAU *au = (HarpAU *)self;
    if (au->initialized) return noErr;
    OSStatus rc = au_alloc_scratch(au);
    if (rc != noErr) return rc;

    uint32_t rate = (uint32_t)au->outFormat.mSampleRate;

    /* Idempotent against a redundant activate (drop any runtime we already hold
     * before re-acquiring). Drop our audio sinks first, same ordering rule as
     * release. Mirrors HarpProcessor::setActive. */
    if (au->rt_) {
        au->releaseSource();
        au->rt_.reset();
    }

    /* Construct THIS instance's private runtime (no sharing — every instance owns
     * its own). Device SELECTION is the runtime's own job (selectDevice():
     * HARP_DEVICE_SERIAL env, else the staged bundle's serial via setStateBundle
     * below, else auto-select). */
    au->rt_ = runtime_acquire();
    HarpRuntime *rt = au->runtime();
    if (!rt) return kAudioUnitErr_FailedInitialization; /* never on success path */

    /* Drive it exactly as the old by-value runtime did — configure, stage the
     * project's bundle (stage-before-start), then start. The event source is the
     * runtime's built-in ownerSource_ (its channel is seeded inside start() from
     * HARP_CHANNEL — byte-identical single-instance path). */
    rt->configure(rate, au->maxFrames);
    rt->setOffline(au->offline); /* §8.3-over-§8.7: host-paced eth if offline (before start) */
    if (!au->pendingState.empty())
        rt->setStateBundle(au->pendingState.data(), au->pendingState.size());
    /* MULTI-OUT (M4): register the active part demux sinks into the audio.start union
     * BEFORE start() — exactly the VST3 ordering. A single-output host activated no
     * part buses, so this is a no-op and the main-mix union is byte-identical. */
    au->registerActivePartSinks();
    rt->start(rate); /* deviceless = silence */
    au->source = rt->ownerSource();
    /* The owner source's default channel is seeded inside start() from HARP_CHANNEL
     * (the headless --channel path); notes carry their own channel->part in the
     * UMP, so there's nothing per-instance to pin. */
    au->initialized = true;
    return noErr;
}

static OSStatus au_Uninitialize(void *self) {
    HarpAU *au = (HarpAU *)self;
    if (!au->initialized) return noErr;
    /* Drop our audio sinks before tearing the runtime down (after which the reader
     * never demuxes into a freed ring), then reset the runtime — its dtor
     * (~HarpRuntime) stop()s + joins its threads. Mirrors HarpProcessor::setActive(false). */
    au->releaseSource();
    au->rt_.reset();
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

    /* the Recall Bundle — identical bytes to the VST3 shell's getState, INCLUDING
     * the P6 versioned header (magic + part byte) prepended ahead of the bundle.
     * getStateBundle is a ctlMutex_-guarded READ of the shared device state, so it
     * is safe for an ATTACHED instance too (it reports the same project state the
     * shared session holds). With no runtime yet (queried before activate) we fall
     * back to the staged pending bundle so a host querying ClassInfo early still
     * round-trips the project state + Part. */
    std::vector<uint8_t> bundle;
    bool haveBundle =
        au->runtime() ? au->runtime()->getStateBundle(bundle) : false;
    if (!haveBundle && !au->pendingState.empty()) {
        bundle = au->pendingState; /* not yet activated: persist what we staged */
        haveBundle = true;
    }
    if (haveBundle && !bundle.empty()) {
        /* header + bundle in ONE contiguous CFData, byte-identical to the VST3's
         * IBStream write order (header first, then the unchanged bundle bytes). */
        std::vector<uint8_t> blob;
        blob.reserve(kStateHeaderLen + bundle.size());
        blob.insert(blob.end(), kStateHeaderMagic,
                    kStateHeaderMagic + sizeof kStateHeaderMagic);
        blob.push_back((uint8_t)0x00); /* part byte FROZEN 0x00 (multi-out main owns all parts) */
        blob.insert(blob.end(), bundle.begin(), bundle.end());
        CFDataRef data = CFDataCreate(nullptr, blob.data(), (CFIndex)blob.size());
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
    if (data && CFGetTypeID(data) == CFDataGetTypeID()) {
        const uint8_t *raw = CFDataGetBytePtr(data);
        size_t rawLen = (size_t)CFDataGetLength(data);
        /* A NEW blob begins with the versioned header (magic + part byte); strip
         * it. The part byte is FROZEN 0x00 / IGNORED (the multi-out main owns all
         * parts). An OLD header-less blob (first byte a CBOR map, never the magic)
         * loads byte-compatibly. Everything past the header is the SAME bundle
         * bytes the device round-trips, byte-identical to the VST3 shell. */
        const uint8_t *bundle = raw;
        size_t blen = rawLen;
        if (rawLen >= kStateHeaderLen &&
            memcmp(raw, kStateHeaderMagic, sizeof kStateHeaderMagic) == 0) {
            bundle += kStateHeaderLen;
            blen -= kStateHeaderLen;
        }
        /* else: MIGRATION — header-less old blob -> Part stays at its env/default
         * seed (0), bundle is the whole blob. */
        if (blen > 0) {
            /* ALWAYS stash the raw bundle: it is this instance's project state and
             * the source of the registry's wanted serial at acquire time. (ClassInfo
             * usually lands before AudioUnitInitialize, so the runtime does not exist
             * yet — staging here reproduces stage-before-start; the owner applies it
             * just before start().) */
            au->pendingState.assign(bundle, bundle + blen);
            /* Live instance: push now, exactly as before. No runtime yet: staged
             * above; it is applied at activate. */
            if (au->runtime())
                au->runtime()->setStateBundle(bundle, blen);
        }
    }
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
            /* 12 device params + the host-side "Part" router (id 98) + the §9.9
             * readonly meter params (ids 0x1000+) */
            size = scope == kAudioUnitScope_Global
                       ? (kNumAuParams + 1 + kNumMeterParams) * sizeof(AudioUnitParameterID)
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
            if (scope == kAudioUnitScope_Output && elem > (UInt32)HarpAU::kNumParts)
                return kAudioUnitErr_InvalidElement; /* 0..16 only */
            /* All 17 output elements share one stereo float format. */
            *(AudioStreamBasicDescription *)outData = au->outFormat;
            *ioSize = sizeof(AudioStreamBasicDescription);
            return noErr;
        case kAudioUnitProperty_ElementCount:
            /* MULTI-OUT (M4): no input; 17 OUTPUT elements (0 = main mix, 1..16 = parts),
             * the Kontakt-style multi-out main. A host that only wires element 0 gets the
             * byte-identical single-output behaviour (no part bus activated -> no sink). */
            *(UInt32 *)outData =
                scope == kAudioUnitScope_Input ? 0 : (UInt32)(HarpAU::kNumParts + 1);
            *ioSize = sizeof(UInt32);
            return noErr;
        case kAudioUnitProperty_MaximumFramesPerSlice:
            *(UInt32 *)outData = au->maxFrames;
            *ioSize = sizeof(UInt32);
            return noErr;
        case kAudioUnitProperty_Latency: {
            if (scope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
            /* Reported latency is a PURE function of the DAW block size, so it is
             * byte-identical whether asked of the live runtime (an OWNER, or an
             * ATTACHED instance reporting the same value as the owner — full PDC)
             * or computed statically before a runtime is acquired (mirrors the VST3
             * shell's getLatencySamples). */
            uint32_t lat = au->runtime() ? au->runtime()->latencySamples()
                                         : HarpRuntime::latencyFor(au->maxFrames);
            *(Float64 *)outData = (Float64)lat / au->outFormat.mSampleRate;
            *ioSize = sizeof(Float64);
            return noErr;
        }
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
            /* the device params, then the §9.9 readonly meter params (ids 0x1000+,
             * peak/rms per slot). (The host-side "Part" router is retired.) */
            const UInt32 total = kNumAuParams + kNumMeterParams;
            UInt32 n = *ioSize / sizeof(AudioUnitParameterID);
            if (n > total) n = total;
            auto *ids = (AudioUnitParameterID *)outData;
            for (UInt32 i = 0; i < n; i++) {
                if (i < kNumAuParams)
                    ids[i] = kAuParams[i].id;
                else
                    ids[i] = kMeterIdBase + (i - kNumAuParams); /* contiguous meter range */
            }
            *ioSize = n * sizeof(AudioUnitParameterID);
            return noErr;
        }
        case kAudioUnitProperty_ParameterInfo: {
            auto *info = (AudioUnitParameterInfo *)outData;
            memset(info, 0, sizeof *info);
            if (isMeterId((uint32_t)elem)) {
                /* §9.9 readonly meter: READABLE but NOT WRITABLE — the AU
                 * read-only equivalent of VST3's kIsReadOnly. A host shows the
                 * live value but cannot write or automate it. Generic 0..1 range
                 * (peak/RMS are already normalized by the device); meterReadable. */
                char nm[64];
                au_meter_name((AudioUnitParameterID)elem, nm, sizeof nm);
                info->flags = kAudioUnitParameterFlag_IsReadable |
                              kAudioUnitParameterFlag_MeterReadOnly |
                              kAudioUnitParameterFlag_HasCFNameString |
                              kAudioUnitParameterFlag_CFNameRelease;
                info->cfNameString =
                    CFStringCreateWithCString(nullptr, nm, kCFStringEncodingUTF8);
                strncpy(info->name, nm, sizeof info->name - 1);
                info->unit = kAudioUnitParameterUnit_LinearGain;
                info->minValue = 0;
                info->maxValue = 1;
                info->defaultValue = 0;
                *ioSize = sizeof(AudioUnitParameterInfo);
                return noErr;
            }
            if (elem < 1 || elem > kNumAuParams) return kAudioUnitErr_InvalidElement;
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
             * with the audio graph, not against it. The block captures `au`
             * and runs during render (the runtime is acquired by then); a
             * null runtime (handed the workgroup before activate) is a no-op,
             * and the workgroup is re-applied at the next render once acquired.
             * For a shared session every attached instance points at the same
             * runtime, so the join is idempotent across the group. */
            AURenderContextObserver obs = ^(const AudioUnitRenderContext *ctx) {
              if (HarpRuntime *rt = au->runtime())
                  rt->setWorkgroup(ctx ? ctx->workgroup : nullptr);
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
            if (scope == kAudioUnitScope_Output && elem > (UInt32)HarpAU::kNumParts)
                return kAudioUnitErr_InvalidElement; /* only 0..16 exist */
            const auto *f = (const AudioStreamBasicDescription *)inData;
            if (f->mFormatID != kAudioFormatLinearPCM ||
                f->mChannelsPerFrame != 2 || f->mBitsPerChannel != 32 ||
                !(f->mFormatFlags & kAudioFormatFlagIsFloat) ||
                !(f->mFormatFlags & kAudioFormatFlagIsNonInterleaved))
                return kAudioUnitErr_FormatNotSupported;
            /* All 17 output elements are stereo float; one shared outFormat (sample rate)
             * suffices. MULTI-OUT (M4): a format SET on a PART element (1..16) is the host
             * declaring it will wire that part — the AU analogue of VST3 activateBus. It
             * arms the demux sink that au_Initialize registers before start(). Element 0
             * (main mix) is always present and never needs arming. */
            if (scope == kAudioUnitScope_Output && elem >= 1)
                au->partBusActive_[elem - 1] = true;
            au->outFormat = *f;
            au_notify(au, prop, scope, elem);
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
            /* §8.3-over-§8.7: select host-paced (deterministic) for an Ethernet
             * offline bounce before the session starts; no-op on USB / no runtime. */
            if (au->runtime()) au->runtime()->setOffline(au->offline);
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
    if (param < 1 || (param > kNumAuParams && !isMeterId((uint32_t)param)))
        return kAudioUnitErr_InvalidParameter;
    /* drain device echoes into the shadows so hosts see panel moves AND live
     * meters. ONE drain feeds both: device-param echoes (ids 1..12) -> paramShadow,
     * §9.9 readonly meter echoes (ids 0x1000+, the SAME evt 'param' path) ->
     * meterShadow, indexed by (id - kMeterIdBase). The echo ring is single-consumer
     * (this instance is its sole owner, like the VST3 shell's process()); an
     * unacquired instance must NOT pop it (it would corrupt the SPSC ring). */
    if (au->runtime()) {
        uint32_t id;
        float v;
        /* multi-out main owns ALL 16 parts: drain every part's echo ring (empty
         * parts yield nothing, so a part-0-only render is byte-identical). */
        for (uint32_t p = 0; p < (uint32_t)HarpAU::kNumParts; p++)
            while (au->runtime()->popEcho(p, id, v)) {
                if (id >= 1 && id <= kNumAuParams)
                    au->paramShadow[id - 1] = v;
                else if (isMeterId(id))
                    au->meterShadow[id - kMeterIdBase] = v;
            }
    }
    *out = isMeterId((uint32_t)param) ? au->meterShadow[(uint32_t)param - kMeterIdBase]
                                      : au->paramShadow[param - 1];
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
    /* Queue on THIS instance's source (P5): the runtime's built-in owner source
     * for an owner, this instance's registered source for an attached one. With a
     * single owner instance the eventPump's merge is exactly the pre-P5 single-ring
     * path — byte-identical wire. nullptr source (queried before activate) drops
     * the event (queue* is a no-op on a null source). */
    if (HarpRuntime *rt = au->runtime())
        rt->queueParamSet(au->source, param, value,
                          offset ? rt->streamPos() + rt->latencySamples() + offset
                                 : 0);
    return noErr;
}

static OSStatus au_ScheduleParameters(void *self, const AudioUnitParameterEvent *ev,
                                      UInt32 n) {
    HarpAU *au = (HarpAU *)self;
    for (UInt32 i = 0; i < n; i++) {
        if (ev[i].eventType == kParameterEvent_Immediate)
            au_SetParameter(self, ev[i].parameter, ev[i].scope, ev[i].element,
                            ev[i].eventValues.immediate.value,
                            ev[i].eventValues.immediate.bufferOffset);
        else { /* ramp: start/end values over a frame span -> §9.4 ramp */
            HarpRuntime *rt = au->runtime();
            const auto &r = ev[i].eventValues.ramp;
            if (rt) {
                uint64_t base = rt->streamPos() + rt->latencySamples();
                /* on THIS instance's source (P5), like au_SetParameter */
                rt->queueRamp(au->source, ev[i].parameter, r.endValue,
                              base + (uint64_t)r.startBufferOffset,
                              base + (uint64_t)r.startBufferOffset +
                                  (uint64_t)r.durationInFrames);
            }
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
    HarpRuntime *rt = au->runtime();
    if (!rt) return noErr; /* not acquired yet: nothing to drive */
    uint64_t ts = rt->streamPos() + rt->latencySamples() + offset;
    UInt32 kind = status & 0xF0;
    uint8_t chan = (uint8_t)(status & 0x0F); /* MIDI channel (-> device part, P2.1) */
    /* queue on THIS instance's source (P5): the owner's built-in source for an
     * owner, this instance's registered source for an attached one. */
    EventSource *src = au->source;
    /* MIDI channel C -> device part C (§9.4 multitimbral): raw MIDI routes each
     * note to its own channel's part directly. (The raw-16ch MPE zone-collapse is
     * retired with the multi-out model; AU per-note expression went with it.
     * Routing-by-channel is byte-identical to the old MPE-zone-inactive path.) */
    if (kind == 0x90 && data2 > 0) {
        rt->queueNote(src, ump_note_on(data1, data2, chan), ts);
    } else if (kind == 0x80 || (kind == 0x90 && data2 == 0)) {
        rt->queueNote(src, ump_note_off(data1, chan), ts);
    } else if (kind == 0xB0) { /* control change */
        if (data1 >= kPerChanCcBase && data1 < kPerChanCcBase + kNumAuParams) {
            /* MULTI-OUT (M2/M4): a GP CC kPerChanCcBase+i on channel N edits part N's
             * device param (i+1) with §9.4 key 5 = N — so ONE main instance drives every
             * part's params (a satellite's MIDI CC on its channel). Raw MIDI carries the
             * channel directly (no IMidiMapping roundtrip, like CLAP). */
            rt->queueParamSet(src, (uint32_t)(data1 - kPerChanCcBase) + 1u,
                              (float)data2 / 127.0f, ts, chan);
        } else if (data1 == 120 || data1 == 123) {
            rt->queueNote(src, ump_all_notes_off(), 0); /* panic, now */
        }
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

/* Resolve the host's (or our scratch) output channel pointers, filling mData when
 * the host passed NULL — shared by the audio and silence paths. */
static void au_resolve_dst(HarpAU *au, AudioBufferList *ioData, UInt32 nFrames,
                           float *dst[2]) {
    for (UInt32 b = 0; b < 2; b++) {
        AudioBuffer *ab = &ioData->mBuffers[b < ioData->mNumberBuffers ? b : 0];
        if (!ab->mData) {
            ab->mData = au->scratch[b];
            ab->mDataByteSize = nFrames * sizeof(float);
        }
        dst[b] = (float *)ab->mData;
    }
}

/* Zero the output bus: the dormant-attached default path (P5) and the pre-acquire
 * path route here — no runtime touch, just clean silence so the host bus is
 * well-defined (the AU analogue of HarpProcessor::processSilence). */
static OSStatus au_render_silence(HarpAU *au, AudioBufferList *ioData,
                                  UInt32 nFrames) {
    float *dst[2];
    au_resolve_dst(au, ioData, nFrames, dst);
    if (ioData->mNumberBuffers >= 2) {
        memset(dst[0], 0, nFrames * sizeof(float));
        memset(dst[1], 0, nFrames * sizeof(float));
    } else {
        memset(dst[0], 0, nFrames * sizeof(float));
    }
    return noErr;
}

/* Deinterleave one stereo interleaved block into the host's output (or our scratch),
 * summing L+R for a mono host bus. Shared by the main-mix and per-part element paths. */
static void au_write_stereo(HarpAU *au, AudioBufferList *ioData, UInt32 nFrames,
                            const float *interleaved) {
    float *dst[2];
    au_resolve_dst(au, ioData, nFrames, dst);
    if (ioData->mNumberBuffers >= 2) {
        for (UInt32 s = 0; s < nFrames; s++) {
            dst[0][s] = interleaved[2 * s];
            dst[1][s] = interleaved[2 * s + 1];
        }
    } else { /* mono host bus: sum */
        for (UInt32 s = 0; s < nFrames; s++)
            dst[0][s] = 0.5f * (interleaved[2 * s] + interleaved[2 * s + 1]);
    }
}

static OSStatus au_Render(void *self, AudioUnitRenderActionFlags *ioFlags,
                          const AudioTimeStamp *ts, UInt32 bus, UInt32 nFrames,
                          AudioBufferList *ioData) {
    HarpAU *au = (HarpAU *)self;
    (void)ioFlags;
    (void)ts;
    if (!au->initialized) return kAudioUnitErr_Uninitialized;
    if (nFrames > au->maxFrames) return kAudioUnitErr_TooManyFramesToProcess;
    if (!ioData || ioData->mNumberBuffers < 1) return kAudio_ParamError;
    if (bus > (UInt32)HarpAU::kNumParts) return kAudioUnitErr_InvalidElement; /* 0..16 */

    HarpRuntime *rt = au->runtime();
    /* Unacquired (rendered before activate): clean silence. */
    if (!rt || !au->source) return au_render_silence(au, ioData, nFrames);

    /* MULTI-OUT (M4) PART ELEMENTS (1..16): serve the cache element 0's pull filled THIS
     * cycle. AU renders one element per call, so the device clock (the main-mix pull that
     * advances ssiRead_) lives on element 0; it also drains every active part sink into
     * partBuf_. Here we just hand back part (bus-1)'s cached block. An inactive/unrouted
     * part, or a frame count that doesn't match the cached block -> clean silence. No
     * runtime touch on this path. */
    if (bus >= 1) {
        int k = (int)bus - 1;
        if (au->partSinks_[k] && au->cachedFrames_ == nFrames)
            au_write_stereo(au, ioData, nFrames, au->partBuf_[k]);
        else
            au_render_silence(au, ioData, nFrames);
        return noErr;
    }

    /* ELEMENT 0 (main mix) — the session clock: transport + the device pull, once/cycle. */

    /* §9.7 transport: OWNER ONLY. Transport is global (the owner anchors the
     * session's musical time); an attached feedTransport would push a second,
     * conflicting stream. (queueTransport pins it to the owner source anyway, but
     * the change DETECTOR must run on one thread — the owner's.) Mirrors the VST3
     * shell's process(). */
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
        rt->feedTransport(playing, posV && tempo > 0, tempo, posV, beat, nFrames,
                          rt->streamPos() + rt->latencySamples());
    }

    /* AUDIO. Pull the session's MAIN MIX (the one main-ring consumer — it sums every
     * part) AND drain every active part sink into the element cache: the SAME pull set,
     * in the SAME order, as the VST3 shell's process() (main bus 0, then each part bus). */
    if (au->offline)
        rt->pullAudioBlocking(au->interleaved, nFrames, 1000);
    else
        rt->pullAudio(au->interleaved, nFrames); /* RT: silence on underrun */
    /* drain every active part sink for THIS cycle. The reader demuxes one device frame
     * into ALL rings together, so after the main pull each sink ring holds the same
     * range; draining fills partBuf_ and keeps the rings from overflowing (the reader
     * is their sole producer — an undrained sink would stall). */
    for (int k = 0; k < HarpAU::kNumParts; k++) {
        if (!au->partSinks_[k]) continue;
        if (au->offline)
            rt->pullAudioBlocking(au->partSinks_[k], au->partBuf_[k], nFrames, 1000);
        else
            rt->pullAudio(au->partSinks_[k], au->partBuf_[k], nFrames);
    }
    au->cachedFrames_ = nFrames;

    au_write_stereo(au, ioData, nFrames, au->interleaved);
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
