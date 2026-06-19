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

#include "mpe_zone.h"
#include "runtime.h"
#include "runtime_registry.h"
#include "shell_constants.h"
#include "ump.h"

/* mpe_zone.h carries its own copies of the §9.5 expression mod-target ids (so it
 * stays self-contained); pin them to the shared shell constants here, exactly as
 * the meter ids are pinned against device.h. A drift is a compile error, not a
 * silently mis-routed MPE axis. (Timbre = Filter Cutoff, device param id 3.) */
static_assert(HARP_MPE_MOD_PITCH_BEND == kHarpModPitchBend, "MPE pitch-bend id drift");
static_assert(HARP_MPE_MOD_PRESSURE == kHarpModPressure, "MPE pressure id drift");
static_assert(HARP_MPE_MOD_TIMBRE == 3u, "MPE timbre must be Filter Cutoff (param 3)");

#include <string>

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

/* §9.9 OUTPUT METERS. The device's readonly per-part + main-mix peak/RMS meters
 * (id range 0x1000+, shared scheme in shell_constants.h, mirroring
 * device/device.h) are surfaced as READONLY AudioUnitParameters: reported in the
 * ParameterList AFTER the 13 device params and the Part router, with ParameterInfo
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

/* The "Part" routing parameter id (98), its step count, and the recall
 * component-state header (kStateHeaderMagic/kStateHeaderLen) are SHARED with the
 * VST3 shell via shell_constants.h — both formats must agree for a project's
 * component state to move between VST3 and AU byte-for-byte (P6;
 * cross-format-recall-test.sh). HOST-SIDE routing only: au_SetParameter special-
 * cases id 98 out of the device param-set path (exactly as the VST3 process()
 * does), so it never affects the wire or param-map-hash. The Part is reported
 * AFTER the 13 device params in the parameter list; default 0 => part 0, the
 * single-instance/golden default. kPartParamId is used where an
 * AudioUnitParameterID is expected (== uint32_t). */

struct HarpAU {
    AudioComponentPlugInInterface iface; /* MUST be first */
    AudioComponentInstance compInstance = nullptr;

    /* P4: the runtime this AU instance drives — obtained from the PROCESS-GLOBAL
     * registry (shell/runtime_registry.h), NOT owned by value, exactly like the
     * VST3 shell's HarpProcessor::handle_. Instances that target the SAME unit
     * (same explicit serial) share ONE runtime / ONE USB claim — the prerequisite
     * for multitimbral aliasing (P5). An instance with NO explicit serial
     * (wantSerial == "") gets its OWN fresh, UNSHARED runtime with owner == true
     * and behaves BYTE-IDENTICALLY to the old by-value member (the golden gate).
     *   owner == true:  drive the session — configure / pull MAIN-MIX audio /
     *                   anchor transport / get+set state — and queue events on the
     *                   runtime's built-in OWNER source (byte-identical to today
     *                   for a single instance).
     *   owner == false: ATTACHED (P5) — a sibling owns and streams the shared
     *                   session. This instance queues notes/params on its OWN
     *                   registered source_ (its part); the owner's eventPump merges
     *                   every source onto the one session, so the group PLAYS
     *                   multitimbrally. It stays AUDIO-SILENT unless it opted into
     *                   per-part audio (P5b sink_), does not anchor transport, and
     *                   does not push state. */
    RuntimeHandle handle;
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

    /* This instance's multitimbral PART (§9.4 channel, 0..15) — the per-instance
     * routing the "Part" param (id 98) drives, mirroring the VST3 shell. DEFAULT =
     * HARP_CHANNEL env if set, else 0: the headless au-host path is unchanged and a
     * fresh in-DAW instance starts on part 0. au_apply_classinfo may override it
     * (recall), and a live Part param edit re-parts the source mid-session. Drives
     * the instance's event-source channel at activate (au_Initialize) and on change
     * (au_SetParameter). */
    uint8_t part = 0;

