# Multitimbral RefDev + multi-shell session sharing — implementation plan

Scopes debt #6 (§15.1 runtime/shell split) concretely: one physical device,
many shells, full per-part multitimbral. Spec model landed in 0.3.6
(§15.1 session sharing, §15.2 state-controller vs multitimbral event group,
audio owner). This doc is the build plan and the test matrix.

## Design decisions (resolved)

1. **Part model — 16 parts, full per-part params.** Each MIDI channel (one UMP
   group, channels 0–15) is an independent monophonic synth: its own voice,
   note state, per-param values, per-param ramps, and arp.

2. **Addressing — channel-scoped, NOT ID-namespaced.** Notes are already
   channel-addressed (UMP word). Param-set (§9.2) and ramp (§9.4) events gain an
   OPTIONAL `channel` body key (absent ⇒ 0, back-compat). So:
   - `param_id` stays 1–13; the param-map shape and `param-map-hash` are
     UNCHANGED (it describes the 13 params that exist *per part*).
   - "part" ≡ "channel" uniformly for notes and params.
   - Multitimbrality is declared separately (capability + identity part-count),
     not by inflating the param map.
   This is strictly smaller and safer than namespacing IDs, and mirrors MIDI.

3. **State / recall — per-part values.** A snapshot / `live/project` ref and the
   Recall Bundle capture all 16 parts' param values. `param-map-hash` unchanged;
   the bundle's identity gains the part-count so a recall validates shape.

4. **Host session sharing — registry keyed by `(vid,pid,serial)`.** A
   process-global table; a 2nd shell binding an already-bound unit *attaches*
   (it cannot open a 2nd USB claim). Refcounted acquire/release; last release
   tears the session down. Per-instance otherwise (distinct serial ⇒ own
   session).

5. **Host event merge — per-shell SPSC ring, single feeder consumer.** Each
   attached shell owns its own `TimedRing` (its audio thread is the sole
   producer — no ring becomes multi-producer, so the TSan-clean invariant
   holds). The feeder drains every shell's ring and merges in timestamp order,
   stamping each shell's assigned channel. No hot ring is rewritten MPSC.

6. **Per-part audio outputs + main mix.** Each part renders its own stereo pair
   (16 × 2 = 32 channels), plus a summed stereo **main mix** (2 channels) =
   **34 output channels**, declared in the channel map (§6.3) as "Main L/R",
   "Part 1 L/R" … "Part 16 L/R". The shared session's audio plane carries all
   slots; the runtime demuxes slot→shell so each track gets its own part's
   audio (no audio-owner election). The host streams only active parts'
   slots (`active-slots-out`, §8). **Determinism anchor:** the main mix with
   only part 0 active == the current single-part output, so the existing
   golden reproduces on the main-mix slots and the engine split stays
   verifiable; per-part play gets new per-slot goldens.

7. **Per-shell channel** is a plugin parameter (default: round-robin on attach,
   user-overridable), so a DAW project pins each track to its part.

## Spec changes

- [x] §15.1 — session sharing, many-shells-to-one-device, registry keying.
- [x] §15.2 — state-controller (single writer) vs multitimbral event group
      (disjoint channels), audio owner.
- [x] 0.3.6 changelog + header bump.
- [x] §9.4 — OPTIONAL `channel` key (key **5**, NOT 3 — key 3 is `voice`,
      §9.5, a different axis) on param-set and ramp event bodies (default 0).
      `harp.cddl` updated. Distinguished from `voice` in prose (part base value
      vs transient sounding voice).
