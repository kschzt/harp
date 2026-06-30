/* shell/runtime_diag.cpp — §14.4 host-context diag-bundle assembly.
 *
 * Extracted VERBATIM from runtime.cpp (a pure translation-unit split — no
 * behavior change; the diag-bundle bytes stay identical). Holds the HarpRuntime
 * diag-bundle producers — emitClockStats / emitUsbTopology / emitNetTopology /
 * getDiagBundle — plus the file-local §16 anon_device_section helper they share.
 * All member declarations already live in runtime.h, so this is purely a move:
 * the same code, compiled into its own object and linked into every shell target.
 */
#include "runtime.h"
#include "runtime_registry.h" /* §8.4 ledger_reserved (path-utilization key 14) */
#include "freerun.h"          /* HARP_ASRC_QUALITY (asrc-stats key 4) */

#include <cmath>   /* llround: est_ppm -> ppb (clock-stats key 0) */
#include <cstring> /* memcpy: bit-cast the snapshot atomics (ratio/uncertainty) */
#include <ctime>   /* time: bundle-meta tstamp (key 2) */
#include <mutex>   /* std::lock_guard over ctlMutex_ */
#include <string>
#include <vector>

/* §14.4 diag-bundle / §16 anonymization. DECODE the device-section map
 * { 0 => identity, 1 => counters } and RE-ENCODE it into `out`, clearing the
 * identity PII leaves to "" IN PLACE while preserving everything else byte-for-
 * meaning. This MIRRORS host/harp-probe.c:anonymize_device_section (the
 * authoritative §16 leaf list) so the host's two diag-bundle producers (the
 * out-of-process harp-probe and the in-shell runtime) clear the SAME leaves
 * identically. Cleared: identity key 2 (serial); vendor (key 0)/product (key 1)
 * sub-map key 1 (name); identity key 9 (build-id); channel-map (key 7) per-entry
 * keys 2/3/4 (name/group/path). PRESERVED verbatim: vid/pid, firmware, engine
 * (incl. engine-id + param-map-hash), protocol, latency-profile, boot count,
 * ump-group-map, part count, caps, per-entry slot/direction/host-paced flag, the
 * array length/order, and the whole counters map (no PII). Subtrees needing no
 * edit are copied via harp_cdec_span (byte-for-byte). Returns false on a
 * malformed section so the caller can fall back. */
