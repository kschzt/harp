/* shell/runtime_events.cpp — the §9 event plane, now a REAL owned-state type.
 *
 * Prior PRs (#140/#144) moved the event encode/queue/drain methods into this TU,
 * but they were still HarpRuntime:: methods over the shared god-object state. This
 * file now implements EventManager (declared in runtime.h) — a genuine component
 * that OWNS the SPSC event source ring, the §8.3.1 fence sequence, the drop
 * counter, the escalated-panic latch, and the §9.7 transport detector. HarpRuntime
 * holds ONE instance (events_) and DELEGATES its public event API to it, applying
 * the §12.2/§11.4 read-only WRITE HOLD in the thin wrappers here BEFORE the plane
 * (so the plane itself carries no reconcile/session policy).
 *
 * The event WIRE BYTES are byte-identical to before: the encoders are verbatim,
 * queue* pushes onto the same ring with the same fence fetch_add + memory orders,
 * and the read-only drop path is unchanged (it just lives in the wrapper now).
 *
 * NOT owned here (stays HarpRuntime / runtime_audio.cpp): the RT threads. The
 * eventPump thread that CALLS drainOwner + takePanic, and the feeder that reads
 * fenceStamp() + calls pollDropLog(), still live on the audio side — this plane is
 * the ENCODE/QUEUE/DRAIN + fence STATE they operate through, not the scheduling.
 */
#include "runtime.h"
#include "runtime_log.h" /* log_msg (EventManager::pollDropLog) */

#include <cstring> /* memcpy: bit-cast ppq <-> u64 (queueTransport / drainOwner) */

/* ============================ EventManager ============================ */

/* Param set as a §9.4 event message: fire-and-forget, no response.
 * ts is an SSI (0 = "now"). Encode-only; the feeder frames and batches. */
void EventManager::encodeParamEvent(harp_cbuf *m, uint32_t id, float v, uint64_t ts,
                                    uint8_t channel) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 1); /* etype: param */
    harp_cbor_map(m, channel ? 3 : 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, v);
    if (channel) { /* key 5 = multitimbral part (§9.4); omitted for part 0 */
        harp_cbor_uint(m, 5);
        harp_cbor_uint(m, channel);
    }
}

/* Each queue* pushes to the owner source ring (its SPSC producer side) and bumps
 * the per-session fence (evtQueuedSeq_): the device must consume that many events
 * before rendering a fenced range. A null source (unacquired instance) is a
 * defensive no-op — the shells only queue once they hold the owner source. The
 * §12.2/§11.4 read-only WRITE HOLD is applied by HarpRuntime's wrapper BEFORE
 * this point (so this plane carries no reconcile policy). */