    /* Classic-MPE zone collapse (shell/mpe_zone.h). Logic and Ableton Live send
     * MPE to an AU as RAW 16-channel MIDI — one note per member channel plus
     * that note's per-note pitch bend / channel pressure / CC74 timbre. The
     * device takes the UMP channel as the multitimbral PART, so a member channel
     * must NEVER become the part: this collapses the whole zone onto `part` and
     * carries each per-note dimension as a §9.5 per-voice mod (the same wire the
     * VST3 Note-Expression path uses). Engages on an MPE Configuration Message
     * (auto-detect) — what both DAWs send when MPE is armed; INACTIVE until then,
     * so a non-MPE session is byte-identical (notes keep their own channel). */
    MpeZone mpe;

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
        paramShadow[8] = 0.0f;  /* Arp Mode off */
        paramShadow[9] = 0.6f;  /* Division 1/16 */
        paramShadow[11] = 0.0f; /* Octaves 1 */
        paramShadow[12] = 0.0f; /* Glide off */
        part = envChannelDefault();
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
        releaseSource(); /* attached: drop our event source/sink before the runtime */
        runtime_release(handle);
        handle = RuntimeHandle{};
        free(scratch[0]);
        free(scratch[1]);
        free(interleaved);
        if (presentPreset.presetName) CFRelease(presentPreset.presetName);
    }

    HarpRuntime *runtime() const { return handle.rt; }
    bool owner() const { return handle.owner; }

    /* The Part default the env pins: HARP_CHANNEL (the headless --channel path)
     * clamped 0..15, else 0 — mirrors the VST3 shell so the env path is unchanged.
     * start() ALSO reads HARP_CHANNEL into the owner source, so for the owner
     * applyPart() is a no-op vs the env; for an attached instance we pass part to
     * registerSource. */
    static uint8_t envChannelDefault() {
        if (const char *e = getenv("HARP_CHANNEL"); e && e[0]) {
            int v = atoi(e);
            if (v >= 0 && v <= 15) return (uint8_t)v;
        }
        return 0;
    }

    /* Drive THIS instance's event-source channel to `part` (its part). For the
     * owner this is the runtime's built-in owner source; for an attached instance
     * it is the source it registered. Both store into EventSource::chan (the
     * eventPump re-reads it per event), so re-parting takes effect without a
     * restart. nullptr source (unacquired / event-dormant 17th alias) = no-op. */
    void applyPart() {
        if (source) source->chan.store(part & 0xf, std::memory_order_relaxed);
        mpe.setPart(part); /* the zone collapses every MPE note + mod onto this part */
    }

    /* Drop our event source (and per-part sink). For an ATTACHED instance this
     * removes + frees what we registered (after which the owner's eventPump/reader
     * never touch them); for an owner / unacquired instance it is a no-op. MUST run
     * before runtime_release (a last release destroys the runtime). Idempotent.
     * Mirrors HarpProcessor::releaseSource ordering exactly: sink first, then
     * source, before the handle. */
    void releaseSource() {
        if (sink && runtime()) runtime()->unregisterAudioSink(sink);
        sink = nullptr;
        if (source && runtime()) runtime()->unregisterSource(source);
        source = nullptr;
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

    uint32_t rate = (uint32_t)au->outFormat.mSampleRate;

    /* Idempotent against a redundant activate (drop any handle we already hold
     * before re-acquiring, so we never leak a reference or strand a shared
     * session's refcount). Drop our event source first too, same ordering rule
     * as release. Mirrors HarpProcessor::setActive. */
    if (au->handle.rt) {
        au->releaseSource();
        runtime_release(au->handle);
        au->handle = RuntimeHandle{};
    }

    /* Acquire the (possibly shared) runtime for THIS instance's target unit, by
     * the SAME wanted-serial priority the VST3 shell computes:
     *   1. HARP_DEVICE_SERIAL env — the field/test pin.
     *   2. the loaded bundle's usb serial, if the project pinned one.
     *   3. else "" — NO explicit target -> a fresh, UNSHARED runtime that auto-
     *      selects, byte-identical to the old by-value member (the golden gate). */
    std::string wantSerial;
    if (const char *e = getenv("HARP_DEVICE_SERIAL"); e && e[0])
        wantSerial = e;
    else if (!au->pendingState.empty())
        wantSerial = HarpRuntime::bundleWantedSerial(au->pendingState.data(),
                                                     au->pendingState.size());

    au->handle = runtime_acquire(wantSerial);
    HarpRuntime *rt = au->runtime();
    if (!rt) return kAudioUnitErr_FailedInitialization; /* never on success path */

    if (au->owner()) {
        /* First/sole instance for this unit: drive it exactly as the old by-value
         * runtime did — configure, stage the project's bundle (stage-before-start),
         * then start. The owner's event source is the runtime's built-in
         * ownerSource_ (its channel is seeded inside start() from HARP_CHANNEL —
         * byte-identical single-instance path). */
        rt->configure(rate, au->maxFrames);
        rt->setOffline(au->offline); /* §8.3-over-§8.7: host-paced eth if offline (before start) */
        if (!au->pendingState.empty())
            rt->setStateBundle(au->pendingState.data(), au->pendingState.size());
        rt->start(rate); /* deviceless = silence */
        au->source = rt->ownerSource();
        /* P6: pin the owner source to THIS instance's part. start() seeds the
         * channel from HARP_CHANNEL; part was seeded from the SAME env (or recalled
         * by au_apply_classinfo), so for the env/golden path this is a no-op (part
         * 0 / the env value), and for a recalled/automated Part it asserts the
         * saved part. */
        au->applyPart();
    } else {
        /* ATTACHED (P5): the shared session is already configured/started under the
         * sibling owner. This instance registers its OWN event source on ITS part
         * and queues notes/params on it; the owner's eventPump merges every source
         * onto the one session, so the group PLAYS multitimbrally. */
        au->source = rt->registerSource(au->part);
        /* P5b per-part AUDIO (OPT-IN). DEFAULT (env unset): this attached instance
         * stays AUDIO-SILENT exactly as P5. When HARP_PART_AUDIO is set it OPTS IN
         * to its OWN part's audio — registers a per-part sink for that part's stereo
         * pair (P2.2 slots {2+2k,3+2k}), which the owner's reader demuxes out of the
         * shared device stream, and au_Render pulls THAT sink instead of silence.
         * (Register before the owner starts to enter the audio.start union — the
         * P5b mid-attach limitation; see runtime.h audioStart.) */
        if (const char *e = getenv("HARP_PART_AUDIO"); e && e[0] && e[0] != '0') {
            std::vector<uint32_t> slots = {2u + 2u * au->part, 3u + 2u * au->part};
            au->sink = rt->registerAudioSink(slots);
        }
    }
    au->initialized = true;
    return noErr;
}

