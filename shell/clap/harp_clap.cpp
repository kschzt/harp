/* HARP RefDev — CLAP shell (§15): presents a HARP device to a CLAP host.
 *
 * The THIRD shell over the one shared HarpRuntime core (VST3 = shell/plugin.cpp,
 * AU = shell/au/harp_au.mm). Same wiring: acquire a runtime for the pinned
 * serial, configure/start it (owner) or attach to it (P5 registry), turn each
 * block's CLAP events into §9.x stream events (notes -> UMP, params -> sets/
 * ramps, transport -> §9.7, and — the modern bit — CLAP's native per-note
 * PARAM_MOD / Brightness Note Expression -> §9.5 per-voice modulation), and
 * pull the device's host-paced audio back. getState/setState write the SAME
 * Recall Bundle header (shell_constants.h) as VST3/AU, so a project moves
 * between all three formats byte-identically.
 *
 * CLAP's polyphonic parameter modulation (clap_event_param_mod with a note_id)
 * is a first-class match for HARP's §9.5 per-voice mod — no Note-Expression
 * translation needed, the host addresses a param on a single voice directly.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <clap/clap.h>

#include "note_voice_map.h"
#include "runtime.h"
#include "runtime_registry.h"
#include "shell_constants.h"
#include "ump.h"

namespace {

/* Mirrors the refdev's parameter set + the VST3/AU shells (device ids 1..13).
 * The CLAP param id IS the device param id, so a PARAM_VALUE/PARAM_MOD event's
 * param_id routes straight through to queueParamSet / queueMod. */
struct DevParam {
    uint32_t id;
    const char *name;
    double def; /* mirrors the device defaults (recall sanity) */
};
static const DevParam kParams[] = {
    {1, "Osc Pitch", 0.5},   {2, "Osc Shape", 0.6},    {3, "Filter Cutoff", 0.7},
    {4, "Filter Reso", 0.5}, {5, "Env Attack", 0.5},   {6, "Env Release", 0.5},
    {7, "FX Send", 0.5},     {8, "Master Level", 0.5},  {9, "Arp Mode", 0.0},
    {10, "Arp Division", 0.6}, {11, "Arp Gate", 0.5},   {12, "Arp Octaves", 0.0},
    {13, "Glide", 0.0},
};
static constexpr uint32_t kNumParams = sizeof(kParams) / sizeof(kParams[0]);
static constexpr uint32_t kCutoffId = 3; /* the per-voice-modulatable param (§9.5) */

static uint8_t envChannelDefault() {
    if (const char *e = getenv("HARP_CHANNEL"); e && e[0]) {
        int v = atoi(e);
        if (v >= 0 && v <= 15) return (uint8_t)v;
    }
    return 0;
}

/* ------------------------------------------------------------------ plugin */
struct HarpClap {
    clap_plugin_t plugin;       /* MUST be first: host holds a clap_plugin_t* */
    const clap_host_t *host = nullptr;

    RuntimeHandle handle{};
    EventSource *source = nullptr;
    double rate = 48000.0;
    uint32_t maxFrames = 4096;
    bool offline = false; /* render ext: offline bounce -> blocking pull (byte-exact) */
    uint8_t part = envChannelDefault(); /* §9.4 multitimbral part (HARP_CHANNEL) */
    std::vector<uint8_t> pendingState;  /* staged by state.load before activate */
    double paramVals[kNumParams];       /* cache for params.get_value (UI only) */
    std::vector<float> interleaved;     /* pull scratch (stereo) */

    /* noteId -> §9.5 voice key, so a PARAM_MOD / Note Expression can target the
     * exact ringing voice — the SAME bridge the VST3 shell uses (note_voice_map.h). */
    NoteVoiceMap noteVoices;

    HarpRuntime *rt() const { return handle.rt; }
    bool owner() const { return handle.owner; }

    void releaseSource() {
        if (source && rt()) rt()->unregisterSource(source);
        source = nullptr;
    }
};

static HarpClap *self(const clap_plugin_t *p) { return (HarpClap *)p->plugin_data; }

