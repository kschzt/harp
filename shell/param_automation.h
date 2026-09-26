/* param_automation.h — DAW parameter automation -> HARP §9.4 device events.
 *
 * Shared by the VST3 instrument shell (plugin.cpp) and the §8.8 FX shell
 * (fx_plugin.cpp) so both turn a host's automation into the same wire:
 *
 *   - consecutive points on one param become §9.4 RAMPS (a DAW curve as a handful
 *     of ramps, §9.1); a point more than 100 ms after the previous one is a jump
 *     (a timestamped SET);
 *   - points closer than 256 samples are THINNED: folded into the next ramp's
 *     target, and a fold with no successor for a full pacing block flushes as a
 *     "now" set (see beginBlock);
 *   - §15.5 offline editing: the current value of every param is tracked whether or
 *     not a device is connected, and params edited while it was ABSENT are replayed
 *     on the reconnect edge — "a mismatch resolved by Push" (§11.4). The FIRST
 *     connect replays nothing: the recall bundle + the live flow carry the initial
 *     state (a blind replay-all-on-connect clobbered recalled state with defaults).
 *
 * Moved verbatim out of plugin.cpp's process(); the instrument shell's wire is
 * byte-identical (the golden/recall/offline-edit gates cover it). Audio-thread
 * only: init() sizes the state once (off the RT path), nothing allocates after.
 */
#ifndef HARP_PARAM_AUTOMATION_H
#define HARP_PARAM_AUTOMATION_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime.h"

class ParamAutomation {
public:
    static constexpr uint64_t kThinSamples = 256;  /* min spacing between emitted points */
    static constexpr uint64_t kJumpSamples = 4800; /* >100 ms since the last point = a jump */

    /* ids[i] / defaults[i]: device param id + default at slot i (the shell's param
     * table order). Defaults seed the §15.5 current values, so an unedited param
     * re-asserts its true value if it is ever replayed (idempotent). */
    void init(const uint32_t *ids, const float *defaults, size_t n) {
        ids_.assign(ids, ids + n);
        curVal_.assign(defaults, defaults + n);
        lastTs_.assign(n, 0);
        pendTs_.assign(n, 0);
        pendVal_.assign(n, 0.0f);
        hasLast_.assign(n, 0);
        pendHas_.assign(n, 0);
        dirtyOffline_.assign(n, 0);
    }

    /* slot of a device param id, or SIZE_MAX if the id is not in the table */
    size_t slotOf(uint32_t id) const {
        for (size_t i = 0; i < ids_.size(); i++)
            if (ids_[i] == id) return i;
        return SIZE_MAX;
    }

    /* Once per process() block, BEFORE its points: the §15.5 reconnect replay,
     * then flush any folded point whose gesture is over. */
    void beginBlock(HarpRuntime &rt, EventSource *src, uint64_t base) {
        connected_ = rt.connected();
        if (connected_ && !wasConnected_) {
            if (everConnected_)
                for (size_t i = 0; i < ids_.size(); i++)
                    if (dirtyOffline_[i]) {
                        rt.queueParamSet(src, ids_[i], curVal_[i], 0);
                        dirtyOffline_[i] = 0;
                    }
            everConnected_ = true;
        }
        wasConnected_ = connected_;
        /* Flush a pending fold only when the gesture is OVER — no successor point for
         * a full pacing block. Flushing one DAW block after the fold emitted 64-sample
         * ramps whose END was already at "now": ~1100/s of guaranteed-stale
         * timestamps at 64-sample buffers (measured; invisible at >= 256 where folding
         * never triggers). The pend holds a gesture's final settling value; a "now" set
         * delivers it without inventing a timestamp the stream already passed. */
        for (size_t i = 0; i < ids_.size(); i++)
            if (pendHas_[i] && base >= pendTs_[i] + kThinSamples) {
                rt.queueParamSet(src, ids_[i], pendVal_[i], 0);
                lastTs_[i] = pendTs_[i];
                pendHas_[i] = 0;
            }
    }

    /* One automation point for table slot `i` at stream position `ts`. */
    void point(HarpRuntime &rt, EventSource *src, size_t i, float v, uint64_t ts) {
        curVal_[i] = v;                                     /* §15.5: track current value */
        if (everConnected_ && !connected_) dirtyOffline_[i] = 1; /* replay on reconnect */
        if (hasLast_[i] && ts > lastTs_[i] && ts - lastTs_[i] < kThinSamples) {
            pendHas_[i] = 1; /* too soon: fold into the next ramp */
            pendTs_[i] = ts;
            pendVal_[i] = v;
            return;
        }
        bool ramp = hasLast_[i] && ts > lastTs_[i] && ts - lastTs_[i] <= kJumpSamples;
        if (ramp)
            rt.queueRamp(src, ids_[i], v, lastTs_[i], ts);
        else
            rt.queueParamSet(src, ids_[i], v, ts);
        lastTs_[i] = ts;
        hasLast_[i] = 1;
        pendHas_[i] = 0;
    }

private:
    std::vector<uint32_t> ids_;
    std::vector<float> curVal_;   /* §15.5 current value per slot */
    std::vector<uint64_t> lastTs_, pendTs_;
    std::vector<float> pendVal_;
    std::vector<uint8_t> hasLast_, pendHas_;
    std::vector<uint8_t> dirtyOffline_; /* edited during an offline gap -> replayed on reconnect */
    bool connected_ = false;      /* rt.connected() sampled at this block's start */
    bool wasConnected_ = false;
    bool everConnected_ = false;  /* replay only on a TRUE reconnect, never the first connect */
};

#endif /* HARP_PARAM_AUTOMATION_H */