static OSStatus au_Uninitialize(void *self) {
    HarpAU *au = (HarpAU *)self;
    if (!au->initialized) return noErr;
    /* Release: an ATTACHED instance first removes its event source (and per-part
     * sink) so the owner's eventPump/reader stop touching it and never touch freed
     * memory — MUST happen BEFORE runtime_release, which may be the last reference
     * and stop+destroy the runtime. Then drop the reference: the LAST holder of a
     * shared runtime stops+destroys it (joining its threads); a private (unshared)
     * runtime is torn down outright. Mirrors HarpProcessor::setActive(false). */
    au->releaseSource();
    runtime_release(au->handle);
    au->handle = RuntimeHandle{};
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
        blob.push_back((uint8_t)(au->part & 0xf));
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
        /* P6 recall-safe Part: a NEW blob begins with the versioned header (magic
         * + part byte); strip it and adopt the Part. An OLD blob is a raw recall
         * bundle (first byte a CBOR map, never the header magic): detected by the
         * magic mismatch, it loads byte-compatibly with Part=0 (left at its
         * env/default seed). Everything past the header is the SAME bundle bytes the
         * device round-trips, byte-identical to the VST3 shell's setState. */
        const uint8_t *bundle = raw;
        size_t blen = rawLen;
        if (rawLen >= kStateHeaderLen &&
            memcmp(raw, kStateHeaderMagic, sizeof kStateHeaderMagic) == 0) {
            au->part = (uint8_t)(raw[sizeof kStateHeaderMagic] & 0xf);
            au->applyPart(); /* live restore (usually a no-op pre-activate) */
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
            /* Live, sole/owning instance: push now, exactly as before. An ATTACHED
             * instance only stages locally — the shared session is the OWNER's to
             * (re)assert ("Live wins" on one device). No runtime yet: staged above;
             * the owner applies it at activate. */
            if (au->owner() && au->runtime())
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
            /* 13 device params + the host-side "Part" router (id 98) + the §9.9
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
            /* 13 device params, then the host-side "Part" router (id 98), then the
             * §9.9 readonly meter params (ids 0x1000+, peak/rms per slot) last */
            const UInt32 total = kNumAuParams + 1 + kNumMeterParams;
            UInt32 n = *ioSize / sizeof(AudioUnitParameterID);
            if (n > total) n = total;
            auto *ids = (AudioUnitParameterID *)outData;
            for (UInt32 i = 0; i < n; i++) {
                if (i < kNumAuParams)
                    ids[i] = kAuParams[i].id;
                else if (i == kNumAuParams)
                    ids[i] = kPartParamId;
                else
                    ids[i] = kMeterIdBase + (i - kNumAuParams - 1); /* contiguous meter range */
            }
            *ioSize = n * sizeof(AudioUnitParameterID);
            return noErr;
        }
        case kAudioUnitProperty_ParameterInfo: {
            auto *info = (AudioUnitParameterInfo *)outData;
            memset(info, 0, sizeof *info);
            if (elem == kPartParamId) {
                /* "Part" (id 98): the host-side multitimbral-part router. Stepped
                 * over 16 parts (0..15), integer/indexed, default 0 => part 0, the
                 * single-instance/golden default. Writable + per-instance so each
                 * alias can own a distinct part and the choice persists with the
                 * project. HOST-SIDE only — NOT a device param (it never enters the
                 * param-set path or the param-map-hash). */
                info->flags = kAudioUnitParameterFlag_IsReadable |
                              kAudioUnitParameterFlag_IsWritable |
                              kAudioUnitParameterFlag_HasCFNameString |
                              kAudioUnitParameterFlag_CFNameRelease;
                info->cfNameString =
                    CFStringCreateWithCString(nullptr, "Part", kCFStringEncodingUTF8);
                strncpy(info->name, "Part", sizeof info->name - 1);
                info->unit = kAudioUnitParameterUnit_Indexed;
                info->minValue = 0;
                info->maxValue = (Float32)kPartStepCount; /* 0..15 */
                info->defaultValue = 0;
                *ioSize = sizeof(AudioUnitParameterInfo);
                return noErr;
            }
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
    if (param == kPartParamId) { /* host-side router: surface this instance's part */
        *out = (Float32)au->part;
        return noErr;
    }
    if (param < 1 || (param > kNumAuParams && !isMeterId((uint32_t)param)))
        return kAudioUnitErr_InvalidParameter;
    /* drain device echoes into the shadows so hosts see panel moves AND live
     * meters. ONE drain feeds both: device-param echoes (ids 1..13) -> paramShadow,
     * §9.9 readonly meter echoes (ids 0x1000+, the SAME evt 'param' path) ->
     * meterShadow, indexed by (id - kMeterIdBase). The echo ring is single-consumer
     * (OWNER-only, like the VST3 shell's process()); an attached/unacquired instance
     * must NOT pop it (it would steal the owner's echoes / corrupt the SPSC ring). */
    if (au->owner() && au->runtime()) {
        uint32_t id;
        float v;
        while (au->runtime()->popEcho(id, v)) {
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
    if (param == kPartParamId) {
        /* HOST-SIDE routing only: re-part THIS instance live and do NOT queue a
         * device param-set (id 98 is not a device param). Indexed 0..15 =>
         * part = round(value), clamped. The eventPump re-reads the source channel
         * per event, so the change applies from the next event with no restart. */
        if (value < 0) value = 0;
        if (value > (Float32)kPartStepCount) value = (Float32)kPartStepCount;
        au->part = (uint8_t)(value + 0.5f);
        au->applyPart();
        return noErr;
    }
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
        else if (ev[i].parameter == kPartParamId) {
            /* a ramp on the host-side router makes no musical sense — re-part to the
             * ramp's end value immediately (host-side only, no device event). */
            au_SetParameter(self, kPartParamId, ev[i].scope, ev[i].element,
                            ev[i].eventValues.ramp.endValue, 0);
        } else { /* ramp: start/end values over a frame span -> §9.4 ramp */
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
    MpeZone &mpe = au->mpe;
    /* The zone is the single funnel for raw MIDI. With NO active zone every note
     * keeps its own channel (mpe.noteOn returns {chan,true}) and the expression
     * paths return valid=false — byte-identical to the pre-MPE handler. When a
     * zone is live (an MCM was seen) notes collapse onto the instance part and
     * the per-note bend/pressure/CC74 ride as §9.5 per-voice mods on that note's
     * voice key, exactly the wire the VST3/CLAP note-expression path emits. */
    if (kind == 0x90 && data2 > 0) {
        MpeNote r = mpe.noteOn(chan, (uint8_t)data1);
        if (r.accepted) rt->queueNote(src, ump_note_on(data1, data2, r.part), ts);
    } else if (kind == 0x80 || (kind == 0x90 && data2 == 0)) {
        MpeNote r = mpe.noteOff(chan, (uint8_t)data1);
        if (r.accepted) rt->queueNote(src, ump_note_off(data1, r.part), ts);
    } else if (kind == 0xE0) { /* pitch bend (X): data1 = 14-bit LSB, data2 = MSB */
        uint16_t v14 = (uint16_t)((data1 & 0x7f) | ((data2 & 0x7f) << 7));
        MpeMod m = mpe.pitchBend(chan, v14);
        if (m.valid) rt->queueMod(src, HARP_MPE_MOD_PITCH_BEND, m.value, m.voiceKey, ts);
    } else if (kind == 0xD0) { /* channel pressure (Z) -> per-voice loudness gain */
        MpeMod m = mpe.channelPressure(chan, (uint8_t)data1);
        if (m.valid) rt->queueMod(src, HARP_MPE_MOD_PRESSURE, m.value, m.voiceKey, ts);
    } else if (kind == 0xB0) { /* control change */
        if (data1 == 120 || data1 == 123) {
            rt->queueNote(src, ump_all_notes_off(), 0); /* panic, now */
            mpe.reset(); /* drop stale channel->voice bindings + RPN parse state */
        } else { /* CC74 timbre (Y) -> Filter Cutoff; CC 101/100/6/38 -> RPN/MCM
                  * (parsed even when inactive so an incoming MCM auto-engages). */
            MpeMod m = mpe.cc(chan, (uint8_t)data1, (uint8_t)data2);
            if (m.valid) rt->queueMod(src, HARP_MPE_MOD_TIMBRE, m.value, m.voiceKey, ts);
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

    HarpRuntime *rt = au->runtime();
    /* Unacquired (rendered before activate): clean silence. */
    if (!rt || !au->source) return au_render_silence(au, ioData, nFrames);

    const bool isOwner = au->owner();

    /* §9.7 transport: OWNER ONLY. Transport is global (the owner anchors the
     * session's musical time); an attached feedTransport would push a second,
     * conflicting stream. (queueTransport pins it to the owner source anyway, but
     * the change DETECTOR must run on one thread — the owner's.) Mirrors the VST3
     * shell's process(). */
    if (isOwner && au->hostCallbacks.beatAndTempoProc) {
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

    /* AUDIO. The OWNER pulls the shared session's MAIN MIX (the one consumer —
     * it sums every part). An ATTACHED instance is AUDIO-SILENT by default (it must
     * NOT pull the main-mix ring — a second consumer corrupts the SPSC tail and
     * steals the owner's samples), unless it OPTED IN to its own part's audio (P5b
     * sink), in which case it pulls ITS demuxed per-part ring. Mirrors the VST3
     * shell's process() exactly. */
    if (!isOwner && !au->sink) return au_render_silence(au, ioData, nFrames);

    /* pull interleaved from the relevant ring; deinterleave into the host's buffers
     * (or our scratch, when the host passes mData = NULL). The OWNER pulls the
     * main-mix ring (the no-sink pullAudio — byte-identical); an opted-in attached
     * instance pulls its demuxed per-part sink. */
    if (isOwner) {
        if (au->offline)
            rt->pullAudioBlocking(au->interleaved, nFrames, 1000);
        else
            rt->pullAudio(au->interleaved, nFrames);
    } else {
        if (au->offline)
            rt->pullAudioBlocking(au->sink, au->interleaved, nFrames, 1000);
        else
            rt->pullAudio(au->sink, au->interleaved, nFrames);
    }

    float *dst[2];
    au_resolve_dst(au, ioData, nFrames, dst);
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
