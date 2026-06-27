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

/* The part byte is 0..15 (4 bits); its high bit 7 persists the VST3 MPE-enable
 * toggle (raw-MIDI MPE engage — see below). Packed here, NOT as a new header byte,
 * so the header length + magic are unchanged and the cross-format-recall oracle
 * stays byte-identical: an MPE-off instance writes part byte = part (bit 7 clear),
 * exactly as before; old 'HP1' projects (bit 7 clear) migrate to MPE-off; and the
 * AU reads the part with `& 0xf`, harmlessly ignoring the bit. */
static constexpr uint8_t kStatePartMask = 0x0fu;
static constexpr uint8_t kStateMpeBit = 0x80u;

/* §9.5 raw-MIDI MPE (VST3): per-channel pitch-bend / CC74-timbre / channel-
 * pressure arrive in VST3 only as IMidiMapping-routed PARAM changes (the host
 * turns the member-channel MIDI into them). The controller maps each
 * (channel, axis) to one hidden param id here; the processor decodes the id back
 * to (channel, axis) and feeds the value through shell/mpe_zone.h. Reserved at
 * 0x3000+, stride 4 per channel (axes 0 pitch / 1 timbre / 2 pressure; 3 spare),
 * clear of the 1..13 device params, the 97/98/99 host params, and the 0x1000
 * meter ids — and special-cased out of the device param-set path like Part. The
 * "MPE" toggle (id 97) engages the zone: classic-MPE auto-detect (MCM/RPN) does
 * not survive VST3's per-param automation model, so VST3 uses an explicit toggle
 * (the AU still auto-detects from ordered raw MIDI). */
static constexpr uint32_t kMpeEnableParamId = 97;
static constexpr uint32_t kMpeMidiBase = 0x3000u;
static constexpr uint32_t kMpeMidiAxes = 4u; /* stride per channel (3 used + 1 spare) */
static constexpr uint32_t kMpeMidiCount = 16u * kMpeMidiAxes; /* 16 channels */
enum { kMpeAxisBend = 0, kMpeAxisTimbre = 1, kMpeAxisPressure = 2 };
static constexpr uint32_t mpeMidiId(uint32_t chan, uint32_t axis) {
    return kMpeMidiBase + (chan & 0xf) * kMpeMidiAxes + axis;
}
static constexpr bool isMpeMidiId(uint32_t id) {
    return id >= kMpeMidiBase && id < kMpeMidiBase + kMpeMidiCount;
}

/* MULTI-OUT M2 — PER-CHANNEL DEVICE PARAMS. A satellite track's MIDI CC on channel N controls
 * part N's device param P (1..12). The host maps (channel N, GP CC kPerChanCcBase+i) -> a
 * synthetic param id here (via IMidiMapping), the processor decodes it back to (N, P) and queues
 * a param-set with §9.4 key 5 = N — one main instance drives every part's params. Stride 16 (>=
 * the 12 device params) keeps the decode pure bit math and clear of the 1..13 params, the 0x2000
 * mod ids, the 0x3000 MPE ids, and the 97/98/99 host params. */
static constexpr uint32_t kPerChanParamBase = 0x4000u;
static constexpr uint32_t kPerChanStride = 16u;                 /* id slots per channel (12 used) */
static constexpr uint32_t kPerChanCount = 16u * kPerChanStride; /* 16 channels */
static constexpr uint32_t kPerChanCcBase = 102u;               /* GP CC 102+i -> device param i+1 */
static constexpr uint32_t perChanParamId(uint32_t chan, uint32_t param /*1-based*/) {
    return kPerChanParamBase + (chan & 0xf) * kPerChanStride + ((param - 1u) & 0xf);
}
static constexpr bool isPerChanParamId(uint32_t id) {
    return id >= kPerChanParamBase && id < kPerChanParamBase + kPerChanCount;
}
static constexpr uint8_t perChanParamChannel(uint32_t id) {
    return (uint8_t)(((id - kPerChanParamBase) / kPerChanStride) & 0xf);
}
static constexpr uint32_t perChanParamDevId(uint32_t id) { /* device param 1..12 */
    return ((id - kPerChanParamBase) & (kPerChanStride - 1u)) + 1u;
}

/* §9.5 per-voice EXPRESSION mod targets (the MPE pitch/pressure axes). A shell
 * sends these via queueMod to drive a voice's pitch bend (SEMITONES) and
 * loudness (gain); they are NOT §9.3 params — they route to the device's
 * per-voice bend_semis / z_gain fields. The values MUST match device.h
 * HARP_MOD_PITCH_BEND / HARP_MOD_PRESSURE (the id is what the wire carries),
 * mirrored host-side exactly like kParams mirrors the 1..13 device params.
 * Reserved at 0x2000+, clear of the params, the 98/99 host params, and the
 * 0x1000 meter ids. */
static constexpr uint32_t kHarpModPitchBend = 0x2001u;
static constexpr uint32_t kHarpModPressure = 0x2002u;

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
