/* Unit tests for the P4 process-global runtime registry
 * (shell/runtime_registry.*): the keying + refcount logic that lets several
 * plugin/host instances naming the SAME serial ride ONE shared HarpRuntime
 * (one USB session, one claim), while everything else stays per-instance.
 *
 * NO DEVICE NEEDED. The registry never calls start() — its contract is "hand
 * back the object; the owner calls configure()/start() itself". A HarpRuntime
 * that is never started touches no USB and spawns no threads: the ctor only
 * opens the on-disk object store (degrades gracefully), and stop()/dtor early-
 * return when running_ was never set. So acquire()/release() exercise the table
 * bookkeeping in full against real HarpRuntime objects, with zero hardware. We
 * point HOME at a temp dir so the store-open is hermetic and writes nothing to
 * the developer's real cache. This is the clean path: no test hook in the
 * shell sources was required (start() is already short-circuited by simply
 * not calling it).
 *
 * What it asserts (every rule in runtime_registry.h):
 *   - same serial twice  -> SAME rt; owner only on the FIRST acquire (attach).
 *   - different serials   -> DIFFERENT rt; each is its own owner.
 *   - empty serial        -> ALWAYS a fresh, never-registered, never-shared rt
 *                            (owner every time), even back-to-back.
 *   - refcount: a shared entry survives until its LAST release, then is torn
 *     down and ERASED — re-acquiring the serial yields a NEW owner, proving
 *     the entry went away on the last release, not the first.
 *   - cross-talk: empty-serial release never disturbs a registered entry.
 *   - idempotence: release of a default/zeroed handle is a no-op.
 */
#include <cstdio>
#include <thread> /* §8.4 admission ledger concurrency sub-test */
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h> /* mkdtemp, setenv — POSIX only */
#endif

#include "runtime_registry.h"
#include "runtime.h"   /* HarpRuntime::bundleWantedSerial (public static) */
#include "harp/cbor.h" /* build a minimal §15.3 bundle for the eth-serial selection test */

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (cond) {                                                        \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                  \
    } while (0)

/* Same serial: first acquire OWNS + creates; subsequent acquires ATTACH to the
 * exact same runtime (owner=false). Releasing back down to zero tears the entry
 * down so the next acquire of that serial is a fresh owner again. */
static void test_same_serial_shares_one_runtime() {
    RuntimeHandle a = runtime_acquire("PI4B-0001");
    CHECK(a.rt != nullptr);
    CHECK(a.owner); /* first instance of this unit: creator + owner */

    RuntimeHandle b = runtime_acquire("PI4B-0001");
    CHECK(b.rt == a.rt); /* SAME shared session, not a second claim */
    CHECK(!b.owner);     /* attached: dormant in P4, must not drive the rt */

    RuntimeHandle c = runtime_acquire("PI4B-0001");
    CHECK(c.rt == a.rt); /* a third instance attaches to the same rt too */
    CHECK(!c.owner);

    /* Release two of three: the shared session must stay alive (refs 3->1). */
    runtime_release(b);
    runtime_release(c);
    RuntimeHandle d = runtime_acquire("PI4B-0001");
    CHECK(d.rt == a.rt); /* still the same live entry — owner still holds it */
    CHECK(!d.owner);
    runtime_release(d);

    /* Last release: entry refcount hits 0, runtime is stopped+destroyed and the
     * key erased. Re-acquiring the serial must therefore mint a NEW OWNER — the
     * owner flag is the sound proof the entry was erased and recreated. (We do
     * NOT compare e.rt against a.rt: a.rt was just delete'd, so reading it is a
     * dangling-pointer value AND the allocator routinely hands the freed slot
     * straight back to the next new HarpRuntime — pointer identity here would
     * be testing malloc, not the registry.) The owner=false on every acquire
     * BEFORE this (b/c/d) proves the entry did NOT die on the first release;
     * owner=true here proves it died on the last. That brackets the refcount. */
    runtime_release(a);
    RuntimeHandle e = runtime_acquire("PI4B-0001");
    CHECK(e.owner); /* fresh owner: the old shared entry was torn down + erased */
    runtime_release(e);
}

