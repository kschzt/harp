/* shell/runtime_log.h — the shell-internal stderr logging helpers, shared
 * across the HarpRuntime translation units.
 *
 * log_msg / log_param_map_drift were file-local statics in runtime.cpp. When the
 * §11.4 recall/reconcile + §15.3 state-bundle logic split out into
 * runtime_recall.cpp (a pure TU move), BOTH files needed them (log_msg: 45 call
 * sites stay in runtime.cpp + 17 move to the recall TU; log_param_map_drift: the
 * connect-apply path stays, setStateBundle moves). They live here VERBATIM as
 * `static inline` so each TU keeps its own copy — no linkage change, no new global
 * symbol, byte-identical stderr output.
 *
 * NOT on any golden path: these write to stderr only. The diag-bundle's runtime-log
 * ring (key 8) is fed by recordLog(), a HarpRuntime method, not by these helpers.
 */
#pragma once

#include <cstdarg>
#include <cstdio>

static inline void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "harp-shell: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* §9.3/§13.4 param-map drift warning — one message, emitted whether the project
 * state arrived while connected or a bundle staged offline applied on connect. */
static inline void log_param_map_drift() {
    log_msg("recall: project's param map differs from the device's (engine update?) "
            "— applying matching ids only");
}