void EventManager::queueParamSet(EventSource *src, uint32_t id, float v, uint64_t ts,
                                 uint8_t channel) {
    if (!src) return;
    /* M2 per-event part: an explicit channel (a satellite's MIDI channel N) targets part N;
     * the default resolves to the source's own channel (byte-identical for every prior caller). */
    uint8_t ch = (channel == kChanFromSource)
                     ? (uint8_t)(src->chan.load(std::memory_order_relaxed) & 0xf)
                     : (uint8_t)(channel & 0xf);
    if (src->ring.push({0, id, v, ts, 0, ch}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void EventManager::queueRamp(EventSource *src, uint32_t id, float target, uint64_t start,
                             uint64_t end, uint8_t channel) {
    if (!src) return;
    uint8_t ch = (channel == kChanFromSource)
                     ? (uint8_t)(src->chan.load(std::memory_order_relaxed) & 0xf)
                     : (uint8_t)(channel & 0xf);
    if (src->ring.push({1, id, target, start, end, ch}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}
void EventManager::queueNote(EventSource *src, uint32_t word, uint64_t ts) {
    if (!src) return;
    if (src->ring.push({2, word, 0.0f, ts, 0})) {
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    } else {
        evDrops_.fetch_add(1, std::memory_order_relaxed);
        /* A dropped note-ON is a missing note; a dropped note-OFF is a
         * note that never stops. If anything but a note-on is lost,
         * escalate to all-notes-off — silence beats a stuck drone. */
        uint32_t status = (word >> 20) & 0xf, vel = word & 0x7f;
        bool isNoteOn = status == 0x9 && vel > 0;
        if (!isNoteOn) panicPending_.store(true, std::memory_order_release);
    }
}

void EventManager::queueMod(EventSource *src, uint32_t id, float offset,
                            uint32_t voice, uint64_t ts) {
    if (!src) return;
    /* kind 4 = mod; the §9.5 voice key rides in `end` (it is a packed uint, not
     * a timestamp). A dropped mod is benign — it leaves the base value as-is, no
     * stuck state — so unlike a note we do not escalate to panic on overflow.
     * M2: carry the source's channel as the part FALLBACK — a per-voice mod still
     * derives its part from the voice key (encodeModEvent), a part-wide mod (voice 0)
     * uses this channel, byte-identical to the prior src.chan path. */
    uint8_t ch = (uint8_t)(src->chan.load(std::memory_order_relaxed) & 0xf);
    if (src->ring.push({4, id, offset, ts, voice, ch}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}

void EventManager::queueTransport(EventSource *src, uint32_t flags, double tempo,
                                  double ppq, uint64_t ts) {
    /* Transport is GLOBAL (no part): force it onto the OWNER source whatever
     * `src` is, so a multitimbral group emits ONE transport stream — the
     * owner's is canonical — instead of N identical copies racing on the wire.
     * (feedTransport's change-detection already runs only on the owner.) */
    (void)src;
    uint64_t ppqBits;
    memcpy(&ppqBits, &ppq, sizeof ppqBits);
    if (ownerSource_.ring.push({3, flags, (float)tempo, ts, ppqBits}))
        evtQueuedSeq_.fetch_add(1, std::memory_order_release);
    else
        evDrops_.fetch_add(1, std::memory_order_relaxed);
}

/* (The attached-source registry — registerSource/unregisterSource — is retired:
 * one private runtime per instance, so the only event source is ownerSource_.) */

void EventManager::feedTransport(bool playing, bool tempoValid, double tempo,
                                 bool posValid, double ppq, uint32_t blockSamples,
                                 uint64_t base, uint32_t rate) {
    bool discont = false;
    if (playing && tpLastPlaying_ && posValid)
        /* half a MIDI tick of slack: anything bigger is a jump */
        discont = ppq + 1e-3 < tpLastEndPpq_ || ppq > tpLastEndPpq_ + 1e-3;
    tpSamplesSince_ += blockSamples;
    bool change =
        playing != tpLastPlaying_ || (tempoValid && tempo != tpLastTempo_) || discont;
    bool refresh = playing && tpSamplesSince_ >= rate;
    if (change || refresh || !tpSent_) {
        uint32_t flags =
            (playing ? 1u : 0) | (tempoValid ? 1u << 3 : 0) | (posValid ? 1u << 5 : 0);
        /* feedTransport runs only on the OWNER (transport-change detection
         * state is owner-audio-thread-owned); transport is global, so push it
         * on the owner source — queueTransport pins it there regardless. */
        queueTransport(&ownerSource_, flags, tempo, ppq, base);
        tpSent_ = true;
        tpSamplesSince_ = 0;
    }
    tpLastPlaying_ = playing;
    tpLastTempo_ = tempo;
    tpLastEndPpq_ = playing && tempoValid && posValid
                        ? ppq + blockSamples * tempo / (60.0 * rate)
                        : ppq;
}

/* transport event (§9.7): etype 7, body {0 flags, 1 tempo, 4 ppq} */
void EventManager::encodeTransportEvent(harp_cbuf *m, uint32_t flags, double tempo,
                                        double ppq, uint64_t ts) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 7); /* etype: transport */
    harp_cbor_map(m, 3);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, flags);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, tempo);
    harp_cbor_uint(m, 4);
    harp_cbor_float(m, ppq);
}

/* UMP event (§9.10): etype 0, body = one packet, words big-endian. */
void EventManager::encodeUmpEvent(harp_cbuf *m, uint32_t word, uint64_t ts) {
    uint8_t bytes[4] = {(uint8_t)(word >> 24), (uint8_t)(word >> 16),
                        (uint8_t)(word >> 8), (uint8_t)word};
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 0); /* etype: ump */
    harp_cbor_bytes(m, bytes, 4);
}

/* Mod event (§9.4 non-destructive modulation): etype 6, body
 * {0 param, 1 signed offset, 3 voice (§9.5 packed key), 5 channel/part}. The
 * offset is NOT clamped here — it is signed and clamped only after summing onto
 * the base, on the device (§9.4). The part (key 5) for a PER-VOICE mod is the
 * channel embedded in the voice key ((voice>>8)&0xf), so the mod lands on the
 * SAME part the note's voice_id was minted under; for a PART-WIDE mod (voice 0)
 * it is `srcChan` — the part the emitting source drives — so a zone-wide MPE
 * master bend/pressure reaches THIS instance's part, not always part 0. Key 5 is
 * omitted when the part is 0 (part-0 byte-economy; the device defaults absent to
 * part 0), so a part-0 source is byte-identical to before. */
void EventManager::encodeModEvent(harp_cbuf *m, uint32_t id, float offset,
                                  uint64_t ts, uint32_t voice, uint8_t srcChan) {
    uint8_t channel = voice ? (uint8_t)((voice >> 8) & 0xf) : (uint8_t)(srcChan & 0xf);
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, ts);
    harp_cbor_uint(m, 6); /* etype: mod */
    int n = 2 + (voice ? 1 : 0) + (channel ? 1 : 0);
    harp_cbor_map(m, (uint64_t)n);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, offset);
    if (voice) { /* key 3 = §9.5 per-voice target; 0/absent = part-wide */
        harp_cbor_uint(m, 3);
        harp_cbor_uint(m, voice);
    }
    if (channel) { /* key 5 = multitimbral part; omitted for part 0 */
        harp_cbor_uint(m, 5);
        harp_cbor_uint(m, channel);
    }
}

