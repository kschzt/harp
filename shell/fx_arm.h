/* fx_arm.h — how the §8.8 runtime ARMS an effect session's wet, as a pure function so it
 * unit-tests off the audio path (tests/runtime_units_tests.cpp), like fx_late_guard.h.
 *
 * Two different delays sit between an effect's input and its wet, and they are handled
 * differently:
 *   - the runtime's own TIMING budget (`base`: fxOfflineLatency/fxLiveLatency — the
 *     automation horizon, the kBlock framing and, live, the ring target). The wet of input
 *     SSI s ARRIVES up to `base` later, so the runtime pre-rolls `base` frames of silence.
 *   - the device's CONTENT pipeline (`device-pipeline-samples`, the audio.start response key
 *     1 — §8.8: an effect MUST report its constant engine contribution there). The device
 *     answers every pacing frame on time, but with wet that trails its input by `pipe`
 *     samples: it shifts WHAT arrives, not when, so it is never pre-rolled — it adds to the
 *     delivered position.
 * delivered = preroll + pipe. The host is told `reported` = base + the pipeline LATCHED at
 * activation (0 if no device was connected then), constant for the activation. The
 * runtime arms the pre-roll so that delivered == reported whenever the session's pipeline
 * is no deeper than the latched one (a shallower one is padded up to it), and otherwise
 * delivers at the smallest position the timing allows — later than reported by `lag`, which
 * the runtime warns about (only re-activation can report it; the dry follows the delivered
 * position either way, so dry and wet stay aligned). `maxDelay` is what a shell's dry line
 * holds: past it the pre-roll is cut to fit (`capped`: the wet then arrives late, and a pipe
 * alone beyond it cannot be aligned at all). */
#pragma once

#include <cstdint>

struct FxArm {
    uint32_t preroll;   /* silence frames before the session's first wet (the armed delay) */
    uint32_t delivered; /* the wet's position behind its input: preroll + pipe */
    uint32_t lag;       /* delivered - reported when positive (the host's PDC lags by this) */
    bool capped;        /* maxDelay clipped it: dry and wet cannot be aligned */
};

static inline FxArm fxArmFor(uint32_t base, uint32_t latchedPipe, uint32_t pipe, uint32_t maxDelay) {
    FxArm a{};
    uint64_t reported = (uint64_t)base + latchedPipe;
    uint64_t preroll = reported > pipe ? reported - pipe : 0;
    if (preroll < base) preroll = base; /* the timing budget is never cut */
    uint64_t delivered = preroll + pipe;
    if (delivered > maxDelay) { /* the timing budget is cut; a pipe beyond maxDelay stays */
        a.capped = true;
        preroll = pipe < maxDelay ? maxDelay - pipe : 0;
        delivered = preroll + pipe;
    }
    a.preroll = (uint32_t)preroll;
    a.delivered = (uint32_t)delivered;
    a.lag = delivered > reported ? (uint32_t)(delivered - reported) : 0;
    return a;
}
