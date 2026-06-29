/* Unit tests for the shell runtime's host-side FUNCTIONAL core, exercised on a
 * constructed HarpRuntime with NO start()/device/transport (the ctor opens only the
 * on-disk store; queue* push to the owner EventSource ring — the SPSC producer side —
 * which the test drains directly as the consumer). Covers the surface registry_tests
 * does not: the param/ramp/note/mod event QUEUE logic + §9.4 per-part channel
 * targeting, the host noteId->voice map (shell/note_voice_map.h), and malformed-bundle
 * REJECTION (HarpRuntime::setStateBundle).
 *
 * Hermetic like registry_tests.cpp (temp HOME before construction). Host-free, builds
 * on all 3 OSes (the same source set as harp-shell; MSVC-clean). harp_voice_pick (§9.5
 * voice allocation) is already covered by harp_engine_logic_tests.c and is NOT repeated. */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#ifndef _WIN32
#include <cstdlib> /* mkdtemp, setenv — POSIX */
#include <unistd.h>
#endif

#include "runtime_registry.h" /* runtime_acquire() — the no-device construction seam */
#include "runtime.h"          /* HarpRuntime, EventSource, queue*, setStateBundle */
#include "ring.h"             /* TimedEv, TimedRing — the host-free observable */
#include "note_voice_map.h"   /* NoteVoiceMap */
#include "harp/cbor.h"        /* build CBOR bundles for the malformed-bundle tests */
#include "harp/store.h"       /* harp_ref + harp_ref_encode — the positive-control ref */

#include "check.h"

/* The §9.4 wire constants are private #defines in runtime.cpp; the host-free test uses
 * the literal strings the wire format pins (a divergence here IS a regression). */
static const char *kMagic = "harpb";
static const char *kLiveRef = "live/project";

/* ---------------------------------------------------------------------------------
 * 1. PARAM / RAMP queue. queueParamSet/queueRamp encode a TimedEv onto the owner ring;
 *    the test pops it (the consumer side) and asserts the field mapping the feeder relies
 *    on: param = {kind 0, a=id, v=value, ts}, ramp = {kind 1, a=id, v=target, ts=start,
 *    end=end}. The id/value pass through unaltered (no truncation/clamp). */
static void test_param_ramp_queue() {
    std::unique_ptr<HarpRuntime> rt = runtime_acquire();
    CHECK(rt != nullptr);
    EventSource *src = rt->ownerSource();
    TimedEv ev;

    rt->queueParamSet(src, 42, 0.75f, 1000); /* default channel */
    CHECK(src->ring.pop(ev));
    CHECK(ev.kind == 0 && ev.a == 42 && ev.v == 0.75f && ev.ts == 1000 && ev.channel == 0);
    CHECK(!src->ring.pop(ev)); /* exactly one event was queued */

    rt->queueRamp(src, 7, 1.0f, 2000, 5000); /* target 1.0 from ts 2000 to 5000 */
    CHECK(src->ring.pop(ev));
    CHECK(ev.kind == 1 && ev.a == 7 && ev.v == 1.0f && ev.ts == 2000 && ev.end == 5000);

    /* full 32-bit id + a negative value pass through unchanged (no truncation/clamp) */
    rt->queueParamSet(src, 0xfffffffeu, -1.0f, 9);
    CHECK(src->ring.pop(ev) && ev.a == 0xfffffffeu && ev.v == -1.0f);
}

/* 2. §9.4 PER-PART channel targeting. setChannel masks to 0xf; a queued param resolves
 *    its part at queue time — the default (kChanFromSource) from the source's channel, an
 *    explicit channel overriding it (a multi-out main driving part N), both masked to 0xf. */
static void test_channel_targeting() {
    std::unique_ptr<HarpRuntime> rt = runtime_acquire();
    EventSource *src = rt->ownerSource();
    TimedEv ev;

    rt->setChannel(0x1f); /* masked to 0xf */
    CHECK(rt->channel() == 0xf);
    rt->queueParamSet(src, 1, 0.1f, 1); /* implicit -> source channel */
    CHECK(src->ring.pop(ev) && ev.channel == 0xf);

    rt->setChannel(3);
    rt->queueParamSet(src, 1, 0.1f, 1, 9); /* explicit 9 overrides source 3 */
    CHECK(src->ring.pop(ev) && ev.channel == 9);

    rt->queueParamSet(src, 1, 0.1f, 1, 0x17); /* explicit channel masked to 0xf -> 7 */
    CHECK(src->ring.pop(ev) && ev.channel == 7);

    /* a ramp resolves its part the same way */
    rt->setChannel(5);
    rt->queueRamp(src, 2, 0.5f, 0, 10);
    CHECK(src->ring.pop(ev) && ev.channel == 5);
}

/* 3. NOTE queue (MPE). queueNote stores the UMP word verbatim in `a` (the channel nibble
 *    rides IN the word, not te.channel) as kind 2. A note carries no read-only gate — a
 *    note must always reach the device. Overflow drops are bounded by the 1024-slot ring. */
