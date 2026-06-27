/* shell_config.h — per-product identity + behaviour the VST3/CLAP/AU shell reads.
 *
 * The DEFAULTS below ARE the reference-device plugin ("HARP RefDev"): with no
 * override, harp-shell compiles byte-for-byte as before. A product that ships
 * this shell as its OWN plugin (its own DAW name, class UIDs, parameter map, and
 * optional network-device filter) supplies its own values from OUTSIDE this repo
 * by configuring the build with -DHARP_SHELL_CONFIG_HEADER="<its-header>" (and
 * putting that header's directory on the include path). That header #defines the
 * macros below; the #ifndef guards here then defer to it. This keeps harp a
 * generic library with NO product-specific strings of its own.
 *
 * See tools/vst3-host/CMakeLists.txt (HARP_SHELL_VARIANT_* cache vars) for the
 * generic build hook a downstream uses.
 */
#ifndef HARP_SHELL_CONFIG_H
#define HARP_SHELL_CONFIG_H

#ifdef HARP_SHELL_CONFIG_HEADER
#include HARP_SHELL_CONFIG_HEADER /* a downstream product's overrides (defines the macros below) */
#endif

/* DAW-visible plugin name (factory stringPluginName). */
#ifndef HARP_SHELL_PLUGIN_NAME
#define HARP_SHELL_PLUGIN_NAME "HARP RefDev"
#endif

/* VST3 class UIDs (each = 4 uint32, the FUID ctor args). DISTINCT per product so a
 * DAW lists each plugin separately. NEVER change a shipped product's UIDs. */
#ifndef HARP_SHELL_PROC_FUID
#define HARP_SHELL_PROC_FUID 0xB520EC1F, 0x856F4A80, 0xA09D6455, 0x12430ACB
#endif
#ifndef HARP_SHELL_CTRL_FUID
#define HARP_SHELL_CTRL_FUID 0x3AF7D698, 0x0DB04F6E, 0x8F107EEF, 0x7480467A
#endif

/* The device parameter table: a comma-separated list of DevParam initialisers
 * {id, name, stepCount, defaultVal, labels}. `labels` is nullptr for a plain
 * (continuous or unlabelled-stepped) param, or a "A|B|C" pipe-delimited list of
 * stepCount+1 enum labels for a NAMED picker — in which case the product header
 * must also define HARP_SHELL_LABELED_PARAMS so the StringListParameter path is
 * compiled in. Mirrors the device's advertised param map so recall stays sane.
 * Default = the refdev's set. */
/* engine 2.1.0: contiguous ids 1..12 — the old id 7 phantom "FX Send" is gone and
 * the set renumbered with no hole (matches device/engine.c + the param-map-hash). */
#ifndef HARP_SHELL_PARAMS
#define HARP_SHELL_PARAMS                                                        \
    {1, "Osc Pitch", 0, 0.5, nullptr},     {2, "Osc Shape", 0, 0.5, nullptr},    \
    {3, "Filter Cutoff", 0, 0.5, nullptr}, {4, "Filter Reso", 0, 0.5, nullptr},  \
    {5, "Env Attack", 0, 0.5, nullptr},    {6, "Env Release", 0, 0.5, nullptr},  \
    {7, "Master Level", 0, 0.5, nullptr},                                        \
    {8, "Arp Mode", 4, 0.0, nullptr},      {9, "Arp Division", 5, 0.6, nullptr}, \
    {10, "Arp Gate", 0, 0.5, nullptr},     {11, "Arp Octaves", 3, 0.0, nullptr}, \
    {12, "Glide", 0, 0.0, nullptr}
#endif

/* HARP_SHELL_ENGINE_TABLES + HARP_SHELL_ENGINE_PARAM_ID (optional, for a multi-engine
 * product whose param NAMES change with the selected engine): ENGINE_TABLES = a comma-
 * separated list of per-engine rows (each row = number-of-params "Name" strings, "—"
 * marks a hidden slot); ENGINE_PARAM_ID = the Engine picker's param id. The controller
 * then re-titles the param slots to the selected engine's names on each Engine change
 * (restartComponent kParamTitlesChanged). Slot IDs/defaults never change (automation +
 * recall hold) — only the displayed names. Undefined (default) = one static name set
 * (the refdev). The rows should mirror the device's per-engine param map. */

/* HARP_SHELL_ENGINE_FILTER (optional): if a product defines it to a §12 engine-id
 * string, the shell auto-binds ONLY a network device reporting that engine (browse
 * _harp._tcp, hello each candidate, keep the match) — so a single-engine product
 * skips the other HARP devices on the bus without the user picking. Undefined
 * (default) = bind the first _harp._tcp device found.
 *
 * HARP_SHELL_ETHERNET_ONLY (optional): if defined, the shell never claims a USB
 * device (a network-only product). Undefined (default) = USB + network. */

#endif /* HARP_SHELL_CONFIG_H */
