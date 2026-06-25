/* libFuzzer: the EVT-STREAM parser (§9.2 / §16/T9) — device/session.c::handle_evt_msg.
 *
 * handle_evt_msg consumes every event message the host streams over HARP_STREAM_EVT: the
 * §9.10 UMP carriage (note on/off, CC panic), §9.4 param-set/ramp/mod, the §9.7 transport
 * anchor, and the §9.6 txn begin/commit/abort. It is a wire-facing parser fed by ANY peer on
 * the §8.7 Ethernet binding, so per §16 it MUST survive arbitrary bytes fail-clean (drop +
 * count, never read/write OOB, never loop unboundedly). Until now NO fuzzer reached it —
 * fuzz_state covers the params-blob codec, fuzz_cbor/envelope the framing, but the evt-stream
 * grammar (the deepest host->device attack surface) was unfuzzed (HIGH #5; the old "T9 DONE"
 * in debt.md was false).
 *
 * handle_evt_msg is `static`, so — like a static codec — we reach it by TU-including
 * device/session.c (the REAL production code, not a copy). The fuzz path through handle_evt_msg
 * touches only the decoder, the in-struct txn buffer, the counters and evq_push/full; every
 * OTHER symbol session.c references (the audio thread, snapshot/closure walk, store I/O, the
 * front panel, reconcile) is satisfied by fuzz/evt_stubs.c with abort-stubs (never reached on
 * this path; a loud failure if a future change routes the fuzzer through them) — exactly the
 * fuzz_state.c discipline, scaled to session.c's larger surface.
 *
 * The harness builds a zero-initialized `device` (calloc: empty txn slots, epoch 0 = "now"
 * current, all counters 0) and calls handle_evt_msg on the raw input. ASan + libFuzzer are the
 * oracle: any OOB into the txn buffer, any read past the message, any unbounded decode trips
 * them. See device.h for the evt body contract.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* TU-include the real parser so the `static` handle_evt_msg is reachable. evt_stubs.c (a
 * separate TU) resolves the engine/session/store symbols session.c references — see CMake. */
#include "session.c"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* A heap device (it is large + carries the txn buffer) zero-initialized: empty txn slots,
     * audio.epoch 0 (= "now", always current so the stale-epoch arm is exercised by non-zero
     * input epochs), counters all 0. calloc gives the redzone-flanked allocation ASan needs. */
    device *d = (device *)calloc(1, sizeof *d);
    if (!d) return 0;
    handle_evt_msg(d, data, size);
    free(d);
    return 0;
}