static void test_note_queue() {
    std::unique_ptr<HarpRuntime> rt = runtime_acquire();
    EventSource *src = rt->ownerSource();
    TimedEv ev;

    uint32_t word = 0x90453C40; /* a MIDI-1.0-in-UMP note-on word (channel nibble in bits 16-19) */
    rt->queueNote(src, word, 1234);
    CHECK(src->ring.pop(ev));
    CHECK(ev.kind == 2 && ev.a == word && ev.ts == 1234 && ev.channel == 0);

    /* ring overflow is bounded + lossless up to capacity: 1024 notes fit, a 1025th drops.
     * Mark the 1025th distinctly and prove it never appears in the drained 1024. */
    for (int i = 0; i < 1024; i++) rt->queueNote(src, (uint32_t)(0x90000040u + i), (uint64_t)i);
    rt->queueNote(src, 0xDEADBEEFu, 99999); /* 1025th — ring full, must drop */
    int count = 0;
    bool sawOverflow = false;
    while (src->ring.pop(ev)) {
        count++;
        if (ev.a == 0xDEADBEEFu) sawOverflow = true;
    }
    CHECK(count == 1024);
    CHECK(!sawOverflow); /* the over-capacity note was dropped, not silently overwriting one */
}

/* 4. MOD queue (§9.4 non-destructive per-voice modulation, etype 6). queueMod = {kind 4,
 *    a=id, v=offset, ts, end=voiceKey, channel=source}. The §9.5 voice key ((channel<<8)|note)
 *    rides in `end`; voice 0 = part-wide. The source channel is the part FALLBACK (the device
 *    derives a per-voice mod's part from the voice key, a part-wide mod's from this channel). */
static void test_mod_queue() {
    std::unique_ptr<HarpRuntime> rt = runtime_acquire();
    rt->setChannel(5);
    EventSource *src = rt->ownerSource();
    TimedEv ev;

    uint32_t voiceKey = (5u << 8) | 60u; /* channel 5, note 60 */
    rt->queueMod(src, 42, 0.5f, voiceKey, 2000);
    CHECK(src->ring.pop(ev));
    CHECK(ev.kind == 4 && ev.a == 42 && ev.v == 0.5f && ev.ts == 2000 && ev.end == voiceKey);
    CHECK(ev.channel == 5); /* the source channel is carried as the part fallback */

    rt->queueMod(src, 9, -1.0f, 0, 3000); /* part-wide (voice 0) */
    CHECK(src->ring.pop(ev) && ev.kind == 4 && ev.end == 0 && ev.channel == 5);
}

/* 5. NoteVoiceMap — host noteId -> §9.5 packed voice key (shell/note_voice_map.h). The
 *    bridge a shell uses to forward per-note (VST3 Note Expression / CLAP per-note) mod to
 *    the right device voice. Round-trip, note-off, the noteId<0 part-wide fallback, same-id
 *    refresh, independence across notes, reset, and the fixed-table bound. */
static void test_note_voice_map() {
    NoteVoiceMap m;
    m.noteOn(42, (3u << 8) | 60u);
    CHECK(m.voiceFor(42) == ((3u << 8) | 60u)); /* round-trip */
    CHECK(m.voiceFor(99) == 0);                 /* an unseen noteId -> 0 (part-wide) */

    m.noteOff((3u << 8) | 60u);
    CHECK(m.voiceFor(42) == 0); /* off clears the mapping */

    m.noteOn(-1, 0x100);
    CHECK(m.voiceFor(-1) == 0); /* no host id (noteId < 0) -> not tracked, part-wide */

    m.noteOn(7, 0x201);
    m.noteOn(7, 0x202);          /* the SAME id refreshes its slot, not a second entry */
    CHECK(m.voiceFor(7) == 0x202);

    m.reset();
    m.noteOn(1, 0xA);
    m.noteOn(2, 0xB);
    CHECK(m.voiceFor(1) == 0xA && m.voiceFor(2) == 0xB); /* distinct notes don't bleed */
    m.reset();
    CHECK(m.voiceFor(1) == 0 && m.voiceFor(2) == 0); /* reset clears all */

    /* the table is a bounded sparse fixed array (64). Fill it, then a 65th distinct id
     * finds no free slot and no matching id -> silently untracked, the 64 intact. */
    NoteVoiceMap full;
    for (int i = 0; i < 64; i++) full.noteOn(1000 + i, 0x10000u + (uint32_t)i);
    full.noteOn(2000, 0xDEAD);
    CHECK(full.voiceFor(2000) == 0);          /* over the bound -> not stored */
    CHECK(full.voiceFor(1000) == 0x10000u);   /* the original 64 are undisturbed */
    CHECK(full.voiceFor(1063) == 0x10000u + 63u);
}

/* ---- malformed-bundle helpers ---- */