/* Ramp event (§9.4): etype 5, msg tstamp = start, body {param, target, end}. */
void EventManager::encodeRampEvent(harp_cbuf *m, uint32_t id, float target,
                                   uint64_t start, uint64_t end, uint8_t channel) {
    harp_cbor_array(m, 3);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, start);
    harp_cbor_uint(m, 5); /* etype: ramp */
    harp_cbor_map(m, channel ? 4 : 3);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, id);
    harp_cbor_uint(m, 1);
    harp_cbor_float(m, target);
    harp_cbor_uint(m, 2);
    harp_cbor_array(m, 2);
    harp_cbor_uint(m, 0);
    harp_cbor_uint(m, end);
    if (channel) { /* key 5 = multitimbral part (§9.4); omitted for part 0 */
        harp_cbor_uint(m, 5);
        harp_cbor_uint(m, channel);
    }
}

/* Drain up to `budget` events from the owner source's ring, appending each as a
 * framed EVT message to `batch`. Returns the count drained. The eventPump is the
 * SOLE consumer of the source ring (SPSC) — there is one fixed session-long
 * source, so no lock and no safe-free dance.
 *
 * Param sets and ramps carry the SOURCE's channel (key 5) so a multi-out main's
 * per-channel knob edits land on the right part (channel N -> part N).
 * Notes already carry their channel in the UMP word (the shell baked it in).
 * Transport (kind 3) is global and only ever lives on the owner source. */
