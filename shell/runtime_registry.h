/* Runtime registry (P4) — process-global sharing of a HarpRuntime by the
 * EXPLICIT wanted serial, so several plugin/host instances that target the
 * SAME physical HARP unit share ONE runtime, ONE USB session, ONE claim.
 *
 * WHY: a HARP device is a single transport client — exactly one claim per
 * unit. Two instances each opening their own runtime against the same unit
 * is a claim conflict; the second loses. Multitimbral aliasing (P5: several
 * shells, one device, one part each) therefore needs the shells to ride a
 * single shared session. P4 lays that prerequisite: the table that lets a
 * second instance ATTACH to the first's runtime instead of fighting for the
 * device.
 *
 * THIS IS NOT THE OLD GLOBAL SINGLETON. The retired singleton handed device
 * #1 to EVERY instance — it broke #16 multi-device (two tracks, two units).
 * The registry shares a runtime ONLY between instances that EXPLICITLY ask
 * for the SAME serial. An instance that does not name a serial (wantSerial
 * == "") is given its OWN fresh, UNREGISTERED runtime — never shared, never
 * looked up — so the auto-select / single-instance path is byte-identical to
 * a runtime owned by value (the golden-render gate). Two instances on
 * DIFFERENT serials likewise get different runtimes and bind different units.
 *
 * SELECTION IS UNCHANGED. The registry keys on the serial the CALLER hands
 * it; it does no device selection of its own. The owner runtime still runs
 * the exact HarpRuntime::selectDevice() policy (HARP_DEVICE_SERIAL / bundle
 * usb-identity / first-unclaimed). The registry only decides share-vs-fresh.
 */
#pragma once

#include <string>

class HarpRuntime;

/* A reference to a runtime obtained from the registry.
 *   rt    — the runtime to drive (owner) or merely observe (attached).
 *   owner — true: THIS handle created the runtime and drives it exactly as a
 *           by-value runtime does today (configure / pull audio / queue
 *           events / get+set state). false: an ATTACHED handle — the shared
 *           session is already streaming under the owner; this handle must
 *           NOT touch the runtime's SPSC audio ring or event queue (single-
 *           producer/single-consumer invariants), only read shared state.
 * A default-constructed handle (rt == nullptr) is the "released / never
 * acquired" state; release() on it is a no-op. */
struct RuntimeHandle {
    HarpRuntime *rt = nullptr;
    bool owner = false;
};

/* Acquire a runtime for `wantSerial`.
 *   wantSerial == ""        -> ALWAYS a fresh, unregistered runtime (owner=true,
 *                              never shared) — the auto-select / single-instance
 *                              path, identical to owning a HarpRuntime by value.
 *   wantSerial present in table -> attach: bump the refcount, return the
 *                              EXISTING runtime (owner=false).
 *   wantSerial absent from table -> create a runtime, register it under the
 *                              serial, return it (owner=true).
 * Thread-safe: called from multiple plugin-instance threads. The OWNER caller
 * is responsible for configure()/start() — the registry does not start the
 * runtime, so the owner's first-bind path stays byte-identical to today. */
RuntimeHandle runtime_acquire(const std::string &wantSerial);

/* Release a handle. refcount--; the LAST release of a registered (shared)
 * runtime stops() and destroys it and removes it from the table. Releasing an
 * owner of an UNREGISTERED (empty-serial) runtime stops+destroys it directly.
 * Idempotent on a default/zeroed handle. Thread-safe. Calling stop() joins the
 * supervisor/reader/pump threads, exactly as ~HarpRuntime would. */
void runtime_release(const RuntimeHandle &h);

/* P4 LIMITATION (owner handoff): only the OWNER drives the shared session;
 * ATTACHED handles are dormant. If the owner releases while an attached sibling
 * is still alive, the runtime stays alive (refcount > 0) but undriven — the
 * session quiesces (no pull -> the feeder stops pacing -> the device idles), no
 * crash, and it is torn down at the last release. In practice instances of one
 * project activate/deactivate together, so this is an edge. P5 (where attached
 * instances become per-part drivers) introduces real owner handoff. */
