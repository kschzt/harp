/* libFuzzer: the multitimbral params-blob parser (§10 / P3 closer) —
 * refdev_parse_params_blob. This blob is the inner payload of every
 * x.harp-refdev.params object the device loads on recall/restore, so it is
 * a §16/T9 wire-facing parser: it MUST survive arbitrary bytes fail-clean.
 *
 * The harness just calls it on the raw input and checks nothing itself —
 * ASan + libFuzzer are the oracle: any OOB write into the v[]/present[]
 * grid, any read past the payload, or an unbounded loop trips them. The
 * grids are on the stack so a stray write past [NPARTS][NPARAMS] lands in
 * a redzone (ASan stack-buffer-overflow) rather than going unnoticed.
 *
 * refdev_parse_params_blob lives in device/state.c; it is documented as a
 * PURE codec (no store I/O, no engine touch) whose only external datum is
 * the g_params id table. We link device/state.c directly and satisfy the
 * non-codec engine/session symbols state.c references with abort-stubs
 * (fuzz/state_stubs.c) — the fuzzer never reaches those, so the codec runs
 * in genuine isolation. See device.h for the format contract. */
#include <stddef.h>
#include <stdint.h>

#include "device.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    float v[NPARTS][NPARAMS];
    bool present[NPARTS][NPARAMS];
    refdev_parse_params_blob(data, size, v, present);
    return 0;
}
