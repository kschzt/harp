/* mpe_zone.h — classic MPE (member-channel MIDI) -> §9.4/§9.5 zone collapse.
 *
 * A shell that receives RAW 16-channel MIDI (Ableton Live on VST3, a CoreMIDI
 * AU input, the harnesses' raw MIDIEvent path) gets classic MPE: ONE instrument
 * spread across a block of member channels, each carrying one note plus that
 * note's per-note pitch bend, timbre (CC74), and channel pressure. The HARP
 * device takes the UMP channel nibble as the MULTITIMBRAL PART (§9.4, spec
 * §9.4 errata 0.3.7): if a member channel became the part, a single MPE chord
 * would scatter across 16 timbres. So a shell bridging MPE MUST collapse the
 * zone onto ONE part and carry the per-note dimension as §9.5 per-voice mods.
 *
 * THIS is that collapse, factored out of the shells like note_voice_map.h: the
 * shell keeps using its OWN instance part (part_, the recall-safe Part param id
 * 98) as the device channel for EVERY note, and asks this state machine to turn
 * each member-channel MIDI message into (a) the part to actually emit on (always
 * part_, NEVER the member chan) and (b) the §9.5 per-voice mod that carries the
 * expression. The device-side voice key it returns is the SAME packing the
 * device mints at note-on — (channel<<8)|note, channel == the instance part —
 * so a mod follows the note's voice exactly as the CLAP/VST3 note-expression
 * path already does (shell/note_voice_map.h, plugin.cpp:345, harp_clap.cpp:239).
 *
 * WHY a separate map from note_voice_map: a modern host addresses per-note
 * modulation by a host noteId (note_voice_map is noteId-keyed). Raw MIDI has NO
 * noteId — a member channel's later pitch-bend/CC/pressure is bound to the note
 * by the MEMBER CHANNEL it shares. So this tracks chan -> live voice key, the
 * raw-MIDI analogue of note_voice_map's noteId -> voice key.
 *
 * Mod-target ids + the device voice-key packing MUST match device.h
 * (HARP_MOD_PITCH_BEND 0x2001 / HARP_MOD_PRESSURE 0x2002) and shell_constants.h
 * / harp_clap.cpp (kCutoffId 3) — kept in sync by hand exactly like the §9.3
 * param ids, with static_asserts the includer can pin against those headers.
 *
 * Render-thread-only, like note_voice_map: the shell calls noteOn/pitchBend/cc/
 * channelPressure from process(); detection (enabled + auto-detect) is also
 * touched only there. No allocation, no locks — a small fixed-size POD.
 */
#ifndef HARP_SHELL_MPE_ZONE_H
#define HARP_SHELL_MPE_ZONE_H

#include <cstdint>

/* §9.5 per-voice expression mod-target ids — the MPE pitch/pressure axes. These
 * MUST equal device.h's HARP_MOD_PITCH_BEND / HARP_MOD_PRESSURE and the shells'
 * kPitchBendId / kPressureId (shell/clap/harp_clap.cpp:53-54). The timbre axis
 * (CC74) drives device param Filter Cutoff (id 3 = kCutoffId), as a SIGNED
 * offset around the neutral 0.5 — the SAME mapping the note-expression path uses
 * (plugin.cpp:361-365, harp_clap.cpp:278-279). Defined here so this header is
 * self-contained; the includer static_asserts them against the real headers. */
#define HARP_MPE_MOD_PITCH_BEND 0x2001u /* per-voice pitch, SEMITONES (signed) */
#define HARP_MPE_MOD_PRESSURE 0x2002u   /* per-voice loudness, gain (signed) */
#define HARP_MPE_MOD_TIMBRE 3u          /* CC74 -> Filter Cutoff, signed around 0.5 */

/* MPE 1.0: an MCM (RPN 6 on the master) defaults Master PB sensitivity to ±2
 * and Member PB sensitivity to ±48 semitones; either may then be changed by
 * RPN 0 on the respective channel. Defaults live here. */
#define HARP_MPE_DEFAULT_MASTER_PB_RANGE 2.0f
#define HARP_MPE_DEFAULT_MEMBER_PB_RANGE 48.0f