static bool anon_device_section(harp_cbuf *out, const uint8_t *sec, size_t len) {
    harp_cdec d;
    harp_cdec_init(&d, sec, len);
    uint64_t nsec;
    if (!harp_cdec_map(&d, &nsec)) return false;
    harp_cbor_map(out, nsec);
    for (uint64_t i = 0; i < nsec; i++) {
        uint64_t key;
        if (!harp_cdec_uint(&d, &key)) return false;
        harp_cbor_uint(out, key);
        if (key != 0) { /* counters (key 1) + any future member: no PII, verbatim */
            const uint8_t *span;
            size_t sl;
            if (!harp_cdec_span(&d, &span, &sl)) return false;
            harp_cbuf_put(out, span, sl);
            continue;
        }
        /* key 0 => identity: re-encode, clearing serial + vendor/product names. */
        uint64_t nid;
        if (!harp_cdec_map(&d, &nid)) return false;
        harp_cbor_map(out, nid);
        for (uint64_t j = 0; j < nid; j++) {
            uint64_t ik;
            if (!harp_cdec_uint(&d, &ik)) return false;
            harp_cbor_uint(out, ik);
            if (ik == 0 || ik == 1) { /* vendor/product { 0 => id, 1 => name } */
                uint64_t nsub;
                if (!harp_cdec_map(&d, &nsub)) return false;
                harp_cbor_map(out, nsub);
                for (uint64_t s = 0; s < nsub; s++) {
                    uint64_t sk;
                    if (!harp_cdec_uint(&d, &sk)) return false;
                    harp_cbor_uint(out, sk);
                    if (sk == 1) {
                        if (!harp_cdec_skip(&d)) return false; /* drop the name */
                        harp_cbor_text(out, "");               /* "" in place (§16) */
                    } else {
                        const uint8_t *span;
                        size_t sl;
                        if (!harp_cdec_span(&d, &span, &sl)) return false;
                        harp_cbuf_put(out, span, sl);
                    }
                }
            } else if (ik == 2 || ik == 9) { /* serial / build-id -> "" (§16) */
                if (!harp_cdec_skip(&d)) return false;
                harp_cbor_text(out, "");
            } else if (ik == 7) { /* channel-map: clear per-entry name/group/path */
                uint64_t nent;
                if (!harp_cdec_array(&d, &nent)) return false;
                harp_cbor_array(out, nent);
                for (uint64_t en = 0; en < nent; en++) {
                    uint64_t nek;
                    if (!harp_cdec_map(&d, &nek)) return false;
                    harp_cbor_map(out, nek);
                    for (uint64_t ek = 0; ek < nek; ek++) {
                        uint64_t ekey;
                        if (!harp_cdec_uint(&d, &ekey)) return false;
                        harp_cbor_uint(out, ekey);
                        if (ekey == 2 || ekey == 3 || ekey == 4) {
                            if (!harp_cdec_skip(&d)) return false; /* name/group/path */
                            harp_cbor_text(out, "");               /* "" in place */
                        } else {
                            const uint8_t *span;
                            size_t sl;
                            if (!harp_cdec_span(&d, &span, &sl)) return false;
                            harp_cbuf_put(out, span, sl);
                        }
                    }
                }
            } else { /* fw/engine/protocol/latency/boot/ump-map/parts: verbatim */
                const uint8_t *span;
                size_t sl;
                if (!harp_cdec_span(&d, &span, &sl)) return false;
                harp_cbuf_put(out, span, sl);
            }
        }
    }
    return !d.err;
}

/* §14.4 host-context-C: clock-stats (top key 11). ALWAYS emitted. Deterministic
 * CBOR per the design CDDL: { 0 => clock_drift_ppb, 3 => clock-recovery, ?4 =>
 * reanchors, ?5 => asrc-stats (iff recovery==asrc), ?6 => ratelock-stats (iff
 * recovery==rate-lock) }. NO PII — the §16 pass does not touch it; it is a pure
 * numeric snapshot of the runtime's recovery state. The recovery enum (CDDL
 * clock-recovery) is decided exactly as bitExact_ is at sessionUp:
 *   host-paced (0): USB / not free-running — SSI-driven, no recovery.
 *   asrc (1):       free-running device with NO audio.rate-lock (host resamples).
 *   rate-lock (2):  free-running + rate-lock (bit-exact; the feeder trims).
 * Called under ctlMutex_ from getDiagBundle (reads the snapshot atomics only). */