/* ------------------------------------------------------------- audio-ports */
static uint32_t ap_count(const clap_plugin_t *, bool is_input) { return is_input ? 0 : 1; }
static bool ap_get(const clap_plugin_t *, uint32_t index, bool is_input,
                   clap_audio_port_info_t *info) {
    if (is_input || index != 0) return false;
    info->id = 0;
    snprintf(info->name, sizeof info->name, "Stereo Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t s_audio_ports = {ap_count, ap_get};

/* -------------------------------------------------------------- note-ports */
static uint32_t np_count(const clap_plugin_t *, bool is_input) { return is_input ? 1 : 0; }
static bool np_get(const clap_plugin_t *, uint32_t index, bool is_input,
                   clap_note_port_info_t *info) {
    if (!is_input || index != 0) return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    snprintf(info->name, sizeof info->name, "Note In");
    return true;
}
static const clap_plugin_note_ports_t s_note_ports = {np_count, np_get};

/* ------------------------------------------------------------------ params */
static uint32_t pa_count(const clap_plugin_t *) { return kNumParams; }
static bool pa_get_info(const clap_plugin_t *, uint32_t index, clap_param_info_t *info) {
    if (index >= kNumParams) return false;
    memset(info, 0, sizeof *info);
    info->id = kParams[index].id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    /* Filter Cutoff advertises CLAP's per-note modulation — the §9.5 capability
     * the device exposes (state.c PFLAG_PER_VOICE_MOD); a CLAP host can then send
     * a per-note PARAM_MOD that we forward as a per-voice mod. */
    if (kParams[index].id == kCutoffId)
        info->flags |= CLAP_PARAM_IS_MODULATABLE | CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID;
    info->min_value = 0.0;
    info->max_value = 1.0;
    info->default_value = kParams[index].def;
    snprintf(info->name, sizeof info->name, "%s", kParams[index].name);
    return true;
}
static bool pa_get_value(const clap_plugin_t *p, clap_id id, double *out) {
    HarpClap *h = self(p);
    for (uint32_t i = 0; i < kNumParams; i++)
        if (kParams[i].id == id) { *out = h->paramVals[i]; return true; }
    return false;
}
static bool pa_value_to_text(const clap_plugin_t *, clap_id, double value, char *out,
                             uint32_t size) {
    snprintf(out, size, "%.3f", value);
    return true;
}
static bool pa_text_to_value(const clap_plugin_t *, clap_id, const char *text, double *out) {
    *out = atof(text);
    return true;
}
/* flush: apply param events while NOT processing (the host may tweak knobs when
 * the plugin is idle). Notes/audio do not flow here — params only. */
static void pa_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                     const clap_output_events_t *);
static const clap_plugin_params_t s_params = {pa_count,         pa_get_info, pa_get_value,
                                              pa_value_to_text, pa_text_to_value, pa_flush};

/* ------------------------------------------------------------------- state */
static bool st_save(const clap_plugin_t *p, const clap_ostream_t *os) {
    HarpClap *h = self(p);
    if (!h->rt()) return false;
    std::vector<uint8_t> bundle;
    if (!h->rt()->getStateBundle(bundle)) return false;
    /* SAME header as VST3/AU getState: magic + part byte, then the bundle. The
     * device gets the identical bundle on reload (load strips the header), so a
     * project round-trips byte-transparently across all three formats. */
    uint8_t header[kStateHeaderLen];
    memcpy(header, kStateHeaderMagic, sizeof kStateHeaderMagic);
    header[sizeof kStateHeaderMagic] = (uint8_t)(h->part & 0xf);
    if (os->write(os, header, sizeof header) != (int64_t)sizeof header) return false;
    size_t off = 0;
    while (off < bundle.size()) {
        int64_t w = os->write(os, bundle.data() + off, bundle.size() - off);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    return true;
}
static bool st_load(const clap_plugin_t *p, const clap_istream_t *is) {
    HarpClap *h = self(p);
    std::vector<uint8_t> raw;
    uint8_t buf[8192];
    for (;;) {
        int64_t r = is->read(is, buf, sizeof buf);
        if (r < 0) return false;
        if (r == 0) break;
        raw.insert(raw.end(), buf, buf + r);
    }
    /* strip the shared header (magic + part), recall the per-project part */
    if (raw.size() >= kStateHeaderLen &&
        memcmp(raw.data(), kStateHeaderMagic, sizeof kStateHeaderMagic) == 0) {
        h->part = (uint8_t)(raw[sizeof kStateHeaderMagic] & 0xf);
        raw.erase(raw.begin(), raw.begin() + kStateHeaderLen);
    }
    if (h->rt() && h->owner())
        h->rt()->setStateBundle(raw.data(), raw.size()); /* live reload */
    else
        h->pendingState = raw; /* stage; activate() applies before start() */
    return true;
}
static const clap_plugin_state_t s_state = {st_save, st_load};

/* ------------------------------------------------------------------ render */
static bool rn_has_hard_realtime(const clap_plugin_t *) { return false; }
static bool rn_set(const clap_plugin_t *p, clap_plugin_render_mode mode) {
    self(p)->offline = (mode == CLAP_RENDER_OFFLINE);
    return true;
}
static const clap_plugin_render_t s_render = {rn_has_hard_realtime, rn_set};

/* ----------------------------------------------------------------- latency */
static uint32_t lt_get(const clap_plugin_t *p) {
    HarpClap *h = self(p);
    return h->rt() ? h->rt()->latencySamples() : 0;
}
static const clap_plugin_latency_t s_latency = {lt_get};

/* ----------------------------------------------------- per-block event apply */
/* Turn ONE CLAP input event into stream events on our source. `base` is the
 * block's stream position (already offset by latency); each event adds its own
 * sample offset. Shared by process() and params flush(). */
static void applyEvent(HarpClap *h, const clap_event_header_t *hdr, uint64_t base) {
    if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    HarpRuntime &rt = *h->rt();
    uint64_t ts = base + hdr->time;
    switch (hdr->type) {
    case CLAP_EVENT_NOTE_ON: {
        auto *e = (const clap_event_note *)hdr;
        uint32_t note = (uint32_t)(e->key & 0x7f);
        uint32_t chan = (uint32_t)(e->channel & 0xf);
        uint32_t vel = (uint32_t)(e->velocity * 127.0 + 0.5);
        if (vel == 0) vel = 1;
        if (vel > 127) vel = 127;
        rt.queueNote(h->source, ump_note_on(note, vel, chan), ts);
        h->noteVoices.noteOn(e->note_id, (chan << 8) | note);
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        auto *e = (const clap_event_note *)hdr;
        uint32_t note = (uint32_t)(e->key & 0x7f);
        uint32_t chan = (uint32_t)(e->channel & 0xf);
        rt.queueNote(h->source, ump_note_off(note, chan), ts);
        h->noteVoices.noteOff((chan << 8) | note);
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        auto *e = (const clap_event_param_value *)hdr;
        rt.queueParamSet(h->source, (uint32_t)e->param_id, (float)e->value, ts);
        for (uint32_t i = 0; i < kNumParams; i++)
            if (kParams[i].id == e->param_id) h->paramVals[i] = e->value;
        break;
    }
    case CLAP_EVENT_PARAM_MOD: {
        /* CLAP-native modulation -> §9.4/§9.5. A note_id scopes it to ONE voice
         * (per-voice); note_id == -1 is part-wide. The amount IS the signed
         * offset on the param's base — exactly HARP's mod semantics. */
        auto *e = (const clap_event_param_mod *)hdr;
        rt.queueMod(h->source, (uint32_t)e->param_id, (float)e->amount,
                    h->noteVoices.voiceFor(e->note_id), ts);
        break;
    }
    case CLAP_EVENT_NOTE_EXPRESSION: {
        /* Brightness -> per-voice Filter Cutoff, identical mapping to the VST3
         * shell (value 0..1, 0.5 = no change). Other expressions ignored. */
        auto *e = (const clap_event_note_expression *)hdr;
        if (e->expression_id == CLAP_NOTE_EXPRESSION_BRIGHTNESS)
            rt.queueMod(h->source, kCutoffId, (float)(e->value - 0.5),
                        h->noteVoices.voiceFor(e->note_id), ts);
        break;
    }
    default: break; /* transport is handled in process() — it needs the block size */
    }
}

/* §9.7 transport from the block header (the owner only). Separate from
 * applyEvent because feedTransport needs the block frame count for its end-ppq
 * extrapolation, exactly like the VST3/AU shells. */
static void applyTransport(HarpClap *h, const clap_event_transport_t *e, uint32_t frames,
                           uint64_t base) {
    bool playing = (e->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
    bool hasTempo = (e->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0;
    bool hasBeats = (e->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
    double beat = (double)e->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
    h->rt()->feedTransport(playing, hasTempo && e->tempo > 0, e->tempo, hasBeats, beat, frames,
                           base);
}

static void pa_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                     const clap_output_events_t *) {
    HarpClap *h = self(p);
    if (!h->rt() || !h->source || !in) return;
    uint64_t base = h->rt()->streamPos() + h->rt()->latencySamples();
    uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; i++) {
        const clap_event_header_t *hdr = in->get(in, i);
        if (hdr && hdr->type == CLAP_EVENT_PARAM_VALUE) applyEvent(h, hdr, base);
    }
}

/* --------------------------------------------------------- plugin lifecycle */
static bool pl_init(const clap_plugin_t *p) {
    HarpClap *h = self(p);
    for (uint32_t i = 0; i < kNumParams; i++) h->paramVals[i] = kParams[i].def;
    return true;
}

static void pl_destroy(const clap_plugin_t *p) {
    HarpClap *h = self(p);
    h->releaseSource();
    runtime_release(h->handle);
    h->handle = RuntimeHandle{};
    delete h;
}

static bool pl_activate(const clap_plugin_t *p, double sample_rate, uint32_t /*min*/,
                        uint32_t max_frames) {
    HarpClap *h = self(p);
    h->rate = sample_rate;
    h->maxFrames = max_frames;
    h->interleaved.assign((size_t)max_frames * 2, 0.0f);

    /* acquire a runtime for the pinned serial — the SAME sequence the VST3/AU
     * shells run at setActive: HARP_DEVICE_SERIAL env, else the serial named in
     * a staged Recall Bundle, else "" (a fresh owner runtime). */
    std::string wantSerial;
    if (const char *e = getenv("HARP_DEVICE_SERIAL"); e && e[0])
        wantSerial = e;
    else if (!h->pendingState.empty())
        wantSerial =
            HarpRuntime::bundleWantedSerial(h->pendingState.data(), h->pendingState.size());
    h->handle = runtime_acquire(wantSerial);
    if (!h->rt()) return false;

    if (h->owner()) {
        /* owner drives the session: configure -> stage project bundle -> start.
         * Byte-identical ordering to the VST3/AU single-instance path. */
        h->rt()->configure((uint32_t)h->rate, h->maxFrames);
        if (!h->pendingState.empty())
            h->rt()->setStateBundle(h->pendingState.data(), h->pendingState.size());
        h->rt()->start((uint32_t)h->rate);
        h->source = h->rt()->ownerSource();
    } else {
        /* attached: the shared session is already configured/started; register
         * our per-part source so our notes/params merge onto it (P5). */
        h->source = h->rt()->registerSource(h->part);
    }
    return true;
}

static void pl_deactivate(const clap_plugin_t *p) {
    HarpClap *h = self(p);
    h->releaseSource();
    runtime_release(h->handle);
    h->handle = RuntimeHandle{};
}

static bool pl_start_processing(const clap_plugin_t *) { return true; }
static void pl_stop_processing(const clap_plugin_t *) {}

static void pl_reset(const clap_plugin_t *p) {
    HarpClap *h = self(p);
    h->noteVoices.reset();
}

static void writeSilence(const clap_process_t *pc) {
    if (pc->audio_outputs_count < 1 || !pc->audio_outputs[0].data32) return;
    for (uint32_t c = 0; c < pc->audio_outputs[0].channel_count; c++)
        memset(pc->audio_outputs[0].data32[c], 0, pc->frames_count * sizeof(float));
}

static clap_process_status pl_process(const clap_plugin_t *p, const clap_process_t *pc) {
    HarpClap *h = self(p);
    if (!h->rt() || !h->source) { writeSilence(pc); return CLAP_PROCESS_CONTINUE; }

    uint64_t base = h->rt()->streamPos() + h->rt()->latencySamples();

    /* The host delivers in_events sorted by sample offset; we emit each as a
     * timestamped stream event on OUR source. The owner also drives transport. */
    if (pc->in_events) {
        uint32_t ne = pc->in_events->size(pc->in_events);
        for (uint32_t i = 0; i < ne; i++) {
            const clap_event_header_t *hdr = pc->in_events->get(pc->in_events, i);
            if (!hdr) continue;
            applyEvent(h, hdr, base);
        }
    }
    if (h->owner() && pc->transport) applyTransport(h, pc->transport, pc->frames_count, base);

    /* pull the device's host-paced audio and de-interleave into the stereo bus.
     * Offline bounce blocks for the exact samples (deterministic / byte-exact);
     * realtime takes what's there (silence on underrun) — same policy as VST3. */
    if (pc->audio_outputs_count < 1 || !pc->audio_outputs[0].data32 ||
        pc->audio_outputs[0].channel_count < 1) {
        writeSilence(pc);
        return CLAP_PROCESS_CONTINUE;
    }
    float *L = pc->audio_outputs[0].data32[0];
    float *R = pc->audio_outputs[0].channel_count > 1 ? pc->audio_outputs[0].data32[1] : nullptr;

    uint32_t remaining = pc->frames_count, written = 0;
    float *tmp = h->interleaved.data();
    while (remaining > 0) {
        uint32_t chunk = remaining > h->maxFrames ? h->maxFrames : remaining;
        if (h->offline)
            h->rt()->pullAudioBlocking(tmp, chunk, 1000);
        else
            h->rt()->pullAudio(tmp, chunk);
        for (uint32_t s = 0; s < chunk; s++) {
            L[written + s] = tmp[2 * s];
            if (R) R[written + s] = tmp[2 * s + 1];
        }
        written += chunk;
        remaining -= chunk;
    }
    return CLAP_PROCESS_CONTINUE;
}

static const void *pl_get_extension(const clap_plugin_t *, const char *id) {
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &s_note_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_params;
    if (!strcmp(id, CLAP_EXT_STATE)) return &s_state;
    if (!strcmp(id, CLAP_EXT_RENDER)) return &s_render;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &s_latency;
    return nullptr;
}
static void pl_on_main_thread(const clap_plugin_t *) {}

/* ----------------------------------------------------------- factory + entry */
static const char *kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                  CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                  CLAP_PLUGIN_FEATURE_STEREO, nullptr};
static const clap_plugin_descriptor_t kDesc = {
    CLAP_VERSION_INIT,
    "org.harp.refdev.clap",
    "HARP RefDev",
    "HARP",
    "",
    "",
    "",
    "0.1.0",
    "HARP reference device — multitimbral, polyphonic, per-voice modulation (CLAP)",
    kFeatures,
};

static const clap_plugin_t *factory_create(const clap_plugin_factory_t *, const clap_host_t *host,
                                           const char *plugin_id) {
    if (!plugin_id || strcmp(plugin_id, kDesc.id)) return nullptr;
    HarpClap *h = new HarpClap();
    h->host = host;
    h->plugin.desc = &kDesc;
    h->plugin.plugin_data = h;
    h->plugin.init = pl_init;
    h->plugin.destroy = pl_destroy;
    h->plugin.activate = pl_activate;
    h->plugin.deactivate = pl_deactivate;
    h->plugin.start_processing = pl_start_processing;
    h->plugin.stop_processing = pl_stop_processing;
    h->plugin.reset = pl_reset;
    h->plugin.process = pl_process;
    h->plugin.get_extension = pl_get_extension;
    h->plugin.on_main_thread = pl_on_main_thread;
    return &h->plugin;
}

static uint32_t factory_count(const clap_plugin_factory_t *) { return 1; }
static const clap_plugin_descriptor_t *factory_get_desc(const clap_plugin_factory_t *,
                                                        uint32_t index) {
    return index == 0 ? &kDesc : nullptr;
}
static const clap_plugin_factory_t s_factory = {factory_count, factory_get_desc, factory_create};

static bool entry_init(const char *) { return true; }
static void entry_deinit(void) {}
static const void *entry_get_factory(const char *factory_id) {
    return strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &s_factory;
}

} // namespace

extern "C" const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit,
                                                   entry_get_factory};