/* What a member-channel message resolves to. `voiceKey` is the §9.5 packed key
 * ((part<<8)|note) the device minted at note-on, or 0 = part-wide (no live note
 * on that channel / a master-channel message). `value` is the SIGNED mod offset
 * ready to hand straight to queueMod (semitones for pitch, gain for pressure,
 * cutoff-offset for timbre). `valid` is false when there is nothing to emit
 * (e.g. an RPN-sequence CC that only updated parse state, or a message on a
 * channel outside the active zone). The shell does:
 *     auto r = zone.pitchBend(chan, v14);
 *     if (r.valid) rt.queueMod(src, HARP_MPE_MOD_PITCH_BEND, r.value, r.voiceKey, ts); */
struct MpeMod {
    uint32_t voiceKey; /* §9.5 (part<<8)|note, or 0 = part-wide */
    float value;       /* signed offset for queueMod */
    bool valid;        /* false = nothing to emit (parse-only / out-of-zone) */
};

/* The note part to emit on. The shell ALWAYS sends notes on the instance part
 * (part_); this is returned for clarity and to centralize the "never the member
 * channel" rule. `accepted` is false for a note on a channel outside the active
 * zone (MPE on, but this channel is neither master nor a member) — the shell
 * may then drop it or route it normally; with MPE OFF every note is accepted
 * on its OWN channel unchanged — HARP derives a note's device part from its
 * MIDI channel, so the inactive zone is the byte-identical non-MPE path. */
struct MpeNote {
    uint8_t part;   /* the device channel to emit on — ALWAYS the instance part */
    bool accepted;  /* false = note outside the active zone (caller policy) */
};

/* shell/mpe_zone.h state machine: classic-MPE zone collapse onto one part.
 *
 * Lifecycle: the shell holds one MpeZone per instance, sets part_ via setPart()
 * whenever the Part param (id 98) changes, and toggles enabled_ from the MPE
 * param (mirrored Part-style) and/or MCM auto-detect. Every raw MIDI message
 * routes through the matching method. All state is render-thread-owned. */
struct MpeZone {
    /* ---- configuration (set off the audio path, read on it) ---- */
    bool enabled = false;     /* explicit MPE force-on (toggle; default OFF). When
                                 set it engages a zone even with no MCM (a default
                                 full lower zone — see setEnabled). */
    bool autoDetect = true;   /* engage automatically once a real MCM (RPN 6) is
                                 seen — what Logic / Ableton Live send when MPE is
                                 armed, so the common case needs no toggle. */
    bool mcmActive = false;   /* a real MCM (RPN 6, count>0) engaged a zone. Kept
                                 SEPARATE from `members` so the toggle's seeded
                                 geometry never masquerades as a detected zone:
                                 active() = enabled OR (autoDetect AND mcmActive),
                                 so turning the toggle off truly disables MPE when
                                 no MCM is live (else auto-detect would re-hold a
                                 toggle seed). */
    uint8_t part = 0;         /* the instance part (Part id 98); EVERY note + mod
                                 carries this as the device channel, NEVER the
                                 member channel — the §9.4 zone-collapse rule. */

    /* ---- zone geometry (MPE 1.0) ----
     * Lower zone: master = channel 0 (MIDI ch 1), members ascending from ch 1.
     * Upper zone: master = channel 15 (MIDI ch 16), members descending from 14.
     * `members` is the member-channel COUNT from the MCM; 0 = zone inactive.
     * We track ONE active zone per instance (the common case — a controller's
     * lower OR upper zone feeds one HARP part); lowerZone selects which. */
    bool lowerZone = true;    /* true = lower (master ch0), false = upper (ch15) */
    uint8_t members = 0;      /* member-channel count (MCM data); 0 = no zone yet */

    /* Pitch-bend ranges in SEMITONES (RPN 0). MPE seeds master ±2 / member ±48
     * at MCM; RPN 0 on a channel overrides. A member channel uses memberPbRange;
     * the master channel uses masterPbRange (its bend is a zone-wide shift). */
    float masterPbRange = HARP_MPE_DEFAULT_MASTER_PB_RANGE;
    float memberPbRange = HARP_MPE_DEFAULT_MEMBER_PB_RANGE;