void HarpRuntime::emitClockStats(harp_cbuf *out) {
    bool freeRun = freeRunning_.load(std::memory_order_relaxed);
    /* recovery: 0 host-paced, 1 asrc, 2 rate-lock. */
    int recovery = !freeRun ? 0 : (bitExact_ ? 2 : 1);
    bool haveAsrc = (recovery == 1) && asrcLive_.load(std::memory_order_relaxed);
    bool haveRatelock = (recovery == 2);
    /* host-measured drift gauge (key 0): the ASRC recovers it from the RTP
     * timestamps (est_ppm -> ppb); rate-lock/host-paced have no host estimate -> 0. */
    int64_t driftPpb = haveAsrc
        ? (int64_t)llround(asrcEstPpm_.load(std::memory_order_relaxed) * 1000.0)
        : 0;
    /* §7.2 correlation-uncertainty MUST (re-audit HIGH #9): the spec requires the
     * recovered correlation (the drift, key 0) to be exposed WITH its CURRENT
     * uncertainty (key 2, "offset uncertainty (1-sigma band), µs"). The ASRC reader
     * recovers the rate by regressing dev_ts against host arrival time; the RMS of
     * that regression's residual, expressed as arrival-time error (freerun
     * jitter_us), IS the 1-sigma uncertainty of the recovered correlation — the
     * floor the recovery averages down. It was a DEAD STORE before this (published
     * to asrcJitterBits_ each drain, never read); reading it here satisfies the MUST
     * with a real host-measured value, not a constant. Only meaningful on the ASRC
     * path (it owns the regression); rate-lock/host-paced have no host-side fit, so
     * key 2 is absent there (the CDDL marks it optional). */
    double driftUncertaintyUs = 0.0;
    if (haveAsrc) {
        uint64_t jb = asrcJitterBits_.load(std::memory_order_relaxed);
        memcpy(&driftUncertaintyUs, &jb, sizeof driftUncertaintyUs);
    }
    uint64_t nkeys = 2; /* key 0 (drift) + key 3 (recovery) always */
    if (haveAsrc) nkeys += 3;              /* key 2 (uncertainty) + key 4 (reanchors) + key 5 (asrc-stats) */
    else if (haveRatelock) nkeys++;        /* key 6 (ratelock-stats) */
    /* NB: rtp_loss is NOT a clock-stats key — clock-stats key 7 is reserved for ptp-stats
     * (a map). RTP loss lives at host-counters key 8; reanchors mirror at clock-stats key 4. */
    harp_cbor_uint(out, 11);
    harp_cbor_map(out, nkeys);
    harp_cbor_uint(out, 0);
    harp_cbor_int(out, driftPpb);            /* clock_drift_ppb (host-measured gauge) */
    if (haveAsrc) { /* §7.2 key 2: 1-sigma uncertainty of the recovered correlation, µs */
        harp_cbor_uint(out, 2);
        harp_cbor_float(out, driftUncertaintyUs);
    }
    harp_cbor_uint(out, 3);
    harp_cbor_uint(out, (uint64_t)recovery); /* clock-recovery enum */
    if (haveAsrc) {
        harp_cbor_uint(out, 4); /* §8.3 stream re-anchors (starvation episodes) — never silent */
        harp_cbor_uint(out, asrcReanchors_.load(std::memory_order_relaxed));
        /* asrc-stats (CDDL): { 0 => ratio, ?3 => phase/fill error vs setpoint, ?4 =>
         * converter quality }. The ratio is the recovered out/in; the fill error is
         * the signed frame deviation from the setpoint (the loop's phase). Quality is
         * the reader's HARP_ASRC_QUALITY == SRC_SINC_BEST_QUALITY (0; ~145 dB, clearing the §8.3
         * >=120 dB floor). (Keys 1/2 — in/out totals — are
         * not snapshotted by the freerun core, so a vN writer omits them.) */
        uint64_t rb = asrcRatioBits_.load(std::memory_order_relaxed);
        double ratio; memcpy(&ratio, &rb, sizeof ratio);
        double fillErr = (double)asrcFill_.load(std::memory_order_relaxed) -
                         (double)ethTargetFrames();
        harp_cbor_uint(out, 5);
        harp_cbor_map(out, 3); /* keys 0,3,4 */
        harp_cbor_uint(out, 0);
        harp_cbor_float(out, ratio);
        harp_cbor_uint(out, 3);
        harp_cbor_float(out, fillErr);
        harp_cbor_uint(out, 4);
        harp_cbor_uint(out, (uint64_t)HARP_ASRC_QUALITY /* §8.3 >=120 dB */);
    } else if (haveRatelock) {
        /* ratelock-stats: { 0 => last audio.trim ppb, 1 => fill, 2 => setpoint,
         * 3 => trim messages sent this session }. */
        harp_cbor_uint(out, 6);
        harp_cbor_map(out, 4);
        harp_cbor_uint(out, 0);
        harp_cbor_int(out, lastTrimPpb_.load(std::memory_order_relaxed));
        harp_cbor_uint(out, 1);
        harp_cbor_uint(out, (uint64_t)(audioRing_.readAvailable() / 2)); /* current fill, frames */
        harp_cbor_uint(out, 2);
        harp_cbor_uint(out, ethTargetFrames());                          /* setpoint, frames */
        harp_cbor_uint(out, 3);
        harp_cbor_uint(out, trimCount_.load(std::memory_order_relaxed));
    }
}

