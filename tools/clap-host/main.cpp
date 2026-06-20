/* clap-host — a minimal CLI CLAP host for automated, agent-driven testing, the
 * CLAP analogue of harp-vst3-host. It dlopen()s a .clap, instantiates the
 * plugin, drives it offline (host-paced, byte-exact) with params + notes +
 * per-note PARAM_MOD, captures the owner main mix, and prints its FNV-1a hash —
 * so the CLAP shell can be gated against the SAME golden hashes as the VST3/AU
 * shells (shared harp_fnv1a in tools/render_check.h).
 *
 * Render structure mirrors harp-vst3-host EXACTLY so the device sees an
 * identical event/pull stream and renders byte-identically: rate 48000, block
 * 256, kOffline, --set applied once at block 0, --chord notes held 0.1s..end at
 * velocity 0.8, --notes sequential at velocity 0.9. The per-voice modulation
 * demo uses CLAP's native per-note PARAM_MOD on Filter Cutoff (param 3): amount
 * = brightness-0.5, the SAME signed offset the VST3 Brightness Note Expression
 * produces — so a CLAP per-note mod and a VST3 note expression render the SAME
 * bytes (cross-format per-voice modulation).
 *
 * usage: clap-host PLUGIN.clap [--rate N] [--block N] [--seconds S] [--channel N]
 *        [--serial S] [--set ID=V]... [--notes N,N,..] [--chord N,N,..]
 *        [--brightness V] [--brightness-idx K] [--bend SEMIS] [--bend-idx K] [--press V] [--press-idx K] [--out FILE.wav] [--hash]
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <clap/clap.h>

#include "render_check.h" /* harp_fnv1a, harp_write_wav16 — shared with the VST3/AU hosts */