    /* ---- per-member-channel live state ----
     * 16 channels; for each, the §9.5 voice key of its CURRENTLY sounding note
     * (0 = none) so a later bend/CC/pressure on that channel finds the voice
     * with NO host noteId (the raw-MIDI analogue of note_voice_map). MPE is one
     * note per member channel at a time; a new note-on on a busy channel
     * replaces the binding (the prior note's off, if any, already cleared it). */
    uint32_t chanVoice[16] = {};

    /* ---- per-channel RPN parse state ----
     * The RPN sequence is CC 101 (RPN MSB), CC 100 (RPN LSB), then CC 6 (data
     * entry MSB) [+ CC 38 LSB]. We latch the selected RPN per channel so a data
     * entry applies to the right parameter. rpnActive distinguishes "RPN 0
     * selected" from the power-on/NULL state. Only RPN 0 (pitch-bend range) and
     * RPN 6 (MCM) are acted on; any other selected RPN swallows its data entry
     * harmlessly. */
    struct RpnState {
        uint8_t msb = 0x7f; /* selected RPN MSB (0x7f = NULL / none selected) */
        uint8_t lsb = 0x7f; /* selected RPN LSB */
    };
    RpnState rpn[16] = {};

    /* =============================== config ============================== */

    /* Mirror the Part param (id 98) — the device channel every note + mod uses.
     * Re-parting mid-session is fine: live voice keys carry the OLD part until
     * their notes end, and new notes mint under the new part. */
    void setPart(uint8_t p) { part = p & 0xf; }

    /* The per-instance MPE toggle (a param mirrored Part-style by the shell).
     * OFF (default) makes every method a pass-through: notes go to the part,
     * member-channel expression is ignored — byte-identical to no MPE. */
    void setEnabled(bool on) {
        enabled = on;
        /* An explicit enable with no zone configured yet assumes a full LOWER
         * zone (master ch0, 15 members) so the toggle engages MPE even for a
         * host that streams raw MPE without ever sending an MCM; a later MCM
         * still reconfigures geometry. Auto-detect needs no seed (applyMcm sets
         * members). */
        if (on) { if (members == 0) { lowerZone = true; members = 15; } }
        else reset(); /* dropping out of MPE clears live bindings + parse state */
    }

    /* Is `chan` the master channel of the active zone? */
    bool isMaster(uint8_t chan) const {
        return lowerZone ? (chan == 0) : (chan == 15);
    }
    /* Is `chan` a MEMBER channel of the active zone (given `members`)? Lower:
     * ch 1..members. Upper: ch (14)..(15-members). Master is NOT a member. */
    bool isMember(uint8_t chan) const {
        if (members == 0) return false;
        if (lowerZone) return chan >= 1 && chan <= members;
        return chan <= 14 && chan >= (uint8_t)(15 - members);
    }
    /* Is the zone live? The explicit toggle forces it on (with a seeded default
     * zone, so `members` > 0 and isMember works); otherwise auto-detect engages
     * it once a real MCM is seen. Either path guarantees members > 0, so the
     * geometry helpers are always valid when active() is true. */
    bool active() const { return enabled || (autoDetect && mcmActive); }

    /* ================================ notes ============================== */

    /* A member-channel NOTE-ON. Records chan -> voiceKey=(part<<8)|note so a
     * later expression on `chan` finds this exact voice, and returns the part to
     * actually emit on — ALWAYS the instance part, NEVER `chan` (the §9.4 rule).
     * The voice key uses `part`, matching what the device mints at note-on
     * (engine.c:1159 (channel<<8)|note). A note OUTSIDE the active zone is
     * marked accepted=false so the shell can apply its own policy. With the zone
     * inactive every note is accepted on the part (the non-MPE pass-through). */
    MpeNote noteOn(uint8_t chan, uint8_t note) {
        chan &= 0x0f; /* a MIDI channel is a 4-bit nibble; keep chanVoice[] in range */
        if (!active()) return {chan, true}; /* pass-through on its OWN channel:
                                               HARP derives the device part from
                                               the note's MIDI channel; MPE off
                                               => byte-identical to no MPE */
        if (!isMember(chan) && !isMaster(chan)) return {part, false}; /* out of zone */
        if (isMember(chan)) chanVoice[chan] = ((uint32_t)part << 8) | (note & 0x7f);
        return {part, true};
    }