/* §14.4 host-context-C: usb-topology (top key 10), USB binding ONLY. Reads the
 * libusb topology of the bound device off the control path (transport_->usbTopology
 * -> host/usb_io.c). Returns false (emitting NOTHING) when the binding is not USB
 * or libusb could not resolve the device, so the assembler can size its map. §16:
 * with anonymize the controller-id (key 0) + serial (key 8) are cleared to "" IN
 * PLACE; bus/addr/port-chain/speed/VID/PID are RETAINED. CDDL usb-topology:
 * { 0 controller, 1 bus, 2 addr, 3 [port-chain], 4 speed, 6 VID, 7 PID, 8 serial }.
 * The caller pre-fetches the topology (so presence can size the bundle map upfront,
 * for deterministic definite-length CBOR) and only calls this when t.ok. */
void HarpRuntime::emitUsbTopology(harp_cbuf *out, const harp_usb_topology &t,
                                  bool anonymize) {
    harp_cbor_uint(out, 10);
    harp_cbor_map(out, 8); /* keys 0,1,2,3,4,6,7,8 */
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, anonymize ? "" : t.controller); /* §16: controller id */
    harp_cbor_uint(out, 1);
    harp_cbor_uint(out, t.bus);
    harp_cbor_uint(out, 2);
    harp_cbor_uint(out, t.addr);
    harp_cbor_uint(out, 3);
    harp_cbor_array(out, (uint64_t)(t.nports > 0 ? t.nports : 0));
    for (int i = 0; i < t.nports; i++) harp_cbor_uint(out, t.ports[i]);
    harp_cbor_uint(out, 4);
    harp_cbor_uint(out, (uint64_t)t.speed); /* usb-speed enum */
    harp_cbor_uint(out, 6);
    harp_cbor_uint(out, t.vendor_id);       /* VID RETAINED */
    harp_cbor_uint(out, 7);
    harp_cbor_uint(out, t.product_id);      /* PID RETAINED */
    harp_cbor_uint(out, 8);
    harp_cbor_text(out, anonymize ? "" : t.serial); /* §16: serial */
}

/* §14.4 host-context-C: net-topology (top key 13), Ethernet binding ONLY. CDDL
 * net-topology: { ?0 host:port, ?1 mDNS, ?2 jitter-frames, ?3 net.ptp, ?4
 * net.ptp.hw, ?5 net.offline }. The host learned the dial target (key 0) and the
 * live ring occupancy (key 2 jitter depth); mDNS/PTP are device-announced and not
 * yet learned host-side, so keys 1/3/4 are omitted (a vN writer omits what it
 * cannot fill). §16: with anonymize host:port (key 0) is cleared to "" IN PLACE;
 * the jitter depth + net.offline flag are RETAINED. Returns false (emitting
 * NOTHING) on a non-Ethernet binding — the caller only calls it on Ethernet. */
void HarpRuntime::emitNetTopology(harp_cbuf *out, bool anonymize) {
    const char *hostport = transport_ ? transport_->netEndpoint() : "";
    bool offline = wantHostPaced_.load(std::memory_order_relaxed);
    harp_cbor_uint(out, 13);
    harp_cbor_map(out, 3); /* keys 0,2,5 */
    harp_cbor_uint(out, 0);
    harp_cbor_text(out, anonymize ? "" : (hostport ? hostport : "")); /* §16: host:port */
    harp_cbor_uint(out, 2);
    harp_cbor_uint(out, ethTargetFrames()); /* RTP jitter-buffer depth (setpoint), frames */
    harp_cbor_uint(out, 5);
    harp_cbor_bool(out, offline);           /* net.offline (host-paced TCP bounce) RETAINED */
}

/* §14.4 host-context-A — see the runtime.h doc. MIRRORS getStateBundle: a
 * deterministic CBOR map assembled under ctlMutex_, off the control path, with
 * no audio-path side effects (golden gate). */