/* Cross-platform dynamic load: a .clap is a bare shared library — dlopen on POSIX,
 * LoadLibrary on Windows. Included after the std headers, with NOMINMAX so windows.h
 * does not clobber std::min/max. */
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <sys/stat.h>
#  ifndef S_ISDIR /* MSVC <sys/stat.h> has _S_IFDIR/_S_IFMT but not the POSIX macro */
#    define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#  endif
static void *harp_dlopen(const char *p) { return (void *)LoadLibraryA(p); }
static void *harp_dlsym(void *h, const char *s) { return (void *)GetProcAddress((HMODULE)h, s); }
static const char *harp_dlerror(void) { return "LoadLibrary/GetProcAddress failed"; }
static void harp_dlclose(void *h) { FreeLibrary((HMODULE)h); }
static int setenv(const char *n, const char *v, int overwrite) { (void)overwrite; return _putenv_s(n, v); }
#else
#  include <dlfcn.h>
static void *harp_dlopen(const char *p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void *harp_dlsym(void *h, const char *s) { return dlsym(h, s); }
static const char *harp_dlerror(void) { return dlerror(); }
static void harp_dlclose(void *h) { dlclose(h); }
#endif

static void die(const std::string &m) {
    fprintf(stderr, "clap-host: %s\n", m.c_str());
    exit(2);
}

/* Resolve the dlopen-able binary inside a .clap. On Linux/Windows a .clap is a
 * bare shared library — dlopen the path directly. On macOS it is a CFBundle
 * DIRECTORY (Contents/MacOS/<CFBundleExecutable>); dlopen of the directory
 * fails, so we read CFBundleExecutable from Contents/Info.plist (basename-minus-
 * .clap fallback) and dlopen the inner Mach-O. This makes the gate load EXACTLY
 * the artifact a DAW loads — the bundle — not a side dylib. The bundle path is
 * still what we hand to clap_entry->init() (the CLAP contract: init gets the
 * bundle so the plugin can find Contents/Resources). */
static std::string clap_binary_path(const std::string &clapPath) {
    struct stat st;
    if (stat(clapPath.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return clapPath; /* bare lib (Linux/Windows) — dlopen directly */
    std::string exe;
    std::ifstream plist(clapPath + "/Contents/Info.plist");
    std::string line;
    while (std::getline(plist, line)) {
        auto k = line.find("CFBundleExecutable");
        if (k == std::string::npos) continue;
        /* value is the next <string>…</string>, on this line or the following */
        std::string scan = line;
        for (int i = 0; i < 2 && scan.find("<string>") == std::string::npos; i++)
            std::getline(plist, scan);
        auto a = scan.find("<string>"), b = scan.find("</string>");
        if (a != std::string::npos && b != std::string::npos)
            exe = scan.substr(a + 8, b - (a + 8));
        break;
    }
    if (exe.empty()) { /* fallback: bundle basename without the .clap extension */
        auto slash = clapPath.find_last_of('/');
        std::string base = slash == std::string::npos ? clapPath : clapPath.substr(slash + 1);
        auto dot = base.rfind(".clap");
        exe = dot == std::string::npos ? base : base.substr(0, dot);
    }
    return clapPath + "/Contents/MacOS/" + exe;
}

/* ---- minimal host (the plugin asks for almost nothing in this harness) ---- */
static const void *host_get_extension(const clap_host_t *, const char *) { return nullptr; }
static void host_noop(const clap_host_t *) {}
static const clap_host_t g_host = {
    CLAP_VERSION_INIT, nullptr, "harp-clap-host", "HARP", "", "0.1.0",
    host_get_extension, host_noop, host_noop, host_noop};

/* ---- one block's input event list (a flat, time-sorted array) ---- */
union AnyEvent {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_param_value_t pval;
    clap_event_param_mod_t pmod;
    clap_event_note_expression_t nexp;
};
struct InEvents {
    std::vector<AnyEvent> evs;
    clap_input_events_t list;
};
static uint32_t ie_size(const clap_input_events_t *l) {
    return (uint32_t)((const InEvents *)l->ctx)->evs.size();
}
static const clap_event_header_t *ie_get(const clap_input_events_t *l, uint32_t i) {
    return &((const InEvents *)l->ctx)->evs[i].header;
}
static bool oe_try_push(const clap_output_events_t *, const clap_event_header_t *) { return true; }

int main(int argc, char **argv) {
    if (argc < 2) die("usage: clap-host PLUGIN.clap [opts] (see source)");
    std::string path = argv[1];
    uint32_t rate = 48000, block = 256;
    double seconds = 2.0, brightness = -1.0;
    int channel = 0, brightness_idx = 0;
    /* MPE note expressions, injected on a chosen chord note (per-voice proof):
     * TUNING (semitones, can be 0/negative) and PRESSURE (0..1). bool flags
     * because 0 is a meaningful value (neutral). */
    bool has_bend = false, has_press = false;
    double bend = 0.0, pressure = 0.0;
    int bend_idx = 0, press_idx = 0;
    std::vector<std::pair<uint32_t, double>> sets;
    std::vector<int> notes, chord;
    std::string out_path;
    bool do_hash = false;
    double note_period = 0.0;
    /* §8.3-over-§8.7 mid-stream toggle test: render LIVE (free-running) until this time,
     * then flip to OFFLINE on the LIVE active plugin (render->set on a live session -> the
     * shell re-dials host-paced). <0 = the default clean bounce (offline set before
     * activate). With it set, `tail-hash:` covers [toggle..end] — the deterministic
     * post-toggle window; run twice and compare to prove the toggle re-dialed host-paced. */
    double toggle_offline_at = -1.0;

    auto next = [&](int &i) -> std::string {
        if (i + 1 >= argc) die("missing arg after " + std::string(argv[i]));
        return argv[++i];
    };
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rate") rate = (uint32_t)atoi(next(i).c_str());
        else if (a == "--block") block = (uint32_t)atoi(next(i).c_str());
        else if (a == "--seconds") seconds = atof(next(i).c_str());
        else if (a == "--channel") channel = atoi(next(i).c_str());
        else if (a == "--serial") setenv("HARP_DEVICE_SERIAL", next(i).c_str(), 1);
        else if (a == "--brightness") brightness = atof(next(i).c_str());
        else if (a == "--brightness-idx") brightness_idx = atoi(next(i).c_str());
        else if (a == "--bend") { bend = atof(next(i).c_str()); has_bend = true; }
        else if (a == "--bend-idx") bend_idx = atoi(next(i).c_str());
        else if (a == "--press") { pressure = atof(next(i).c_str()); has_press = true; }
        else if (a == "--press-idx") press_idx = atoi(next(i).c_str());
        else if (a == "--out") out_path = next(i);
        else if (a == "--hash") do_hash = true;
        else if (a == "--toggle-offline-at") toggle_offline_at = atof(next(i).c_str());
        else if (a == "--set") {
            std::string s = next(i);
            size_t eq = s.find('=');
            if (eq == std::string::npos) die("--set wants ID=VALUE");
            sets.push_back({(uint32_t)atoi(s.c_str()), atof(s.c_str() + eq + 1)});
        } else if (a == "--notes" || a == "--chord") {
            std::vector<int> &dst = (a == "--notes") ? notes : chord;
            std::string list = next(i);
            for (size_t p = 0; p < list.size();) {
                dst.push_back(atoi(list.c_str() + p));
                size_t c = list.find(',', p);
                if (c == std::string::npos) break;
                p = c + 1;
            }
        } else die("unknown option " + a);
    }
    if (note_period == 0.0) note_period = 0.6; /* --notes spacing; matches harp-vst3-host's default */

    /* ---- load the .clap (bare lib on Linux/Windows, CFBundle on macOS) ---- */
    std::string binary = clap_binary_path(path); /* inner Mach-O if a bundle */
    void *dso = harp_dlopen(binary.c_str());
    if (!dso) die("dlopen failed: " + std::string(harp_dlerror() ? harp_dlerror() : "?"));
    auto *entry = (const clap_plugin_entry_t *)harp_dlsym(dso, "clap_entry");
    if (!entry) die("no clap_entry symbol");
    if (!entry->init(path.c_str())) die("entry init failed"); /* init() gets the bundle path */
    auto *factory = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory || factory->get_plugin_count(factory) < 1) die("no plugin factory");
    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t *plugin = factory->create_plugin(factory, &g_host, desc->id);
    if (!plugin) die("create_plugin failed");
    if (!plugin->init(plugin)) die("plugin init failed");

    /* Offline render mode is set BEFORE activate, so the shell reads it when it starts
     * the session and comes up HOST-PACED (deterministic, byte-exact). Setting it AFTER
     * activate races the async §8.7 session dial: on a slow runner the dial wins and the
     * session comes up free-running (non-deterministic), which v1 does not re-dial mid-
     * stream. (A real CLAP host that toggles render mode on a LIVE session is the
     * mid-stream-toggle case — exercised by a separate test, not this clean bounce.) */
    auto *render = (const clap_plugin_render_t *)plugin->get_extension(plugin, CLAP_EXT_RENDER);
    /* Clean bounce: offline BEFORE activate (deterministic from the first sample). Toggle
     * test (toggle_offline_at >= 0): start LIVE and flip mid-render below. */
    if (render && toggle_offline_at < 0) render->set(plugin, CLAP_RENDER_OFFLINE);
    if (!plugin->activate(plugin, (double)rate, block, block)) die("activate failed");
    if (!plugin->start_processing(plugin)) die("start_processing failed");

    /* ---- render loop: blocks of `block`, total = seconds*rate ---- */
    size_t total = (size_t)(seconds * rate);
    std::vector<float> Lbuf(block), Rbuf(block), capture;
    capture.reserve(total * 2);
    size_t onAt = (size_t)(0.1 * rate); /* --chord note-on, matches the VST3 host */
    int64_t steady = 0;
    bool first = true;
    bool toggled = false;
    size_t toggle_sample = toggle_offline_at >= 0 ? (size_t)(toggle_offline_at * rate) : 0;

    for (size_t done = 0; done < total;) {
        uint32_t n = (uint32_t)((total - done) < block ? (total - done) : block);
        bool last = done + n >= total;
        /* §8.3-over-§8.7 mid-stream toggle: flip the LIVE active plugin to OFFLINE at the
         * block boundary >= toggle_sample. render->set runs on this (main) thread and the
         * shell BLOCKS it until the host-paced re-dial is up — fine for an offline render.
         * Subsequent blocks render deterministic host-paced. */
        if (render && toggle_offline_at >= 0 && !toggled && done >= toggle_sample) {
            render->set(plugin, CLAP_RENDER_OFFLINE);
            toggled = true;
        }
        InEvents in;
        in.list.ctx = &in;
        in.list.size = ie_size;
        in.list.get = ie_get;

        auto hdr = [](AnyEvent &e, uint16_t type, uint32_t size, uint32_t time) {
            e.header.size = size;
            e.header.time = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = type;
            e.header.flags = 0;
        };
        if (first) { /* params set once, at block 0 (matches the VST3 host) */
            for (auto &kv : sets) {
                AnyEvent e{};
                hdr(e, CLAP_EVENT_PARAM_VALUE, sizeof e.pval, 0);
                e.pval.param_id = kv.first;
                e.pval.note_id = -1;
                e.pval.port_index = -1;
                e.pval.channel = -1;
                e.pval.key = -1;
                e.pval.value = kv.second;
                in.evs.push_back(e);
            }
        }
        /* --chord: each note held 0.1s..end, vel 0.8, note id = index */
        int32_t nid = 0;
        for (int cn : chord) {
            if (onAt >= done && onAt < done + n) {
                AnyEvent e{};
                hdr(e, CLAP_EVENT_NOTE_ON, sizeof e.note, (uint32_t)(onAt - done));
                e.note.note_id = nid;
                e.note.port_index = 0;
                e.note.channel = (int16_t)channel;
                e.note.key = (int16_t)cn;
                e.note.velocity = 0.8;
                in.evs.push_back(e);
                /* --brightness on chord note `brightness_idx`: CLAP-native per-note
                 * PARAM_MOD on Filter Cutoff. amount = brightness-0.5 == the signed
                 * offset the VST3 Brightness Note Expression yields, so the two
                 * formats render byte-identically. */
                if (brightness >= 0.0 && nid == brightness_idx) {
                    AnyEvent m{};
                    hdr(m, CLAP_EVENT_PARAM_MOD, sizeof m.pmod, (uint32_t)(onAt - done));
                    m.pmod.param_id = 3; /* Filter Cutoff */
                    m.pmod.note_id = nid;
                    m.pmod.port_index = 0;
                    m.pmod.channel = (int16_t)channel;
                    m.pmod.key = (int16_t)cn;
                    m.pmod.amount = brightness - 0.5;
                    in.evs.push_back(m);
                }
                /* MPE note expressions on this note (the §9.5 per-voice path): a
                 * TUNING (pitch bend, semitones) and/or PRESSURE (loudness, 0..1)
                 * on chord note `bend_idx`/`press_idx`. */
                auto nexpr = [&](uint16_t exprId, double value) {
                    AnyEvent x{};
                    hdr(x, CLAP_EVENT_NOTE_EXPRESSION, sizeof x.nexp, (uint32_t)(onAt - done));
                    x.nexp.expression_id = exprId;
                    x.nexp.note_id = nid;
                    x.nexp.port_index = 0;
                    x.nexp.channel = (int16_t)channel;
                    x.nexp.key = (int16_t)cn;
                    x.nexp.value = value;
                    in.evs.push_back(x);
                };
                if (has_bend && nid == bend_idx) nexpr(CLAP_NOTE_EXPRESSION_TUNING, bend);
                if (has_press && nid == press_idx) nexpr(CLAP_NOTE_EXPRESSION_PRESSURE, pressure);
            }
            if (last) {
                AnyEvent e{};
                hdr(e, CLAP_EVENT_NOTE_OFF, sizeof e.note, n > 0 ? n - 1 : 0);
                e.note.note_id = nid;
                e.note.port_index = 0;
                e.note.channel = (int16_t)channel;
                e.note.key = (int16_t)cn;
                e.note.velocity = 0.0;
                in.evs.push_back(e);
            }
            nid++;
        }
        /* --notes: sequential, vel 0.9, on at ni*period, off at on+0.75*period */
        for (size_t ni = 0; ni < notes.size(); ni++) {
            int64_t on = (int64_t)((double)ni * note_period * rate);
            int64_t off = on + (int64_t)(0.75 * note_period * rate);
            if (on >= (int64_t)done && on < (int64_t)(done + n)) {
                AnyEvent e{};
                hdr(e, CLAP_EVENT_NOTE_ON, sizeof e.note, (uint32_t)(on - (int64_t)done));
                e.note.note_id = 1000 + (int32_t)ni;
                e.note.port_index = 0;
                e.note.channel = (int16_t)channel;
                e.note.key = (int16_t)notes[ni];
                e.note.velocity = 0.9;
                in.evs.push_back(e);
            }
            if (off >= (int64_t)done && off < (int64_t)(done + n)) {
                AnyEvent e{};
                hdr(e, CLAP_EVENT_NOTE_OFF, sizeof e.note, (uint32_t)(off - (int64_t)done));
                e.note.note_id = 1000 + (int32_t)ni;
                e.note.port_index = 0;
                e.note.channel = (int16_t)channel;
                e.note.key = (int16_t)notes[ni];
                e.note.velocity = 0.0;
                in.evs.push_back(e);
            }
        }

        clap_output_events_t oe{nullptr, oe_try_push};
        float *chans[2] = {Lbuf.data(), Rbuf.data()};
        clap_audio_buffer_t ab{};
        ab.data32 = chans;
        ab.channel_count = 2;
        clap_process_t pc{};
        pc.steady_time = steady;
        pc.frames_count = n;
        pc.transport = nullptr; /* free-running (no arp in the CLAP gate) */
        pc.audio_inputs = nullptr;
        pc.audio_outputs = &ab;
        pc.audio_inputs_count = 0;
        pc.audio_outputs_count = 1;
        pc.in_events = &in.list;
        pc.out_events = &oe;
        if (plugin->process(plugin, &pc) < 0) die("process failed");

        for (uint32_t s = 0; s < n; s++) { /* interleaved L,R — matches the VST3 host */
            capture.push_back(Lbuf[s]);
            capture.push_back(Rbuf[s]);
        }
        steady += n;
        done += n;
        first = false;
    }

    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    harp_dlclose(dso);

    double rms = 0;
    for (float v : capture) rms += (double)v * v;
    rms = capture.empty() ? 0 : sqrt(rms / capture.size());
    if (!out_path.empty()) harp_write_wav16(out_path, capture, 2, rate);
    if (do_hash) {
        uint64_t h = harp_fnv1a(capture.data(), capture.size() * sizeof(float));
        char hex[17];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
        printf("output-hash: %s\n", hex);
    }
    /* §8.3-over-§8.7 toggle test: hash ONLY the post-toggle window [toggle..end] (the
     * pre-toggle live/free-running portion is non-deterministic and excluded). Two runs
     * must match IFF the toggle re-dialed to deterministic host-paced. */
    if (toggle_offline_at >= 0) {
        size_t tail = toggle_sample * 2; /* interleaved stereo */
        if (tail > capture.size()) tail = capture.size();
        uint64_t th = harp_fnv1a(capture.data() + tail, (capture.size() - tail) * sizeof(float));
        char hex[17];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)th);
        printf("tail-hash: %s\n", hex);
    }
    printf("processed %zu samples x 2 ch, rms=%.5f\n", capture.size() / 2, rms);
    return 0;
}