    /* A member-channel NOTE-OFF: clears the channel's voice binding so a stray
     * later expression on it falls back to part-wide (voice 0) instead of a dead
     * voice. (The note OFF itself still emits on the part, like noteOn — the
     * caller uses the returned part.) The binding is cleared only when THIS note
     * is the one bound to the channel: MPE is one note per member channel, but a
     * mismatched off (degenerate) must not strand the sounding voice by dropping
     * its expression route. */
    MpeNote noteOff(uint8_t chan, uint8_t note) {
        chan &= 0x0f;
        if (!active()) return {chan, true}; /* pass-through on its own channel */
        if (isMember(chan) && chanVoice[chan] == (((uint32_t)part << 8) | (note & 0x7f)))
            chanVoice[chan] = 0;
        if (!isMember(chan) && !isMaster(chan)) return {part, false};
        return {part, true};
    }

    /* ============================ expression ============================ */

    /* 14-bit pitch bend on `chan`. A MEMBER channel bends its ONE voice:
     * semis = (value14-8192)/8192 * memberPbRange, addressed to that voice's
     * §9.5 key. The MASTER channel bends the whole zone: semis via masterPbRange
     * with voiceKey 0 (part-wide), so it shifts every sounding voice on the part
     * — the device's voice==0 mod path (engine.c:1250). Returns valid=false off
     * the active zone or when a member channel has no live note.
     *
     * KNOWN LIMIT — master-channel (part-wide, voiceKey 0) messages are correct
     * only when the instance part is 0: the runtime derives a mod's part from
     * (voiceKey>>8)&0xf (runtime.cpp), so voiceKey 0 always encodes part 0. On a
     * non-zero instance part a zone-wide master bend/pressure would land on part
     * 0, not this part. The per-note (member-channel) path — the primary MPE
     * dimension — is correct for ANY part. (A fix would fan the master message
     * out to each live member voice; deferred until a non-zero-part MPE rig
     * needs it.) */
    MpeMod pitchBend(uint8_t chan, uint16_t value14) const {
        chan &= 0x0f;
        if (!active()) return {0, 0.0f, false};
        float norm = ((float)(int)value14 - 8192.0f) / 8192.0f; /* -1..+1 */
        if (isMaster(chan)) return {0, norm * masterPbRange, true}; /* part-wide */
        if (isMember(chan)) {
            uint32_t v = chanVoice[chan];
            if (!v) return {0, 0.0f, false}; /* no live note on this channel */
            return {v, norm * memberPbRange, true};
        }
        return {0, 0.0f, false};
    }

    /* Channel pressure (Dn) on `chan` -> per-voice loudness gain. MPE's Z axis.
     * gain = val/127 (0..1), the SAME normalization the CLAP PRESSURE path uses
     * (harp_clap.cpp). A member channel addresses its voice; the master channel
     * is part-wide (voice 0) — a zone-wide swell. */
    MpeMod channelPressure(uint8_t chan, uint8_t val) const {
        chan &= 0x0f;
        if (!active()) return {0, 0.0f, false};
        float gain = (float)(val & 0x7f) / 127.0f;
        if (isMaster(chan)) return {0, gain, true};
        if (isMember(chan)) {
            uint32_t v = chanVoice[chan];
            if (!v) return {0, 0.0f, false};
            return {v, gain, true};
        }
        return {0, 0.0f, false};
    }