/* Different serials never alias: two units, two runtimes, two owners. */
static void test_different_serials_distinct_runtimes() {
    RuntimeHandle x = runtime_acquire("PI4B-0001");
    RuntimeHandle y = runtime_acquire("PI4B-0002");
    CHECK(x.rt != nullptr && y.rt != nullptr);
    CHECK(x.rt != y.rt); /* distinct units -> distinct runtimes (debt #16) */
    CHECK(x.owner && y.owner);

    /* A sibling on each serial attaches to ITS unit's runtime only. */
    RuntimeHandle x2 = runtime_acquire("PI4B-0001");
    RuntimeHandle y2 = runtime_acquire("PI4B-0002");
    CHECK(x2.rt == x.rt && !x2.owner);
    CHECK(y2.rt == y.rt && !y2.owner);

    runtime_release(x2);
    runtime_release(y2);
    runtime_release(x);
    runtime_release(y);
}

/* Empty serial = auto-select / single-instance: ALWAYS a fresh, unregistered,
 * never-shared runtime. Two empty-serial acquires must NOT alias each other
 * (they are both LIVE here, so distinct addresses is sound), and each is its
 * own owner (the byte-identical golden / multi-device path). */
static void test_empty_serial_always_fresh() {
    RuntimeHandle a = runtime_acquire("");
    RuntimeHandle b = runtime_acquire("");
    CHECK(a.rt != nullptr && b.rt != nullptr);
    CHECK(a.rt != b.rt); /* never shared, even back-to-back */
    CHECK(a.owner && b.owner);

    /* An empty-serial runtime is never in the table, so its release must not
     * touch a registered entry that happens to coexist. */
    RuntimeHandle reg = runtime_acquire("PI4B-0001");
    runtime_release(a); /* tears down the private rt only */
    RuntimeHandle reg2 = runtime_acquire("PI4B-0001");
    CHECK(reg2.rt == reg.rt); /* registered entry untouched by the empty release */
    CHECK(!reg2.owner);
    runtime_release(reg2);
    runtime_release(reg);
    runtime_release(b);

    /* After all releases the registered serial is fresh again. */
    RuntimeHandle reg3 = runtime_acquire("PI4B-0001");
    CHECK(reg3.owner);
    runtime_release(reg3);
}

/* Releasing a default/zeroed (never-acquired / already-released) handle is a
 * no-op and must not crash or disturb the table. */
static void test_release_of_zeroed_handle_is_noop() {
    RuntimeHandle zero; /* rt == nullptr */
    runtime_release(zero);
    runtime_release(zero); /* twice, for good measure */
    CHECK(zero.rt == nullptr);
    /* (We do NOT double-release a real handle: that is a caller double-free,
     * not a registry property — the contract only promises the zeroed no-op.) */
}

/* §8.4 admission ledger: cross-session aggregation on one path, refuse-WITH-budget, exact
 * release, re-negotiation idempotency (sum-excluding-own-row), no leak, and >=8 sessions. */
