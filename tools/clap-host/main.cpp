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
 *        [--brightness V] [--brightness-idx K] [--out FILE.wav] [--hash]
 */
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <clap/clap.h>

#include "render_check.h" /* harp_fnv1a, harp_write_wav16 — shared with the VST3/AU hosts */

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
    std::vector<std::pair<uint32_t, double>> sets;
    std::vector<int> notes, chord;
    std::string out_path;
    bool do_hash = false;
    double note_period = 0.0;

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
        else if (a == "--out") out_path = next(i);
        else if (a == "--hash") do_hash = true;
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
    void *dso = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!dso) die("dlopen failed: " + std::string(dlerror() ? dlerror() : "?"));
    auto *entry = (const clap_plugin_entry_t *)dlsym(dso, "clap_entry");
    if (!entry) die("no clap_entry symbol");
    if (!entry->init(path.c_str())) die("entry init failed"); /* init() gets the bundle path */
    auto *factory = (const clap_plugin_factory_t *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory || factory->get_plugin_count(factory) < 1) die("no plugin factory");
    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);
    const clap_plugin_t *plugin = factory->create_plugin(factory, &g_host, desc->id);
    if (!plugin) die("create_plugin failed");
    if (!plugin->init(plugin)) die("plugin init failed");
    if (!plugin->activate(plugin, (double)rate, block, block)) die("activate failed");

    /* offline render mode -> the shell blocks for the exact samples (byte-exact) */
    auto *render = (const clap_plugin_render_t *)plugin->get_extension(plugin, CLAP_EXT_RENDER);
    if (render) render->set(plugin, CLAP_RENDER_OFFLINE);
    if (!plugin->start_processing(plugin)) die("start_processing failed");

    /* ---- render loop: blocks of `block`, total = seconds*rate ---- */
    size_t total = (size_t)(seconds * rate);
    std::vector<float> Lbuf(block), Rbuf(block), capture;
    capture.reserve(total * 2);
    size_t onAt = (size_t)(0.1 * rate); /* --chord note-on, matches the VST3 host */
    int64_t steady = 0;
    bool first = true;

    for (size_t done = 0; done < total;) {
        uint32_t n = (uint32_t)((total - done) < block ? (total - done) : block);
        bool last = done + n >= total;
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
    dlclose(dso);

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
    printf("processed %zu samples x 2 ch, rms=%.5f\n", capture.size() / 2, rms);
    return 0;
}