    /* A control change on `chan`. Two jobs:
     *   1. CC74 (timbre, MPE's Y axis) -> a SIGNED Filter Cutoff offset around
     *      the neutral 0.5: offset = val/127 - 0.5, exactly the BRIGHTNESS /
     *      Note-Expression mapping (plugin.cpp:361, harp_clap.cpp:278). Member
     *      channel -> its voice; master -> part-wide.
     *   2. The RPN sequence — CC 101/100 select an RPN, CC 6 (+38) is the data:
     *        RPN 0 -> pitch-bend range (data MSB = semitones) for THIS channel's
     *                 role (master vs member range).
     *        RPN 6 -> MCM: data MSB = member-channel count; engages/resizes the
     *                 zone, picks lower/upper by the channel the MCM arrived on
     *                 (ch0 lower, ch15 upper), and auto-detect engages it.
     *      These return valid=false (they only update state). cc() returns an
     *      MpeMod so the timbre case can be queued; the RPN cases set valid=false.
     *
     * Non-const: the RPN sequence + MCM mutate parse/zone state. The shell calls
     * this for EVERY CC; with the zone inactive, CC74 is ignored (valid=false)
     * but RPN/MCM are still parsed so an incoming MCM can auto-engage. */
    MpeMod cc(uint8_t chan, uint8_t num, uint8_t val) {
        chan &= 0x0f; /* a MIDI channel is a 4-bit nibble; keep rpn[]/chanVoice[] in range */
        switch (num) {
        case 101: rpn[chan].msb = val & 0x7f; return {0, 0.0f, false}; /* RPN MSB */
        case 100: rpn[chan].lsb = val & 0x7f; return {0, 0.0f, false}; /* RPN LSB */
        case 38:  return {0, 0.0f, false}; /* data entry LSB: ignored (MSB suffices) */
        case 6: {                          /* data entry MSB: apply to selected RPN */
            RpnState r = rpn[chan];
            if (r.msb == 0) {
                if (r.lsb == 0) {          /* RPN 0: pitch-bend sensitivity */
                    float semis = (float)(val & 0x7f);
                    /* the master channel sets the zone-wide (master) range; any
                     * member sets the shared member range. isMaster depends only
                     * on the zone side, so this routes correctly even before an
                     * MCM has engaged the zone (lowerZone defaults to true). */
                    if (isMaster(chan)) masterPbRange = semis;
                    else memberPbRange = semis;
                } else if (r.lsb == 6) {   /* RPN 6: MCM — configure the zone */
                    applyMcm(chan, val & 0x7f);
                }
            }
            return {0, 0.0f, false};
        }
        case 74: { /* CC74 timbre (Y) -> signed Filter Cutoff offset */
            if (!active()) return {0, 0.0f, false};
            float offset = (float)(val & 0x7f) / 127.0f - 0.5f;
            if (isMaster(chan)) return {0, offset, true};
            if (isMember(chan)) {
                uint32_t v = chanVoice[chan];
                if (!v) return {0, 0.0f, false};
                return {v, offset, true};
            }
            return {0, 0.0f, false};
        }
        default: return {0, 0.0f, false};
        }
    }

    /* ============================ MCM / detect ========================== */

    /* Apply an MPE Configuration Message (RPN 6) seen on channel `chan` with a
     * member-channel count `count`. The channel picks the zone: ch0 = lower,
     * ch15 = upper (the only legal MCM master channels). count 0 DEACTIVATES the
     * zone. A fresh MCM re-seeds the §9.1 default PB ranges (±2 / ±48) per spec.
     * Auto-detect engaging is implicit: active() reads members>0. */
    void applyMcm(uint8_t chan, uint8_t count) {
        if (chan == 0) lowerZone = true;
        else if (chan == 15) lowerZone = false;
        else return; /* MCM only valid on a master channel (ch0 / ch15) */
        members = count > 15 ? 15 : count; /* MPE caps at 15 members (clamp, not wrap) */
        mcmActive = members > 0; /* count 0 DEACTIVATES the auto-detected zone */
        /* The explicit toggle is a force-on that a zone-teardown MCM must not
         * defeat: if MPE is forced on but this MCM dropped the zone (count 0),
         * keep a usable default full lower zone (else active() stays true with
         * members 0 and every member note would be rejected). */
        if (members == 0 && enabled) { lowerZone = true; members = 15; }
        /* an MCM resets PB sensitivities to the MPE defaults */
        masterPbRange = HARP_MPE_DEFAULT_MASTER_PB_RANGE;
        memberPbRange = HARP_MPE_DEFAULT_MEMBER_PB_RANGE;
        /* dropping/resizing the zone invalidates stale channel->voice bindings */
        for (auto &v : chanVoice) v = 0;
    }

    /* ============================== reset =============================== */

    /* Clear live bindings + RPN parse state (a session/transport reset, or MPE
     * toggled off). Geometry (zone, members, ranges) is configuration and
     * survives — re-engaging the toggle resumes the same zone an MCM set up. */
    void reset() {
        for (auto &v : chanVoice) v = 0;
        for (auto &r : rpn) r = RpnState{};
    }
};

#endif /* HARP_SHELL_MPE_ZONE_H */