std::vector<uint8_t> HarpRuntime::getDiagBundle(bool anonymize) {
    /* Serialize with feeder/control ops (the request() the device-section needs
     * is always issued under ctlMutex_, like every other control op). */
    std::lock_guard<std::mutex> lk(ctlMutex_);

    /* Fetch the device-section FIRST (its bytes are embedded verbatim at key 4):
     * issue `req diag.bundle` and keep the response body. A failure (device gone,
     * or no diag.bundle cap) leaves an empty section — the bundle stays valid. */
    harp_cbuf devReq, devRsp;
    harp_cbuf_init(&devReq);
    harp_cbuf_init(&devRsp);
    harp_client_req_head(&client_, &devReq, "diag.bundle", false); /* no body */
    harp_env de = {};
    bool haveDev = connected() && request(&devReq, &devRsp, &de) && de.has_body;

    /* §14.4 host-context-B: snapshot the instrumentation rings under ctlMutex_
     * (control-path read; both snapshots are non-destructive, so a later bundle
     * re-observes the same recent window). Done BEFORE the map header so the
     * key 6 / key 8 presence can size the definite-length map deterministically. */
    std::vector<StateTransition> history = sessionHistory_.snapshot(kDiagHistoryMax);
    std::vector<LogRecord> logs = runtimeLog_.snapshot(kDiagLogMax);

    harp_cbuf out;
    harp_cbuf_init(&out);

    /* §14.4 host-context-C: pre-resolve the binding-conditional sections so the
     * top map can size upfront (definite length, deterministic CBOR). key 11
     * (clock-stats) is ALWAYS present. key 10 (usb-topology) is present iff the
     * binding is USB AND libusb resolved the topology; key 13 (net-topology) iff
     * the binding is Ethernet. The two are mutually exclusive (a binding is one or
     * the other). On the §8.7 loopback the binding is Ethernet, so the test sees
     * key 13 + key 11 and NOT key 10. usbTopology() is fetched ONCE here and reused
     * by emitUsbTopology — a read-only libusb query, off the control path. */
    harp_usb_topology usbTopo;
    usbTopo.ok = false;
    bool isEth = transport_ && transport_->kind() == ShellTransport::Kind::Ethernet;
    bool haveUsbTopo = transport_ &&
                       transport_->kind() == ShellTransport::Kind::Usb &&
                       transport_->usbTopology(&usbTopo) && usbTopo.ok;

    /* §8.4 path-utilization (key 14): present whenever bound — this session's transport
     * path id, computed exactly like audioStart's admission key (the live reservation's
     * cached path if streaming, else the current binding's). Its reserved/capacity come
     * from the process-global admission ledger below. */
    bool haveUtil = transport_ != nullptr;
    std::string utilPath;
    if (haveUtil) {
        if (admittedBps_ && !admittedPath_.empty())
            utilPath = admittedPath_;
        else if (transport_->kind() == ShellTransport::Kind::Usb)
            utilPath = std::string("usb:") + (usbTopo.ok ? usbTopo.controller : "unknown");
        else
            utilPath = "eth:global";
    }

    /* Top-level diag-bundle map. v3 fills keys 0-6, 8, 9, 11 + (10 USB | 13 Eth);
     * key 7/12 (loopback-results) is a later sub-step (a vN writer omits unfilled
     * sections). key 9 is present only when a session is up (audio-config is
     * meaningless otherwise); keys 6 (session-history) + 8 (runtime logs) are
     * present only when their ring has records. The map size is declared upfront
     * (definite length, deterministic CBOR). */
    bool haveAudio = connected_.load(std::memory_order_acquire);
    uint64_t nkeys = 6; /* 0,1,2,3,4,5 */
    if (!history.empty()) nkeys++; /* key 6 */
    if (!logs.empty()) nkeys++;    /* key 8 */
    if (haveAudio) nkeys++;        /* key 9 */
    nkeys++;                       /* key 11 clock-stats (ALWAYS) */
    if (haveUsbTopo) nkeys++;      /* key 10 usb-topology (USB binding) */
    if (isEth) nkeys++;            /* key 13 net-topology (Ethernet binding) */
    if (haveUtil) nkeys++;         /* key 14 §8.4 path-utilization (any binding) */
    harp_cbor_map(&out, nkeys);

    /* KEY 0: magic. */
    harp_cbor_uint(&out, 0);
    harp_cbor_text(&out, "harpd");

    /* KEY 1: version. v2 added session-history (key 6) + runtime logs (key 8); v3
     * adds host-context-C: clock-stats (key 11, always) + usb-topology (key 10,
     * USB) | net-topology (key 13, Ethernet) + transport enum (audio-config key 12). */
    harp_cbor_uint(&out, 1);
    harp_cbor_uint(&out, 4); /* v4: §8.4 path-utilization (top key 14) */

    /* KEY 2: bundle-meta { 0 => tstamp [epoch, msc], 1 => tool }. */
    harp_cbor_uint(&out, 2);
    harp_cbor_map(&out, 2);
    harp_cbor_uint(&out, 0);
    harp_cbor_array(&out, 2);
    harp_cbor_uint(&out, (uint64_t)time(nullptr)); /* epoch seconds */
    /* current MSC if streaming, else 0 — streamPos() is the stream-domain SSI. */
    harp_cbor_uint(&out, haveAudio ? streamPos() : 0);
    harp_cbor_uint(&out, 1);
    harp_cbor_text(&out, "harp-runtime (device-assembled device-section)");

    /* KEY 3: anonymized flag. */
    harp_cbor_uint(&out, 3);
    harp_cbor_bool(&out, anonymize);

    /* KEY 4: device-section. Non-anon: the device's response body VERBATIM (the
     * byte-identical conformance gate). Anon: decode-reencode with §16 leaves
     * cleared (the seam exception — NOT kept verbatim). harp_cbuf_put COPIES the
     * bytes, so de.body can be freed after this. */
    harp_cbor_uint(&out, 4);
    if (haveDev) {
        if (anonymize) {
            if (!anon_device_section(&out, de.body, de.body_len))
                harp_cbor_map(&out, 0); /* malformed -> empty (still valid CBOR) */
        } else {
            harp_cbuf_put(&out, de.body, de.body_len);
        }
    } else {
        harp_cbor_map(&out, 0); /* device not ready / no cap -> empty device-section */
    }
    harp_cbuf_free(&devReq);
    harp_cbuf_free(&devRsp);

    /* KEY 5: host-counters (keys 0-8). Numeric leaves — no PII, unchanged by the
     * anon pass. All read here under ctlMutex_. (Keys 7/8 = §8.3 stream_reanchors /
     * §8.7 rtp_loss — the canonical host-section homes per docs/diag-bundle-design.md;
     * clock-stats key 4 mirrors reanchors, key 7 there is reserved for ptp-stats.) */
    harp_cbor_uint(&out, 5);
    harp_cbor_map(&out, 9);
    harp_cbor_uint(&out, 0);
    harp_cbor_uint(&out, underruns_.load(std::memory_order_relaxed)); /* host_underruns */
    harp_cbor_uint(&out, 1);
    harp_cbor_uint(&out, padSamples_.load(std::memory_order_relaxed)); /* pad_debt_samples */
    harp_cbor_uint(&out, 2);
    harp_cbor_uint(&out, evDrops_.load(std::memory_order_relaxed)); /* event_drops */
    harp_cbor_uint(&out, 3);
    harp_cbor_uint(&out, framesSent_); /* frames_sent (audio-thread member, read off-path) */
    harp_cbor_uint(&out, 4);
    harp_cbor_uint(&out, framesRecvAtomic_.load(std::memory_order_relaxed)); /* frames_recv */
    harp_cbor_uint(&out, 5);
    harp_cbor_uint(&out, sessionGen_.load(std::memory_order_relaxed)); /* session_generation */
    harp_cbor_uint(&out, 6);
    harp_cbor_uint(&out, renegCount_.load(std::memory_order_acquire)); /* audio_renegotiations */
    harp_cbor_uint(&out, 7);
    harp_cbor_uint(&out, asrcReanchors_.load(std::memory_order_relaxed)); /* §8.3 stream_reanchors */
    harp_cbor_uint(&out, 8);
    harp_cbor_uint(&out, rtpLostSnap_.load(std::memory_order_relaxed)); /* §8.7 rtp_loss (RTP seq gaps) */

    /* KEY 6: session-history — the §12.1 state-machine transition ring. Each
     * record is { 0 => [epoch, msc], 1 => from-state, 2 => to-state, 3 =>
     * reason, ?4 => detail }. §16: with anonymize the free-text detail (key 4)
     * is cleared to "" IN PLACE — the numeric tstamp/from/to/reason are RETAINED
     * (reveal whether, not what). Emitted only when the ring has records. */
    if (!history.empty()) {
        harp_cbor_uint(&out, 6);
        harp_cbor_array(&out, history.size());
        for (const StateTransition &t : history) {
            harp_cbor_map(&out, 5); /* keys 0,1,2,3,4 — detail always present (""=redacted) */
            harp_cbor_uint(&out, 0);
            harp_cbor_array(&out, 2);
            harp_cbor_uint(&out, t.tstamp_epoch);
            harp_cbor_uint(&out, t.tstamp_msc);
            harp_cbor_uint(&out, 1);
            harp_cbor_uint(&out, t.from_state);
            harp_cbor_uint(&out, 2);
            harp_cbor_uint(&out, t.to_state);
            harp_cbor_uint(&out, 3);
            harp_cbor_uint(&out, t.reason_code);
            harp_cbor_uint(&out, 4);
            harp_cbor_text(&out, anonymize ? "" : t.detail); /* §16: clear detail in place */
        }
    }

    /* KEY 8: runtime logs — the §14.4 RuntimeLog ring. Each record is { 0 =>
     * msc, 1 => level, 2 => tag, 3 => msg, ?4 => [epoch, msc] wall-stamp }. §16:
     * with anonymize the free-text msg (key 3) is cleared to "" IN PLACE; the
     * tag (key 2), level, msc and wall-stamp are RETAINED. Emitted only when the
     * ring has records. */
    if (!logs.empty()) {
        harp_cbor_uint(&out, 8);
        harp_cbor_array(&out, logs.size());
        for (const LogRecord &l : logs) {
            harp_cbor_map(&out, 5); /* keys 0,1,2,3,4 */
            harp_cbor_uint(&out, 0);
            harp_cbor_uint(&out, l.msc);
            harp_cbor_uint(&out, 1);
            harp_cbor_uint(&out, l.level);
            harp_cbor_uint(&out, 2);
            harp_cbor_text(&out, l.tag); /* §16: tag RETAINED (reveal whether, not what) */
            harp_cbor_uint(&out, 3);
            harp_cbor_text(&out, anonymize ? "" : l.msg); /* §16: clear msg in place */
            harp_cbor_uint(&out, 4);
            harp_cbor_array(&out, 2); /* tstamp [epoch, msc] (wall-clock correlation) */
            harp_cbor_uint(&out, l.tstamp_epoch);
            harp_cbor_uint(&out, l.msc);
        }
    }

    /* KEY 9: audio-config (host-owned DAW view; keys 0-12). Only when a session
     * is up (otherwise these read defaults that misdescribe a dead session). v3
     * adds key 12 (transport enum: 0 usb, 1 ethernet) — the EXPLICIT binding
     * selector so a decoder can tell a USB v3 bundle from an Ethernet one (the v2
     * ambiguity the design flagged); it pairs with key 10 (USB) vs key 13 (Eth). */
    if (haveAudio) {
        harp_cbor_uint(&out, 9);
        harp_cbor_map(&out, 13);
        harp_cbor_uint(&out, 0);
        harp_cbor_uint(&out, rate_); /* DAW sample rate */
        harp_cbor_uint(&out, 1);
        harp_cbor_uint(&out, kBlock); /* DAW pacing block (256) */
        harp_cbor_uint(&out, 2);
        harp_cbor_uint(&out, freeRunning_.load(std::memory_order_relaxed) ? 0 : 1); /* clock-mode */
        harp_cbor_uint(&out, 3); /* active out-slots (the audio.start union) */
        harp_cbor_array(&out, unionSlots_.size());
        for (uint32_t slot : unionSlots_) harp_cbor_uint(&out, slot);
        harp_cbor_uint(&out, 4); /* active in-slots: H->D only, empty */
        harp_cbor_array(&out, 0);
        harp_cbor_uint(&out, 5);
        harp_cbor_uint(&out, targetFrames_); /* target ring depth (frames) */
        harp_cbor_uint(&out, 6);
        harp_cbor_uint(&out, (uint64_t)latencySamples()); /* §6.4 DAW-domain PDC latency = buffer depth + event headroom; byte-identical to getLatencySamples() reported to the DAW */
        harp_cbor_uint(&out, 7);
        harp_cbor_bool(&out, bitExact_); /* 1:1 rate-lock vs ASRC */
        harp_cbor_uint(&out, 8);
        harp_cbor_bool(&out, freeRunning_.load(std::memory_order_relaxed)); /* free-running */
        harp_cbor_uint(&out, 9);
        harp_cbor_bool(&out, wantHostPaced_.load(std::memory_order_relaxed)); /* offline */
        harp_cbor_uint(&out, 10);
        harp_cbor_uint(&out, 0); /* sample format (0 = float32) */
        harp_cbor_uint(&out, 11);
        harp_cbor_uint(&out, unionWidth_.load(std::memory_order_relaxed)); /* RTP columns */
        harp_cbor_uint(&out, 12);
        harp_cbor_uint(&out, isEth ? 1u : 0u); /* transport enum: 0 usb, 1 ethernet (§4.4) */
    }

    /* KEY 10: usb-topology — USB binding ONLY (absent on the §8.7 Ethernet
     * loopback). Emitted from the topology fetched once above. §16 clears the
     * controller-id (0) + serial (8); bus/addr/port-chain/speed/VID/PID RETAINED. */
    if (haveUsbTopo) emitUsbTopology(&out, usbTopo, anonymize);

    /* KEY 11: clock-stats — ALWAYS. The host's recovery/correlation snapshot
     * (recovery mode + drift + asrc-stats|ratelock-stats). No PII (the §16 pass
     * leaves it untouched). MUST sit AFTER key 10 and BEFORE key 13 (integer keys
     * ascending — deterministic CBOR). */
    emitClockStats(&out);

    /* KEY 13: net-topology — Ethernet binding ONLY (the §8.7 loopback path). §16
     * clears host:port (key 0); the jitter depth + net.offline flag RETAINED. */
    if (isEth) emitNetTopology(&out, anonymize);

    /* KEY 14: §8.4 path-utilization — audio bandwidth reserved on THIS session's transport
     * path vs the path capacity, from the process-global admission ledger. The spec's
     * "per-controller utilization in the diagnostic bundle": on real USB the controller's
     * figures, on the refdev loopback the single eth:global path's. { 0 => path-id,
     * 1 => reserved B/s, 2 => capacity B/s, 3 => per-mille }. §16 clears the path-id (0);
     * the numeric gauges are retained (reveal whether/how-much, not the controller name). */
    if (haveUtil) {
        uint64_t ucap = 0; /* the capacity the path was last metered against — self-consistent
                            * with `reserved` (vs recomputing, which could diverge if a USB
                            * topology query transiently fails at diag time). */
        uint64_t ures = ledger_reserved(utilPath, &ucap);
        harp_cbor_uint(&out, 14);
        harp_cbor_map(&out, 4);
        harp_cbor_uint(&out, 0);
        harp_cbor_text(&out, anonymize ? "" : utilPath.c_str());
        harp_cbor_uint(&out, 1);
        harp_cbor_uint(&out, ures);
        harp_cbor_uint(&out, 2);
        harp_cbor_uint(&out, ucap);
        harp_cbor_uint(&out, 3);
        harp_cbor_uint(&out, ucap ? (ures * 1000 / ucap) : 0);
    }

    std::vector<uint8_t> result(out.buf, out.buf + out.len);
    harp_cbuf_free(&out);
    return result;
}
