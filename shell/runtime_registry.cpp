/* Runtime registry (P4) — see runtime_registry.h for the contract and the
 * "this is NOT the old singleton" distinction.
 *
 * Shape: a process-global, mutex-guarded table keyed by the explicit wanted
 * serial. Each entry owns one HarpRuntime and a refcount. The runtime is
 * created lazily on the first acquire of a serial and destroyed on the last
 * release. The EMPTY serial is never stored — it always yields a fresh,
 * unregistered runtime, so the auto-select / single-instance path never
 * touches the table.
 */
#include "runtime_registry.h"

#include <cstdint>
#include <map>
#include <mutex>

#include "runtime.h"

namespace {

struct Entry {
    HarpRuntime *rt = nullptr;
    unsigned refs = 0;
};

/* The whole registry: one table, one mutex. Function-local statics so there's
 * no global-ctor ordering question across translation units, and the table
 * outlives every plugin instance for the life of the process. */
std::mutex &registryMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, Entry> &registryTable() {
    static std::map<std::string, Entry> t;
    return t;
}

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

RuntimeHandle runtime_acquire(const std::string &wantSerial) {
    /* Empty serial = auto-select / single-instance: ALWAYS a fresh, never-
     * shared, never-registered runtime. This is the byte-identical golden
     * path — the registry adds nothing to it but a heap allocation the caller
     * drives exactly like a by-value HarpRuntime. The owner caller calls
     * configure()/start() itself; we only hand back the object. */
    if (wantSerial.empty()) {
        RuntimeHandle h;
        h.rt = new HarpRuntime();
        h.owner = true;
        return h;
    }

    /* Explicit serial: share by serial. The table lookup/mutation is the only
     * critical section — short and non-blocking. We deliberately do NOT
     * start() the runtime under the lock: the owner caller starts it after we
     * return (a synchronous sessionUp() can take time), so other instances on
     * DIFFERENT serials never serialize behind one unit's connect. */
    std::lock_guard<std::mutex> lk(registryMutex());
    Entry &e = registryTable()[wantSerial];
    RuntimeHandle h;
    if (e.rt == nullptr) {
        /* first instance for this unit: create + register + own it. */
        e.rt = new HarpRuntime();
        e.refs = 1;
        h.rt = e.rt;
        h.owner = true;
    } else {
        /* a sibling already owns this unit's runtime: attach (dormant in P4). */
        e.refs++;
        h.rt = e.rt;
        h.owner = false;
    }
    return h;
}

void runtime_release(const RuntimeHandle &h) {
    if (!h.rt) return; /* default/zeroed handle, or already released */

    /* An unregistered (empty-serial) runtime is owned solely by its handle:
     * the owner is always the creator and there's no table entry. Find out by
     * searching the table for this rt; if it isn't there, it's the fresh
     * private kind — stop+destroy it directly. (Searching by pointer keeps
     * RuntimeHandle free of the serial, so a handle never lies about its key.)
     *
     * stop() + delete happen OUTSIDE the lock: stop() joins the supervisor/
     * reader/pump threads and can block, and we must not hold the table mutex
     * across that (it would stall acquires/releases for unrelated serials).
     * We null the entry's rt and erase it under the lock, then tear down the
     * captured pointer after unlocking. */
    HarpRuntime *toDestroy = nullptr;
    {
        std::lock_guard<std::mutex> lk(registryMutex());
        auto &table = registryTable();
        auto it = table.end();
        for (auto i = table.begin(); i != table.end(); ++i) {
            if (i->second.rt == h.rt) {
                it = i;
                break;
            }
        }
        if (it == table.end()) {
            /* not registered: the private fresh runtime. Tear it down. */
            toDestroy = h.rt;
        } else if (--it->second.refs == 0) {
            /* last reference to a shared runtime: unregister and tear down. */
            toDestroy = it->second.rt;
            table.erase(it);
        }
        /* else: other instances still attached; leave the session running. */
    }
    if (toDestroy) {
        toDestroy->stop(); /* joins supervisor/reader/pump, like ~HarpRuntime */
        delete toDestroy;
    }
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
