/* fx_late_guard.h — the §8.8 live-FX late-guard POLICY as a pure function, so it
 * unit-tests off the audio path (tests/runtime_units_tests.cpp), like note_voice_map.h.
 *
 * The runtime holds an armed effect's wet exactly fxLatencySamples() behind its input and
 * pays back a short read by dropping the late wet when it lands (pad debt). A transient
 * hiccup clears that debt within a block or two. A wet that is PERSISTENTLY later than the
 * budget never clears it, and paying it would drop nearly every block — a silent insert —
 * so once late wet has been owed without a break for `window` frames, the runtime
 * re-anchors (forgives the debt). The signal is the outstanding debt, not short reads:
 * under jitter, short blocks interleave with blocks eaten paying the debt.
 *
 * fxLateStep() advances `run` (frames pulled with late wet owed) by one pull of `nFrames`
 * and returns true exactly when the runtime must re-anchor now; `run` restarts at 0 then,
 * and whenever nothing is owed or the stream is disconnected. */
#pragma once

#include <cstddef>
#include <cstdint>

static inline bool fxLateStep(uint64_t &run, bool owed, bool connected, size_t nFrames,
                              uint64_t window) {
    if (!owed || !connected) {
        run = 0;
        return false;
    }
    run += nFrames;
    if (run < window) return false;
    run = 0;
    return true;
}
