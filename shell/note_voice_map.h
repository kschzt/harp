/* note_voice_map.h — host noteId -> §9.5 packed voice key.
 *
 * A modern host addresses per-note modulation by a noteId (VST3 Note Expression,
 * CLAP per-note PARAM_MOD); the HARP device addresses a voice by the §9.5 packed
 * key it mints at note-on ((channel<<8)|note for MIDI-1.0 note-ons). Every shell
 * that forwards per-note modulation needs the same small bookkeeping to bridge
 * the two, so it lives here once instead of being copied per shell.
 *
 * Sparse fixed table (notes are few per instance); a note the host gave NO id
 * (noteId < 0) is not tracked, so its modulation falls back to voice 0 =
 * part-wide. Render-thread-only, like the rest of a shell's per-block state.
 */
#ifndef HARP_SHELL_NOTE_VOICE_MAP_H
#define HARP_SHELL_NOTE_VOICE_MAP_H

#include <cstdint>

struct NoteVoiceMap {
    struct Entry {
        int32_t noteId;
        uint32_t voiceKey;
        bool active;
    };
    Entry entries[64] = {};

    /* remember the §9.5 voice key a note-on minted, keyed by its host noteId */
    void noteOn(int32_t noteId, uint32_t voiceKey) {
        if (noteId < 0) return; /* unaddressable without an id */
        for (auto &e : entries) /* take a free slot, or refresh the same id */
            if (!e.active || e.noteId == noteId) { e = {noteId, voiceKey, true}; return; }
    }
    void noteOff(uint32_t voiceKey) {
        for (auto &e : entries)
            if (e.active && e.voiceKey == voiceKey) e.active = false;
    }
    /* the voice key for a live noteId, or 0 (part-wide) if we never saw it */
    uint32_t voiceFor(int32_t noteId) const {
        if (noteId < 0) return 0;
        for (auto &e : entries)
            if (e.active && e.noteId == noteId) return e.voiceKey;
        return 0;
    }
    void reset() {
        for (auto &e : entries) e.active = false;
    }
};

#endif /* HARP_SHELL_NOTE_VOICE_MAP_H */