- [ ] §C / identity — `evt.multitimbral` capability + identity key for part
      count (mirrors 0.3.5's `ump-group-map`). [P3]

## Phased implementation (each phase: committable + testable)

| P | scope | risk | hw-validate? |
|---|-------|------|--------------|
| ~~P1~~ ✅ | Wire: `channel` key (key 5) on param/ramp encode+decode (runtime + device parse + `dev_event`), default 0, byte-identical for part 0. Unit test `test_event_channel` (byte-identity + round-trip + skip-unknown). Host+device compile-verified | low | done |
| ~~P2.0~~ ✅ | Engine per-part **data model**: `part` struct (voice+note+arp), `NPARTS=1`, all→part 0; params/ramps/evq/counters stay global. **Byte-identical** — golden+groove unchanged (A/B on PI4B-0003, both shells). Verified by a workflow (impl + 5-lens adversarial equivalence + completeness critic) then the hardware oracle | high | **done (golden held)** |
| ~~P2.1~~ ✅ | `NPARTS=16`; notes/arp route by channel to their part; transport broadcasts to all parts' arps. **Golden-preserving render**: part 0 always renders the full voice (drone+note) with a direct `=` write — bit-identical to P2.0 incl. `−0.0f` (the signed-zero subtlety the adversarial pass caught: `memset`+`+=` flipped 5408 groove samples); parts 1..15 are note-only, rendered only when active (note/latch/sounding/env-tail), accumulated. Drone hazard resolved (only part 0 drones at P2.1; per-part drone is P3). Host note-channel chain (`ump.h`/`plugin.cpp`/`harp_au.mm`/`vst3-host --channel`, default 0). **Verified on PI4B-0003**: golden 65770cc8 + groove 45e240e2 unchanged; ch5≠ch0≠drone-only (routing real, notes reach part 5); realtime flood evt_late/overruns flat | **high** | **done** |
| ~~P2.2~~ ✅ | 34-channel output (Main L/R + 16 stereo Part pairs), channel map (§6.3), `active-slots-out` parse. Default/`[0,1]` request → **literal P2.1 render** → golden+groove byte-identical (no regen). Per-part path additive: each part rendered once/segment into its slot(s); host `--part N` requests a part's slots. **Verified PI4B-0003**: golden 65770cc8 + groove 45e240e2 held; `multitimbral-test.sh` PASS — all 16 parts play their channel, exclusive routing (no leak), unplayed parts silent (rms 0). New test `scripts/multitimbral-test.sh` (the "all timbres play" gate) | **high** | **done** |
| ~~P3~~ ✅ | Per-part param VALUES + ramps (16 independent timbres); g_params stays the global definitions table. 16-part state object `{partIdx:{id:value}}` (all 16), deterministic + atomic load + old-flat migration; per-part `evt.param.echo` channel; `evt.multitimbral` capability + identity part-count (16). Panel edits part 0 (full per-part panel = follow-up). **Verified PI4B-0003**: golden 65770cc8 + groove 45e240e2 byte-identical; `param-map: 01f5b2e2…` unchanged; `evt.multitimbral` advertised; per-part isolation (distinct timbres) + **recall round-trip lossless across save/scramble/reload**. New `recall-perpart-test.sh`; both new hw tests wired into `hw-tests{,-linux}.sh`. **Closer done**: params-blob codec extracted to a pure `refdev_{encode,parse}_params_blob` (fixed the `load_part_map` mid-map smell → fail-clean), `fuzz-state` target + corpus (crash-free on hostile input), `harp-device-tests` (round-trip/determinism/legacy-migration/fail-clean) wired into `ctest` (the `ci` `core` job now runs ctest, not just `harp-tests`). Verified on CI (hw+ci green) | med | **done** |
| ~~P4~~ ✅ | Process-global **runtime registry** (`shell/runtime_registry.*`): instances explicitly targeting the same serial share ONE runtime/session/claim (refcounted acquire/release, owner vs dormant-attached role). NOT the old singleton — empty/auto serial → fresh unshared runtime (single-instance byte-identical; #16 preserved). `plugin.cpp` + `tsan-host` migrated; `setActive` idempotent. **Verified**: golden 65770cc8 byte-identical, `session-share-test` (2 instances → 1 claim, TSan-clean), `multidevice-test` (#16: distinct serials → distinct units), `registry` ctest (both build configs). New `session-share-test.sh` wired into hw runners. *Deferred (tracked in code)*: AU shell registry migration; owner-handoff when owner releases first (P5). | low | **done** |
| P5 | Host contributor merge: per-shell channel + event source, feeder merge, siblings silent; shells expose channel param | **high** (RT core) | **yes** |
| P6 | `tools/vst3-host`: per-instance channel, multi-channel note sequences, multi-instance shared-session driving (headless e2e) | low | no |
| P7 | Test matrix wired into CI + hw runner across 3 platforms (below) | — | yes |

Sequencing rule: P1 → P4 are low/med risk and land first. P2 and P5 touch the
determinism and RT paths respectively and each ships with its own hardware
validation pass before the next phase builds on it.

## Test matrix — existing suites and the gaps

Platforms: **macOS** (VST3+AU, dev rig / harp.local), **Windows** (MSVC VST3,
vendored libusb), **Linux** (CI KVM runner + harptest.local; reaper-e2e headless).

| test | exists | covers now | multitimbral addition | platforms |
|------|--------|-----------|----------------------|-----------|
| `fuzz/` + `gen-corpus` | yes | cbor/envelope/object/link/audio parse | add `channel`-keyed param/ramp seeds; assert absent⇒0 | Linux CI (libFuzzer), macOS ASan replay |
| wire round-trip (new) | no | — | encode→decode param/ramp with channel 0/15/absent | all 3 (unit, no device) |
| determinism golden | yes (device-rendered) | single-part render | regen golden; add **per-part** golden (notes on ch N) | render on hw, hash host-independent |
| `timing-test.sh` | yes | single-part note/ramp timing | run per-part (a channel’s timing must match single-part) | macOS rig |
| `multidevice-test.sh` | yes | distinct serials → distinct sessions | add **same-serial → one session** (registry) | macOS rig (2 boards) |
| session-sharing (new) | no | — | 2 instances same serial: 1 claim, refcount teardown | all 3 (host-side, device optional) |
| contributor-merge (new) | no | — | events from 2 shells land on 2 parts, in order | macOS + Linux rig |
| audio-owner (new) | no | — | owner streams; sibling bus is silent | macOS rig |
| recall round-trip | yes (reaper-e2e) | single-part setState | per-part: save 16 parts, reopen, byte-identical | Linux hw runner (REAPER), macOS |
| `tsan-shell.sh` | yes | one runtime, N instances device-less/live | drive **2 shells sharing one runtime** (merge path) | macOS + Linux rig |
| `abuse-test.sh` | yes | T9-lite hostile inputs | add channel-out-of-range param/ramp | Linux |
| `replug-test.sh` | yes | session resume | resume re-asserts all parts | macOS rig |
| Windows e2e | reaper-e2e is Linux | — | add a Windows headless host smoke (vst3-host) covering channel routing | Windows |

New scripts: `scripts/multitimbral-test.sh` (device: 16-channel render →
distinct voices + per-part timing), `scripts/session-share-test.sh` (host:
registry/refcount/audio-owner, runs on all 3), and `tools/vst3-host`
`--channel`/`--instances-channels` flags feeding the above.

## Open risks
- **Goldens are device-rendered** → P2 needs a hardware regen; the macOS golden
  reproduces on the Linux runner (host-OS-independent), so regen once on the rig.
- **P5 is the only MPSC-adjacent change** — mitigated by per-shell SPSC rings
  (design decision 5); still requires a fresh `tsan-shell.sh` two-shells run.
- **Pi CPU**: 16 monophonic voices is cheap, but confirm the realtime flood
  matrix (`timing-test.sh` evt_late) still holds with all 16 parts gated.
