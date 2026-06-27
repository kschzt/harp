/* Per-device runtime construction + the §8.4 admission ledger — see
 * runtime_registry.h. The HarpRuntime-sharing registry this TU once held (the
 * P4/P5 owner+attached refcount table keyed by serial) is RETIRED: one private
 * runtime per claim, so there is nothing to share. Construction + the ledger
 * are all that remain.
 */
#include "runtime_registry.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "runtime.h"

namespace {

/* §8.4 admission ledger: audio bandwidth reserved per transport PATH, one row per live
 * session. A SEPARATE mutex from registryMutex() — the ledger functions take no other
 * lock, so they nest with nothing (audioStart holds ctlMutex_ when it reserves). */
struct PathLedger {
    std::map<std::string, uint64_t> reservations; /* reservationKey -> bytes/sec */
    uint64_t capacity = 0;                         /* last capacity seen on this path (diag) */
};
std::mutex &ledgerMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, PathLedger> &ledgerTable() {
    static std::map<std::string, PathLedger> t;
    return t;
}

} // namespace

std::unique_ptr<HarpRuntime> runtime_acquire() {
    /* A fresh, PRIVATE runtime — no table, no refcount, no sharing. Heap-
     * allocated exactly as the retired empty-serial path's `new HarpRuntime()`,
     * so the golden / single-instance render is byte-identical. The owner drives
     * configure()/start(); the unique_ptr's dtor (~HarpRuntime) stop()s, joining
     * the supervisor/reader/pump threads on teardown — what runtime_release did. */
    return std::unique_ptr<HarpRuntime>(new HarpRuntime());
}

/* ---- §8.4 admission ledger ---- */

bool ledger_reserve(const std::string &pathKey, const std::string &reservationKey,
                    uint64_t needBps, uint64_t capacityBps, uint64_t *outReserved,
                    uint64_t *outCapacity, uint64_t *outAvail) {
    std::lock_guard<std::mutex> lk(ledgerMutex());
    auto &table = ledgerTable();
    /* sum every OTHER session's reservation on this path — excluding our OWN row makes a
     * re-negotiation that overwrites its figure inherently safe (no double-count, no leak).
     * find() (not operator[]) so a REFUSAL on a transient/unknown path leaves no zombie row. */
    auto it = table.find(pathKey);
    uint64_t used = 0;
    if (it != table.end())
        for (const auto &kv : it->second.reservations)
            if (kv.first != reservationKey) used += kv.second;
    uint64_t avail = capacityBps > used ? capacityBps - used : 0;
    if (outReserved) *outReserved = used;
    if (outCapacity) *outCapacity = capacityBps;
    if (outAvail) *outAvail = avail;
    if (needBps > avail) return false; /* refuse: record NOTHING, leave the table clean */
    PathLedger &pl = table[pathKey];   /* admit: now create/get the row */
    pl.capacity = capacityBps;
    pl.reservations[reservationKey] = needBps;
    return true;
}

void ledger_release(const std::string &pathKey, const std::string &reservationKey) {
    std::lock_guard<std::mutex> lk(ledgerMutex());
    auto it = ledgerTable().find(pathKey);
    if (it != ledgerTable().end()) it->second.reservations.erase(reservationKey); /* idempotent */
}

uint64_t ledger_reserved(const std::string &pathKey, uint64_t *outCapacity) {
    std::lock_guard<std::mutex> lk(ledgerMutex());
    auto it = ledgerTable().find(pathKey);
    uint64_t used = 0, cap = 0;
    if (it != ledgerTable().end()) {
        for (const auto &kv : it->second.reservations) used += kv.second;
        cap = it->second.capacity;
    }
    if (outCapacity) *outCapacity = cap;
    return used;
}
