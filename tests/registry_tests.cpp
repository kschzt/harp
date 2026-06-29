/* Unit tests for shell/runtime_registry.*: the runtime construction seam +
 * the §8.4 admission ledger. (The process-global SHARING registry this TU once
 * held — owner+attached refcount keyed by serial — is RETIRED: one multi-out
 * main instance per device claim, so every runtime is private and there is
 * nothing to share. Device selection that keeps two units apart lives in
 * HarpRuntime::selectDevice(), not a table.)
 *
 * NO DEVICE NEEDED. runtime_acquire() never calls start() — it just hands back
 * a fresh object the owner drives. A HarpRuntime that is never started touches
 * no USB and spawns no threads: the ctor only opens the on-disk object store
 * (degrades gracefully), and stop()/dtor early-return when running_ was never
 * set. So acquire exercises construction against real HarpRuntime objects with
 * zero hardware. We point HOME at a temp dir so the store-open is hermetic.
 *
 * What it asserts:
 *   - runtime_acquire() -> a fresh, non-null, PRIVATE runtime every call; two
 *     acquires never alias; the unique_ptr dtor tears each down independently.
 *   - §8.4 ledger: cross-session aggregation, refuse-with-budget, exact release,
 *     re-negotiation idempotency, no leak, >=8 sessions, concurrency.
 *   - §15.3: an eth-synthesized key-5 serial is NOT a USB selection target.
 */
#include <cstdio>
#include <memory> /* std::unique_ptr<HarpRuntime> (the construction seam) */
#include <thread> /* §8.4 admission ledger concurrency sub-test */
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h> /* mkdtemp, setenv — POSIX only */
#endif

#include "runtime_registry.h"
#include "runtime.h"   /* HarpRuntime::bundleWantedSerial (public static) */
#include "harp/cbor.h" /* build a minimal §15.3 bundle for the eth-serial selection test */

#include "check.h"

/* runtime_acquire(): always a fresh, PRIVATE runtime — no table, no refcount, no
 * sharing. Two acquires must NOT alias (both LIVE here, so distinct addresses is
 * sound); resetting one tears it down independently of the other (the unique_ptr
 * owns its runtime's lifetime). This is the byte-identical golden / multi-device
 * path — and now the only path. */
static void test_acquire_is_private_and_fresh() {
    std::unique_ptr<HarpRuntime> a = runtime_acquire();
    std::unique_ptr<HarpRuntime> b = runtime_acquire();
    CHECK(a != nullptr && b != nullptr);
    CHECK(a.get() != b.get()); /* never shared, even back-to-back */

    a.reset(); /* tears down one private runtime; the other is undisturbed */
    std::unique_ptr<HarpRuntime> c = runtime_acquire();
    CHECK(c != nullptr);
    CHECK(c.get() != b.get()); /* still a distinct, private runtime */
    /* b and c are torn down by the unique_ptr dtor at scope end (~HarpRuntime). */
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

    test_acquire_is_private_and_fresh();
    test_admission_ledger();
    test_eth_bundle_not_usb_target();

    return check_report(NULL);
}
