/* Host-side shell constants SHARED by both shell adapters (shell/plugin.cpp VST3,
 * shell/au/harp_au.mm AU) so they cannot silently diverge — the two formats MUST
 * agree on the "Part" parameter id and on the recall component-state header for a
 * project to move between VST3 and AU (cross-format-recall-test.sh). Plain
 * file-scope constants (internal linkage per TU, no ODR concern); the point is one
 * source of truth for the VALUES, not shared storage. */
#pragma once

#include <cstddef>
#include <cstdint>

/* "Part" parameter: per-instance multitimbral routing (which device part 0..15
 * this shell instance drives). HOST-SIDE ROUTING ONLY — both shells special-case
 * it out of the device param-set path, so it never enters the wire or
 * param-map-hash. id 98 sits just below the VST3 Panic param (99). */
static constexpr uint32_t kPartParamId = 98;
static constexpr int kPartStepCount = 15; /* 16 parts (0..15); VST3 stepCount = N-1 */

/* Recall component-state header, prepended to the recall bundle by getState and
 * stripped by setState: the 3-byte magic 'HP1' + 1 part byte. 'H' (0x48) can never
 * be a recall bundle's first byte (a CBOR map, 0xA6), so an old header-less state
 * is detected and migrated to Part 0. MUST be byte-identical across both shells. */
static const uint8_t kStateHeaderMagic[3] = {'H', 'P', '1'};
static constexpr size_t kStateHeaderLen = sizeof kStateHeaderMagic + 1; /* magic + part byte */

/* ---------------- §9.9 OUTPUT METERS (readonly host params) ----------------
 *
 * The device exposes per-part + main-mix peak/RMS meters as READONLY parameters
 * (§9.9 "output parameters"): the render thread folds them and a control-thread
 * pump echoes them via the SAME evt 'param' path the front-panel echo uses, so
 * they arrive in the shell's echo ring exactly like a panel knob move (id +
 * value). The shells register these ids as READONLY host parameters — VST3
 * kIsReadOnly (NOT automatable), AU read-only AudioUnitParameter — so a DAW shows
 * live meters but MUST NOT offer or record them as automation (§9.9).
 *
 * These mirror the device-side meter id scheme (device/device.h: METER_ID_BASE /
 * METER_ID_PEAK / METER_ID_RMS / METER_NSLOTS) the SAME way kParams mirrors the
 * device's 1..13 param set — host-side, so the shells stay self-contained and
 * never include the device-private header. The two MUST agree (the id is what the
 * echo carries); guarded by static_asserts in each shell against the values here.
 *
 * The id SELF-ENCODES the slot + metric, so the existing pollEcho/popEcho path
 * needs no change: id = kMeterIdBase + slot*2 + {0 peak, 1 rms}; slot 0..15 = the
 * multitimbral parts, slot 16 = the summed main mix. Ids start at 0x1000, far
 * above the 1..13 device params and the 98/99 host params, so they cannot collide.
 *
 * Additive + readonly: meters are NEVER on the render/automation/event path, so a
 * single instance's golden render is byte-identical (the determinism gate). */
static constexpr uint32_t kMeterIdBase = 0x1000u;
static constexpr uint32_t kMeterParts = 16;           /* parts 0..15 (device NPARTS) */
static constexpr uint32_t kMeterSlots = kMeterParts + 1; /* + the main mix (slot 16) */
static constexpr uint32_t kMeterMainSlot = kMeterParts;  /* slot index of the main mix */
static constexpr uint32_t kNumMeterParams = kMeterSlots * 2; /* peak + rms per slot */
static constexpr uint32_t kMeterRateHz = 30u; /* descriptor meter-rate hint (§9.3 k11) */

static constexpr uint32_t meterPeakId(uint32_t slot) { return kMeterIdBase + slot * 2u + 0u; }
static constexpr uint32_t meterRmsId(uint32_t slot) { return kMeterIdBase + slot * 2u + 1u; }
/* Is `id` one of the readonly meter ids? (the whole contiguous 0x1000 range) */
static constexpr bool isMeterId(uint32_t id) {
    return id >= kMeterIdBase && id < kMeterIdBase + kNumMeterParams;
}
