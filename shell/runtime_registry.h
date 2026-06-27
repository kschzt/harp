/* Per-device runtime construction + the §8.4 admission ledger.
 *
 * HISTORY: this TU once hosted a process-global registry that SHARED one
 * HarpRuntime across plugin instances naming the same serial (the P4/P5
 * multitimbral-alias model — N shells, one device, one claim, owner+attached
 * refcount + an event-source merge). That model is RETIRED: a device is now
 * ONE multi-out MAIN instance per claim (17 buses, channel->part routing), so
 * every runtime is private to its single owner and there is nothing to share.
 * Multi-device (two units in one project) still works because each main
 * instance gets its own private runtime and HarpRuntime::selectDevice() binds
 * a DIFFERENT unit per serial — selection, not a shared table, is what keeps
 * two units apart.
 *
 * What remains in this TU: (1) the runtime construction seam below, and (2) the
 * §8.4 bandwidth ledger, which still needs ONE process-global home reached by
 * every independent per-device runtime.
 *
 * SELECTION IS UNCHANGED and lives ENTIRELY in HarpRuntime::selectDevice()
 * (HARP_DEVICE_SERIAL env / bundle usb-identity / first-unclaimed +
 * singleton-kill). The construction seam does NO selection — it just hands back
 * a fresh runtime the owner drives by value (configure / start / pull / state).
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

class HarpRuntime;

/* Construct a fresh, PRIVATE runtime. Every caller gets its own — there is no
 * sharing — so this is byte-identical to owning a HarpRuntime by value (the
 * golden-render path, the old empty-serial case, now the only case). The owner
 * drives configure()/start() itself; device selection is
 * HarpRuntime::selectDevice()'s job (env / bundle / auto-select), not this
 * seam's. Returned by unique_ptr: the owner's lifetime IS the runtime's — its
 * dtor (~HarpRuntime) calls stop(), joining the supervisor/reader/pump threads,
 * exactly as the retired runtime_release did before destroying. */
std::unique_ptr<HarpRuntime> runtime_acquire();

/* §8.4 admission-control ledger — process-global, keyed by transport PATH (one USB
 * controller / eth segment carries several device sessions but ONE bandwidth budget).
 * Bandwidth is bytes/sec; reservationKey names one live session on the path. Lives in the
 * registry TU because that is the one shared, process-global home reached by every
 * independent per-device runtime. Its own mutex (distinct from the registry's), and the
 * functions take no other lock — callers MUST NOT take ctlMutex_ while holding the ledger.
 *   ledger_reserve  — admit needBps iff (sum of OTHER sessions on pathKey) + needBps <=
 *                     capacityBps; records the row on admit, nothing on refusal. ALWAYS
 *                     fills outReserved (others' usage) / outCapacity / outAvail so the
 *                     caller can refuse WITH the computed budget. Returns whether admitted.
 *   ledger_release  — drop this session's row (idempotent: erase-if-present).
 *   ledger_reserved — total bytes/sec reserved on pathKey (ALL sessions) + the path
 *                     capacity, for the §14.4 diagnostic bundle. */
bool ledger_reserve(const std::string &pathKey, const std::string &reservationKey,
                    uint64_t needBps, uint64_t capacityBps,
                    uint64_t *outReserved, uint64_t *outCapacity, uint64_t *outAvail);
void ledger_release(const std::string &pathKey, const std::string &reservationKey);
uint64_t ledger_reserved(const std::string &pathKey, uint64_t *outCapacity);