static void test_admission_ledger() {
    uint64_t reserved = 0, cap = 0, avail = 0;
    /* SIM-A reserves 384000 of a 500000 path -> admitted; 116000 left. */
    CHECK(ledger_reserve("eth:T1", "SIM-A", 384000, 500000, &reserved, &cap, &avail));
    /* SIM-B wants 384000 but only 116000 remains -> REFUSED, and the COMPUTED budget is returned. */
    CHECK(!ledger_reserve("eth:T1", "SIM-B", 384000, 500000, &reserved, &cap, &avail));
    CHECK(reserved == 384000 && cap == 500000 && avail == 116000);
    /* release A -> the path frees up; B now fits. */
    ledger_release("eth:T1", "SIM-A");
    CHECK(ledger_reserve("eth:T1", "SIM-B", 384000, 500000, &reserved, &cap, &avail));
    /* re-negotiation: SIM-B re-reserves a WIDER figure on the SAME key — it re-meters against
     * OTHERS only (none here), never refuses itself, never double-counts. */
    CHECK(ledger_reserve("eth:T1", "SIM-B", 480000, 500000, &reserved, &cap, &avail));
    CHECK(reserved == 0); /* no OTHER session on this path */
    CHECK(ledger_reserved("eth:T1", &cap) == 480000 && cap == 500000);
    ledger_release("eth:T1", "SIM-B");
    CHECK(ledger_reserved("eth:T1", &cap) == 0);

    /* no leak: reserve+release N times returns the path to 0. */
    for (int i = 0; i < 100; i++) {
        CHECK(ledger_reserve("eth:T2", "loop", 1000, 1000000, &reserved, &cap, &avail));
        ledger_release("eth:T2", "loop");
    }
    CHECK(ledger_reserved("eth:T2", &cap) == 0);

    /* >=8 concurrent sessions admit under a realistic segment budget (8*384000 = 3.07 MB/s
     * vs 110 MB/s) — confirms admission never caps the session COUNT, only genuine bandwidth. */
    char key[16];
    for (int i = 0; i < 8; i++) {
        snprintf(key, sizeof key, "S%d", i);
        CHECK(ledger_reserve("eth:8", key, 384000, 110ull * 1024 * 1024, &reserved, &cap, &avail));
    }
    CHECK(ledger_reserved("eth:8", &cap) == 8ull * 384000);
    for (int i = 0; i < 8; i++) {
        snprintf(key, sizeof key, "S%d", i);
        ledger_release("eth:8", key);
    }
    CHECK(ledger_reserved("eth:8", &cap) == 0);

    /* concurrency: N threads each reserve+release a DISTINCT key on the SAME path many times.
     * After the join the path must be EXACTLY 0 — a dropped lock_guard races the std::map and
     * TSan flags it; a torn read/write would leave a nonzero residue here even without TSan. */
    {
        const int NT = 8;
        std::thread th[NT];
        for (int t = 0; t < NT; t++)
            th[t] = std::thread([t]() {
                char k[16];
                snprintf(k, sizeof k, "T%d", t);
                uint64_t r, c, a;
                for (int i = 0; i < 2000; i++) {
                    ledger_reserve("eth:mt", k, 1000, 100000000, &r, &c, &a);
                    ledger_release("eth:mt", k);
                }
            });
        for (int t = 0; t < NT; t++) th[t].join();
        uint64_t c2 = 0;
        CHECK(ledger_reserved("eth:mt", &c2) == 0);
    }
}

/* §15.3 med-bundle-key-reconnect: an EthTransport-synthesized "eth:<peer>:<port>" key-5 serial is
 * NOT a USB selection target — bundleWantedSerial must return "" so an eth bundle maps to a FRESH
 * runtime + selectDevice falls through to mDNS discovery (not an impossible USB bind that loops).
 * A real USB serial still returns as the target. */
static void test_eth_bundle_not_usb_target() {
    auto wanted = [](const char *serial) {
        harp_cbuf b;
        harp_cbuf_init(&b);
        harp_cbor_map(&b, 1);  /* top map: 1 entry */
        harp_cbor_uint(&b, 5); /* key 5: usb-identity (the selection key) */
        harp_cbor_map(&b, 1);  /* sub-map: 1 entry */
        harp_cbor_uint(&b, 2); /* sub-key 2: serial */
        harp_cbor_text(&b, serial);
        std::string r = HarpRuntime::bundleWantedSerial(b.buf, b.len);
        harp_cbuf_free(&b);
        return r;
    };
    CHECK(wanted("eth:127.0.0.1:47990").empty()); /* eth serial -> NOT a USB target */
    CHECK(wanted("PI4B-0001") == "PI4B-0001");     /* a real USB serial -> the target */
}

int main() {
    /* Hermetic store: keep the ctor's harp_store_open out of the real cache.
     * POSIX only; on Windows the default store dir is used (harmless — the
     * store degrades gracefully and this test writes no meaningful objects). */
#ifndef _WIN32
    char tmpl[] = "/tmp/harp-registry-test.XXXXXX";
    char *home = mkdtemp(tmpl);
    if (home) setenv("HOME", home, 1);
#endif

    test_same_serial_shares_one_runtime();
    test_different_serials_distinct_runtimes();
    test_empty_serial_always_fresh();
    test_release_of_zeroed_handle_is_noop();
    test_admission_ledger();
    test_eth_bundle_not_usb_target();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