/* Build a bundle with an OPTIONAL magic value (nullptr = omit key 0 entirely) and a refs
 * array holding one born ref named refName. Each negative below varies exactly ONE field
 * from the valid shape, so a rejection ISOLATES the gate under test (a magic-only change
 * still carries a valid LIVE_REF, so haveTarget can't be the reason it's rejected). A valid
 * bundle on an UNCONNECTED runtime stages offline and returns true. */
static void build_bundle(std::vector<uint8_t> &out, const char *magic, const char *refName) {
    harp_cbuf b;
    harp_cbuf_init(&b);
    harp_cbor_map(&b, (uint64_t)(magic ? 2 : 1)); /* (magic?) + refs */
    if (magic) {
        harp_cbor_uint(&b, 0);
        harp_cbor_text(&b, magic);
    }
    harp_cbor_uint(&b, 3); /* refs */
    harp_cbor_array(&b, 1);
    harp_ref r;
    memset(&r, 0, sizeof r);
    strncpy(r.name, refName, sizeof r.name - 1);
    r.unborn = false; /* born -> a real target */
    r.dirty = false;
    r.generation = 1;
    r.hash.b[0] = 0x01; /* SHA-256 algo byte so harp_hash_read accepts the 33-byte hash */
    harp_ref_encode(&b, &r);
    out.assign(b.buf, b.buf + b.len);
    harp_cbuf_free(&b);
}

/* 6. MALFORMED-BUNDLE rejection. setStateBundle MUST reject malformed input with false and
 *    NOT crash, at each parse gate: a positive control proves the harness can produce a
 *    `true`, so each negative is a meaningful rejection. */
static void test_setstatebundle_rejection() {
    /* positive control: a well-formed bundle stages (offline) -> true */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        std::vector<uint8_t> good;
        build_bundle(good, kMagic, kLiveRef);
        CHECK(rt->setStateBundle(good.data(), good.size()) == true);
    }
    /* zero length -> false (the !len gate, before any deref) */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        uint8_t dummy = 0;
        CHECK(rt->setStateBundle(&dummy, 0) == false);
    }
    /* non-map root (a CBOR array) -> false (harp_cdec_map fails) */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        harp_cbuf b;
        harp_cbuf_init(&b);
        harp_cbor_array(&b, 0);
        CHECK(rt->setStateBundle(b.buf, b.len) == false);
        harp_cbuf_free(&b);
    }
    /* wrong magic on an OTHERWISE-VALID bundle (valid LIVE_REF) -> false. Only magicOk differs
     * from the positive control, so this isolates the magic-value check (haveTarget is true). */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        std::vector<uint8_t> b;
        build_bundle(b, "WRONG", kLiveRef); /* 5 bytes, != "harpb" */
        CHECK(rt->setStateBundle(b.data(), b.size()) == false);
    }
    /* magic key OMITTED entirely, but a valid LIVE_REF present -> false (magicOk default false).
     * Isolates that a MISSING magic is rejected, not only a wrong one. */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        std::vector<uint8_t> b;
        build_bundle(b, nullptr, kLiveRef);
        CHECK(rt->setStateBundle(b.data(), b.size()) == false);
    }
    /* magic ok but NO refs array -> false (haveTarget never set) */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        harp_cbuf b;
        harp_cbuf_init(&b);
        harp_cbor_map(&b, 1);
        harp_cbor_uint(&b, 0);
        harp_cbor_text(&b, kMagic);
        CHECK(rt->setStateBundle(b.buf, b.len) == false);
        harp_cbuf_free(&b);
    }
    /* refs present but the LIVE_REF is absent (a differently-named ref) -> false. Proves the
     * gate keys on the LIVE_REF name, not merely the presence of any ref. */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        std::vector<uint8_t> other;
        build_bundle(other, kMagic, "archive/2026"); /* born ref, but not live/project */
        CHECK(rt->setStateBundle(other.data(), other.size()) == false);
    }
    /* truncated mid-parse -> false (decoder fails), never a crash/overrun */
    {
        std::unique_ptr<HarpRuntime> rt = runtime_acquire();
        std::vector<uint8_t> good;
        build_bundle(good, kMagic, kLiveRef);
        good.resize(good.size() / 2); /* chop it in half */
        CHECK(rt->setStateBundle(good.data(), good.size()) == false);
    }
}

int main() {
    /* Hermetic store: keep the ctor's harp_store_open out of the real cache. POSIX only;
     * on Windows the default store dir is used (harmless — setStateBundle still parses,
     * and the queue/NoteVoiceMap tests touch no store). Mirrors registry_tests.cpp. */
#ifndef _WIN32
    char tmpl[] = "/tmp/harp-runtime-units.XXXXXX";
    char *home = mkdtemp(tmpl);
    if (home) setenv("HOME", home, 1);
#endif

    test_param_ramp_queue();
    test_channel_targeting();
    test_note_queue();
    test_mod_queue();
    test_note_voice_map();
    test_setstatebundle_rejection();

    return check_report("harp-runtime-units-tests");
}
