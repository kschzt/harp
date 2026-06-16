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