int EventManager::drainOwner(harp_cbuf &batch, harp_cbuf &msgbuf, int budget) {
    /* M2: the target part (§9.4 key 5) is PER EVENT — te.channel, resolved at queue time
     * from the caller's explicit channel (a satellite's MIDI channel N) or this source's own
     * channel. So one main instance drives every part; an attached single-channel source is
     * byte-identical (its events all carry src.chan). Notes carry their channel in the UMP word. */
    TimedEv te;
    int sent = 0;
    for (; sent < budget && ownerSource_.ring.pop(te); sent++) {
        harp_cbuf_reset(&msgbuf);
        if (te.kind == 0)
            encodeParamEvent(&msgbuf, te.a, te.v, te.ts, te.channel);
        else if (te.kind == 1)
            encodeRampEvent(&msgbuf, te.a, te.v, te.ts, te.end, te.channel);
        else if (te.kind == 3) {
            double ppq;
            memcpy(&ppq, &te.end, sizeof ppq);
            encodeTransportEvent(&msgbuf, te.a, te.v, ppq, te.ts);
        } else if (te.kind == 4)
            /* mod (§9.4): the voice key rides in `end`; a per-voice mod takes its
             * part from the voice key, a part-wide mod (voice 0) from te.channel. */
            encodeModEvent(&msgbuf, te.a, te.v, te.ts, (uint32_t)te.end, te.channel);
        else
            encodeUmpEvent(&msgbuf, te.a, te.ts);
        harp_frame_hdr h = {HARP_FRAME_FVER, HARP_STREAM_EVT, HARP_FLAG_FIN,
                            (uint32_t)msgbuf.len};
        uint8_t hdr[HARP_FRAME_HDR_LEN];
        harp_frame_hdr_encode(&h, hdr);
        harp_cbuf_put(&batch, hdr, sizeof hdr);
        harp_cbuf_put(&batch, msgbuf.buf, msgbuf.len);
    }
    return sent;
}

/* Feeder-thread poll: log the events newly dropped to ring overflow since the last
 * call (log-on-change, so an idle stream is quiet). evDropsLogged_ is this plane's
 * private bookkeeping; the feeder is its sole caller. */
void EventManager::pollDropLog() {
    uint64_t drops = evDrops_.load(std::memory_order_relaxed);
    if (drops != evDropsLogged_) {
        log_msg("WARNING: %llu events dropped (ring overflow)",
                (unsigned long long)(drops - evDropsLogged_));
        evDropsLogged_ = drops;
    }
}

/* ================== HarpRuntime event-plane facade ==================
 * The public event API delegates to events_. queueParamSet / queueRamp apply the
 * §12.2/§11.4 read-only WRITE HOLD HERE (session/reconcile policy that must NOT
 * live in the event plane) — byte-for-byte the check that used to open those two
 * methods: a null source is a no-op, and while a read-only hold is active a live
 * param/automation write is DROPPED + counted (roWrDrops_) so it can never reach a
 * mismatched-engine device. queueNote/queueMod/queueTransport are never gated. */
void HarpRuntime::queueParamSet(EventSource *src, uint32_t id, float v, uint64_t ts,
                                uint8_t channel) {
    if (!src) return;
    if (readOnlyDefault_.load(std::memory_order_relaxed) ||
        roExplicit_.load(std::memory_order_relaxed)) {
        roWrDrops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    events_.queueParamSet(src, id, v, ts, channel);
}
void HarpRuntime::queueRamp(EventSource *src, uint32_t id, float target, uint64_t start,
                            uint64_t end, uint8_t channel) {
    if (!src) return;
    if (readOnlyDefault_.load(std::memory_order_relaxed) ||
        roExplicit_.load(std::memory_order_relaxed)) {
        roWrDrops_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    events_.queueRamp(src, id, target, start, end, channel);
}
void HarpRuntime::queueNote(EventSource *src, uint32_t word, uint64_t ts) {
    events_.queueNote(src, word, ts);
}
void HarpRuntime::queueMod(EventSource *src, uint32_t id, float offset,
                           uint32_t voice, uint64_t ts) {
    events_.queueMod(src, id, offset, voice, ts);
}
void HarpRuntime::queueTransport(EventSource *src, uint32_t flags, double tempo,
                                 double ppq, uint64_t ts) {
    events_.queueTransport(src, flags, tempo, ppq, ts);
}
void HarpRuntime::feedTransport(bool playing, bool tempoValid, double tempo,
                                bool posValid, double ppq, uint32_t blockSamples,
                                uint64_t base) {
    events_.feedTransport(playing, tempoValid, tempo, posValid, ppq, blockSamples,
                          base, rate_);
}
