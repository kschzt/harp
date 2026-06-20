# §14 Diagnostics — design + schema draft

diag.bundle (§14.4) + diag.loopback (§14.3). Produced by the `harp-s14-design` workflow.
The diag-bundle CDDL fills the Appendix A gap (§14.4 calls the schema normative there, but it was undefined).
**Status: DRAFT** — the open questions at the end must be frozen before spec adoption.

## Ownership
- **Device (eth-agent):** `diag.bundle` assembly (device-section), `diag.loopback.start/.stop` + engine loopback flag, capability adverts.
- **Host (us):** the bundle assembler/aggregator, host-context gather, §16 anonymizer, CBOR export, the round-trip measurement, the CI tests.

## Bundle schema (CDDL)
```cddl
; =====================================================================
; HARP Diagnostic Bundle (§14.4) — normative CDDL.
; Append to spec/harp.cddl after recall-bundle, and to Appendix A.
;
; Style/convention (load-bearing): mirrors recall-bundle EXACTLY —
;   magic string at key 0 ("harpd" vs recall's "harpb"),
;   version uint at key 1, integer keys throughout, definite lengths,
;   RFC 8949 deterministic encoding (core/src/cbor.c already guarantees
;   shortest-form + definite). Reuses the existing Appendix A rules
;   verbatim (identity, vendor, product, engine, channel-map,
;   latency-profile, tstamp, hash, semver) instead of redefining them,
;   so identity drift updates the bundle for free.
;
; OWNERSHIP SEAM (the device/host split):
;   key 4 device-section : 100% DEVICE-assembled (eth-agent). The device
;                          emits this exact map as the body of `rsp
;                          diag.bundle`. In the non-anonymized path the
;                          host stores it under key 4 VERBATIM (no
;                          re-encode); the byte-identical round-trip of
;                          device-section is the conformance gate both
;                          agents test against (see ANON note re: the one
;                          exception).
;   keys 5..15           : 100% HOST-assembled (host runtime). The device
;                          neither sees nor produces any of it.
; Top-level keys 0..3 are host-owned envelope/meta, placed first so a
; support tool knows the format, version, capture time, and PRIVACY STATE
; before it walks either section.
;
; V0-EXTENSIBILITY (incremental — keys are assigned, not all written from
; day one). A v0 reader skips unknown keys; a vN writer omits sections it
; cannot fill. Required-from-day-one set is keys 0,1,2,3,4 only:
;   v0 (harp-probe today): 0 magic, 1 version, 2 bundle-meta,
;       3 anonymized?, 4 device-section{identity + counters}.
;       (harp-probe assembles this from do_hello + req diag.counters.)
;   v1: + host-counters(5), audio-config(9).
;   v2: + logs-runtime(8), session-history(6).
;   v3: + usb-topology(10), clock-stats(11), loopback-results(12).
;   v4: + net-topology(13).
; =====================================================================

diag-bundle = {
    0  => "harpd",               ; magic (parallels recall-bundle "harpb")
    1  => uint,                  ; bundle schema version (this rev = 1)
    2  => bundle-meta,           ; [host] capture provenance (when/by-what)
    3  => bool,                  ; [host] anonymized? — true => §16 pass applied.
                                 ;   FIRST-class top-level flag so a reader knows
                                 ;   the privacy state before parsing leaves.
    4  => device-section,        ; [device] assembled by eth-agent, embedded verbatim
  ? 5  => host-counters,         ; [host] runtime-origin counters
  ? 6  => [* state-transition],  ; [host] session state-machine history (§12.1)
  ? 7  => [* log-record],        ; [host] RESERVED: device logs lifted to top level
                                 ;   IF a host chooses to merge timelines (normally
                                 ;   device logs live in device-section key 2).
  ? 8  => [* log-record],        ; [host] recent runtime logs
  ? 9  => audio-config,          ; [host] DAW-domain view of the active stream (§8.2/§8.3)
  ? 10 => usb-topology,          ; [host] USB topology as host sees it (§4.3)
  ? 11 => clock-stats,           ; [host] correlation / drift statistics (§7.3/§8.7)
  ? 12 => [* loopback-result],   ; [host] persisted §14.3 measurements per rate
  ? 13 => net-topology,          ; [host] Ethernet/IP binding context (§4.4/§8.7), if net
}

; --- capture metadata (host-owned) ---
bundle-meta = {
    0 => tstamp,                 ; capture wall-clock + host MSC: [epoch, msc-or-0]
  ? 1 => tstr,                   ; assembling tool + version, e.g. "harp-probe 0.4.0"
  ? 2 => tstr,                   ; host platform string (anonymized => "")
  ? 3 => semver,                 ; runtime/spec version
  ? 4 => uint,                   ; session generation captured (shell/runtime.h:723)
}

; =====================================================================
; DEVICE SECTION — the contract the eth-agent codes to.
; Every member is producible from data the device already owns/emits:
;   identity      <- encode_identity (device/session.c:176, keys 0..12)
;   counters      <- handle_diag_counters (device/session.c:688, the 16-pair map)
;   logs          <- §4.2 stream `log` ring buffer, drained at bundle time
;   audio-config  <- device's view of the active stream (audio.start rsp);
;                    absent when no stream is running
; A device with no live stream omits key 3 and MAY emit an empty log array.
; Nothing here depends on the host. The device returns EXACTLY this map as
; the body of `rsp diag.bundle`.
; =====================================================================
device-section = {
    0 => identity,               ; reuse Appendix A `identity` verbatim (§6.2)
    1 => counters,               ; the diag.counters map, embedded as-is (§14.2)
  ? 2 => [* log-record],         ; recent DEVICE logs from the `log` ring
  ? 3 => device-audio-config,    ; device's view of the stream, present iff active
}

; --- counters (§14.2): the EXACT keys the refdev emits today
;     (device/session.c:688-735), reused for device AND host counter sets.
;     Open map: named members are the mandatory minimum; clock_drift_ppb is
;     a signed gauge (int), all other mandatory counters are uint;
;     `x.`-prefixed vendor counters validate without enumeration.
;     temperature_centi_c is SHOULD (refdev does not emit it today). ---
counters = {
    "usb_errors"           => uint,
    "frame_errors"         => uint,
    "audio_underruns"      => uint,
    "audio_overruns"       => uint,
    "audio_late_frames"    => uint,
    "msc_discontinuities"  => uint,
    "clock_drift_ppb"      => int,    ; gauge (may be negative)
    "evt_late"             => uint,
    "evt_stale_epoch"      => uint,
    "session_resets"       => uint,
    "storage_bytes_total"  => uint,
    "storage_bytes_free"   => uint,
  ? "temperature_centi_c"  => int,    ; SHOULD, if sensed (§14.2)
    * vendor-counter-key   => uint / int    ; e.g. x.harp-refdev.fence_waits
}
vendor-counter-key = tstr .regexp "x\\..+"

; --- device's own view of the running stream (mirrors audio.start, §8.2) ---
device-audio-config = {
    0 => uint,                   ; sample rate, Hz
    1 => uint,                   ; nsamples per frame (epoch-constant, §8.2)
    2 => clock-mode,             ; clock mode in effect (§8.3)
    3 => [* uint],               ; active in-slots  (H->D), §6.3 slot indices
    4 => [* uint],               ; active out-slots (D->H)
    5 => uint,                   ; device-pipeline-samples (fixed engine+transport)
  ? 6 => uint,                   ; RTP prebuffer frames (§8.7), free-running only
  ? 7 => int,                    ; rate_trim_ppb currently applied (free-running)
  ? 8 => uint,                   ; device-side re-anchor count (§8.3)
  ? 9 => uint,                   ; current epoch (§8.6)
}
clock-mode = &( free-running: 0, host-paced: 1 )

; =====================================================================
; HOST SECTION (keys 5..13) — host's private business; eth-agent never
; touches it. Built from shell/runtime.* state + libusb + the time-
; correlation subsystem.
; =====================================================================

; --- host-origin counters (the runtime's own observations) ---
host-counters = {
  ? 0 => uint,        ; host_underruns        (audio-thread starvation, runtime.h:629)
  ? 1 => uint,        ; pad_debt_samples      (total silence padded, runtime.h:630)
  ? 2 => uint,        ; event_drops           (host event-ring overflow, runtime.h:631)
  ? 3 => uint,        ; frames_sent           (H->D frames, runtime.h:802)
  ? 4 => uint,        ; frames_recv           (D->H frames, runtime.h:802)
  ? 5 => uint,        ; session_generation    (bumped at sessionUp, runtime.h:723)
  ? 6 => uint,        ; audio_renegotiations  (re-neg due to late sink, runtime.h:737)
  ? 7 => uint,        ; stream_reanchors      (§8.3 host-side re-anchors; spec names this a host-section counter)
  ? 8 => uint,        ; rtp_loss              (RTP seq gaps, §8.7), free-running net only
  ? 9 => uint,        ; admission_failures    (§8.4 admission-control rejections)
    * vendor-counter-key => uint / int   ; host/vendor x.* extension counters
}

; --- session state-machine history (§12.1) ---
; DETACHED -> ATTACHED -> NEGOTIATED -> SYNCED -> STREAMING ; (detach/error -> DETACHED)
session-state = &(
    DETACHED: 0, ATTACHED: 1, NEGOTIATED: 2, SYNCED: 3, STREAMING: 4,
)
state-transition = {
    0 => tstamp,                 ; when (epoch, msc) — msc 0 if no stream yet
    1 => session-state,          ; from-state
    2 => session-state,          ; to-state
  ? 3 => transition-reason,      ; reason code
  ? 4 => tstr,                   ; free-text detail (anonymized => "")
}
; transition-reason enumerates the §12.2 mismatch cases and §11.4 four safe actions.
transition-reason = &(
    attach: 0, detach: 1, hello-ok: 2, serial-mismatch: 3,
    engine-major-mismatch: 4, param-map-hash-mismatch: 5,
    reconcile-push: 6, reconcile-pull: 7, reconcile-open-ro: 8,
    reconcile-duplicate-push: 9, audio-start: 10, audio-stop: 11,
    session-reset: 12, transport-error: 13,
)

; --- log record (§14.4 stream `log`): shared shape, device + runtime ---
; The §14.4 record uses INTEGER keys (0 msc, 1 level, 2 tag, 3 msg). This is
; one of two intentional integer-keyed-map exceptions inside the bundle (the
; other is `counters`, which is text-keyed to match the diag.counters wire
; format). Both faithfully transcribe the established wire shapes.
log-record = {
    0 => uint64,                 ; msc, or 0 if pre-stream / not audio-clocked
    1 => log-level,              ; severity
    2 => tstr,                   ; tag (stable, machine-greppable; NOT anonymized)
    3 => tstr,                   ; message free-text (anonymized => "", §16)
  ? 4 => tstamp,                 ; wall-clock correlation, if known (host runtime)
}
log-level = &( debug: 0, info: 1, warn: 2, error: 3 )

; --- audio configuration: the host / DAW-domain view (§8.2/§8.3) ---
audio-config = {
    0 => uint,                   ; DAW sample rate, Hz
    1 => uint,                   ; DAW block size, frames
    2 => clock-mode,             ; clock mode in effect (§8.3)
    3 => [* uint],               ; active out-slots (D->H), §6.3
    4 => [* uint],               ; active in-slots  (H->D)
    5 => uint,                   ; target ring / elastic-buffer depth, frames
    6 => uint,                   ; reported DAW-domain PDC latency, samples (§6.4)
  ? 7 => bool,                   ; bit-exact (1:1 rate-lock) vs ASRC (§8.7; runtime bitExact_)
  ? 8 => bool,                   ; free-running transport (Ethernet/RTP) vs host-paced
  ? 9 => bool,                   ; offline: §8.3 host-paced non-real-time bounce over TCP (§8.7)
  ? 10 => uint,                  ; sample format: 0 float32, 1 int24 (§8.2/§8.7)
  ? 11 => uint,                  ; union-width: audio slot columns per RTP frame (runtime.h:606)
  ? 12 => transport,            ; transport in use (§4.4)
}
transport = &( usb: 0, ethernet: 1 )

; --- USB topology as the host sees it (§4.3): controller + hub chain ---
; For an Ethernet binding (§4.4) this key is ABSENT; the locus is net-topology.
usb-topology = {
    0 => tstr,                   ; host controller / root identifier (anonymized => "")
    1 => uint,                   ; USB bus number
    2 => uint,                   ; device address on the bus
    3 => [* uint],               ; port-number chain root->device (libusb_get_port_numbers)
    4 => usb-speed,              ; negotiated link speed
  ? 5 => [* hub-node],           ; hub chain root->device (controller, hubs)
  ? 6 => uint,                   ; VID of the bound device (kept; not a serial/name)
  ? 7 => uint,                   ; PID of the bound device (kept)
  ? 8 => tstr,                   ; device serial as USB descriptor (anonymized => "", §16)
}
usb-speed = &( unknown: 0, low: 1, full: 2, high: 3, super: 4, super-plus: 5 )
hub-node = {
    0 => uint,                   ; port number on parent
    1 => usb-speed,              ; speed at this hop
  ? 2 => uint,                   ; hub VID (kept)
  ? 3 => uint,                   ; hub PID (kept)
  ? 4 => tstr,                   ; hub product name (anonymized => "", §16)
  ? 5 => uint,                   ; number of downstream ports
}

; --- correlation / drift statistics (§7.3, §8.7) ---
clock-stats = {
    0 => int,                    ; current clock_drift_ppb estimate (host-measured, gauge)
  ? 1 => float64,                ; offset estimate, microseconds (host timeline vs device MSC)
  ? 2 => float64,                ; offset uncertainty (1-sigma band), microseconds
  ? 3 => clock-recovery,         ; recovery mode in effect
  ? 4 => uint,                   ; stream_reanchors observed this session (mirrors host-counter 7)
  ? 5 => asrc-stats,             ; present iff recovery == asrc (free-running)
  ? 6 => ratelock-stats,         ; present iff recovery == rate-lock (§8.7 bit-exact)
  ? 7 => ptp-stats,              ; present iff device participates in PTPv2 (§7.3, net.ptp)
}
clock-recovery = &(
    host-paced: 0,               ; SSI-driven, no recovery (§8.3 mode 1)
    asrc:       1,               ; receiver-side ASRC (free-running default)
    rate-lock:  2,               ; host-locked bit-exact (audio.rate-lock, §8.7)
)
asrc-stats = {
    0 => float64,                ; current resample ratio (out/in)
  ? 1 => uint64,                 ; input samples consumed
  ? 2 => uint64,                 ; output samples produced
  ? 3 => float64,                ; phase / fill error vs setpoint
  ? 4 => uint,                   ; converter quality mode (libsamplerate)
}
ratelock-stats = {
    0 => int,                    ; last audio.trim sent, ppb (signed rate correction)
  ? 1 => uint,                   ; receive-buffer fill, frames
  ? 2 => uint,                   ; buffer setpoint, frames
  ? 3 => uint64,                 ; trim messages sent this session
}
ptp-stats = {
    0 => bool,                   ; hardware timestamping in use (net.ptp.hw)
  ? 1 => float64,                ; offset-from-master, microseconds
  ? 2 => float64,                ; mean path delay, microseconds
  ? 3 => uint,                   ; PTP port state (slave/uncalibrated/etc.)
}

; --- persisted loopback measurements (§14.3), one per device/rate/mode ---
loopback-result = {
    0 => uint,                   ; sample rate, Hz (matches a latency-profile entry)
    1 => loopback-mode,          ; digital | analog
    2 => uint,                   ; measured round-trip, samples (transport buffering subtracted)
    3 => uint,                   ; in-slot measured
    4 => uint,                   ; out-slot measured
  ? 5 => uint,                   ; declared latency-profile total for this rate, samples (§6.4)
  ? 6 => int,                    ; measured - declared delta, samples (±1 ms gate, T11)
  ? 7 => uint,                   ; device-internal loop latency subtracted, samples (start-rsp key 5)
  ? 8 => tstamp,                 ; when measured
}
loopback-mode = &( digital: 0, analog: 1 )

; --- Ethernet/IP binding context (§4.4/§8.7), present iff net transport ---
net-topology = {
  ? 0 => tstr,                   ; device host:port as host resolved it (anonymized => "")
  ? 1 => tstr,                   ; mDNS instance name _harp._tcp (anonymized => "")
  ? 2 => uint,                   ; RTP jitter-buffer depth, frames (sized to segment PDV)
  ? 3 => bool,                   ; net.ptp advertised
  ? 4 => bool,                   ; net.ptp.hw advertised
  ? 5 => bool,                   ; net.offline in use (host-paced bounce over TCP)
}

; =====================================================================
; ANONYMIZATION (§16) — normative invariant. When diag-bundle key 3 == true,
; every tstr that is a serial, a user-facing name, or log free-text MUST be
; the empty string "", and CBOR structure / keys / map order MUST be
; preserved (the bundle stays valid against this schema; counters, timings,
; hashes, VID/PID, and `tag` fields are RETAINED — they reveal whether, not
; what; §16). Empty-string-in-place is REQUIRED (not key omission, and not a
; "[redacted]" placeholder): "" is shortest-form so deterministic CBOR
; length/order survive, and consumers must treat "" as "redacted", not as a
; distinct value.
;
; Fields the pass MUST clear:
;   bundle-meta key 2 (host platform);
;   device-section.identity key 2 (serial);
;   device-section.identity vendor key 1 / product key 1 (names);
;   device-section.identity engine key 0 (engine-id) MAY be retained (class
;     id, not a user name); engine key 2 (hash) ALWAYS retained;
;   device-section.identity channel-map entry keys 2/3/4 (name/group/path);
;   device-section.identity key 9 (build-id; may embed host/date);
;   device-section log-record key 3 (msg);
;   host-section log-record key 3 (msg), both top-level (key 8) and any merged;
;   state-transition key 4 (free-text detail);
;   usb-topology key 0 (controller id), key 8 (device serial);
;   hub-node key 4 (hub product name);
;   net-topology keys 0/1 (host:port, mDNS name).
;
; SEAM EXCEPTION (resolves the embed-verbatim vs §16 tension): the host
; stores device-section (key 4) byte-verbatim ONLY in the non-anonymized
; path. The anonymization pass MUST decode-and-re-encode device-section to
; clear the leaves above. The byte-identical "store verbatim" conformance
; gate is therefore defined on the device's `rsp diag.bundle` output and on
; the non-anonymized bundle — NOT on the post-anonymization bundle.
; =====================================================================
```

## diag.loopback methods (CDDL)
```cddl
; =====================================================================
; diag.loopback — round-trip latency test (§14.3). Append to spec/harp.cddl
; and Appendix A alongside the diag-bundle rules. Bodies are integer-keyed
; maps; responses ride the standard envelope { 0 => 2 (response), 1 => rid,
; 2 => method, ? 3 => body }; failures use the standard error-body.
;
; OWNERSHIP: device (eth-agent) owns start/stop handlers, the digital out->in
; routing, and capability advertisement. Host owns the measurement signal,
; the cross-correlation round-trip math, and the §6.4 latency-profile compare.
; =====================================================================

; ---- capability advertisement (§6.2 identity key 6) ----
; Devices add the modes they support to the capabilities array:
;   "diag.loopback.digital"  — host output -> input, NO analog stage
;                              (pure-DSP routing; refdev: yes)
;   "diag.loopback.analog"   — user patches a cable; path includes converters
;                              (refdev: no)
; The host MUST NOT issue diag.loopback.start for a mode the device did not
; advertise; the device MUST reject it with error "unsupported".

; ---- req diag.loopback.start ----
; Body verbatim from §14.3 (0 in-slot, 1 out-slot, 2 mode), with an OPTIONAL
; rate hint so the host can measure a specific latency-profile rate.
diag-loopback-start-req = {
    0 => uint,                   ; in-slot  (H->D slot, §6.3) the echo is read back on
    1 => uint,                   ; out-slot (D->H slot) the device routes back to in
    2 => loopback-mode-str,      ; "digital" | "analog" (tstr per §14.3)
  ? 3 => uint,                   ; OPTIONAL sample-rate hint, Hz (default = current epoch rate)
}
loopback-mode-str = "digital" / "analog"

; ---- rsp diag.loopback.start ----
; The device acks it armed the loop and ECHOES the routing actually in effect
; (a device MAY clamp/refuse a slot) plus the one constant the host genuinely
; cannot derive: device-internal loop latency. Returning effective slots/mode
; lets the host detect a slot it could not honor and fall back to §6.4.
diag-loopback-start-rsp = {
    0 => bool,                   ; armed: true => loopback engaged
    1 => loopback-mode-str,      ; mode actually engaged
    2 => uint,                   ; effective in-slot
    3 => uint,                   ; effective out-slot
    4 => uint,                   ; effective rate, Hz
  ? 5 => uint,                   ; device-internal loop latency, samples (engine/DSP path the
                                 ;   host cannot know; ~0 for a pure-digital copy, converter
                                 ;   latency for analog). Host subtracts this in T11 and persists
                                 ;   it as loopback-result key 7.
}
; Errors (envelope error-body, §A): "unsupported" if the requested
; diag.loopback.<mode> capability is absent; "bad-slot" for an in/out slot
; outside the channel map; "busy" if a stream/loopback is already armed;
; "state" if not in a state that can arm the loop.
; The host MUST treat start-rsp keys 1/2/3 mismatching its request as a SOFT
; failure and fall back to the declared latency-profile (§6.4).

; ---- req diag.loopback.stop ----
; No body (§14.3 shows bare `req diag.loopback.stop`; envelope key 3 absent).
diag-loopback-stop-req = null

; ---- rsp diag.loopback.stop ----
; Empty/absent body == success. The device MAY report informational figures,
; including a device-observed round-trip if it timed its own DSP path, so the
; host's cross-correlation can be corroborated.
diag-loopback-stop-rsp = {
  ? 0 => bool,                   ; stopped: loopback disengaged, normal routing restored
  ? 1 => uint,                   ; device-observed round-trip, samples (if the device measured)
  ? 2 => uint,                   ; frames echoed during the armed window (informational)
}

; ---- host-side measurement contract (§14.3, informative) ----
; 1. diag.loopback.start{in,out,mode}; check start-rsp.armed and effective routing.
; 2. emit a measurement signal (one-sample impulse or chirp) on out-slot at a
;    known H->D stream timestamp.
; 3. cross-correlate the D->H in-slot capture to find the echo.
; 4. round-trip = rx_ts - tx_ts.
; 5. subtract known transport buffering (stream depth + prebuffer, from
;    audio-config) AND start-rsp key 5 (device-internal loop latency).
; 6. compare to latency-profile[rate] total (§6.4) within ±1 ms (T11).
; 7. diag.loopback.stop.
; 8. persist as loopback-result in the diag-bundle (key 12), per device/rate/mode.
;    Per §14.3 a present measured value takes precedence for PDC.
; The test is most reliable in host-paced mode (deterministic pacing); CI on
; macOS/Windows (relative-nanosleep jitter) should gate the strict ±1 ms oracle
; to Linux (TIMER_ABSTIME), matching the existing eth-test oracle pattern.
```

## Anonymization (§16)

When diag-bundle key 3 == true the host runs a §16 pass that overwrites a fixed list of tstr LEAVES with the empty string "", preserving CBOR structure, keys, and map order so the bundle stays schema-valid and deterministically encoded. Cleared: bundle-meta key 2 (host platform); device-section identity key 2 (serial), identity vendor.1 / product.1 (names), identity channel-map entries' keys 2/3/4 (name/group/path), identity key 9 (build-id); device-section log-record key 3 (msg); host runtime log-record key 3 (msg, key 8 and any merged); state-transition key 4 (detail); usb-topology key 0 (controller id) and key 8 (device serial); hub-node key 4 (hub product name); net-topology keys 0/1 (host:port, mDNS name). RETAINED (reveal whether, not what, per §16): all counters (device + host), timings/tstamps, every hash (incl. identity engine.2 param-map-hash), VID/PID numeric ids, and all `tag` fields. identity engine.0 (engine-id) MAY be retained as a class id. Rules: empty-string-in-place is REQUIRED — not key omission and not a "[redacted]" placeholder ("" is shortest-form, so deterministic length/order survive; consumers MUST treat "" as redacted, not as a distinct value). SEAM EXCEPTION resolving the embed-verbatim vs §16 tension: the host stores device-section (key 4) byte-verbatim ONLY in the non-anonymized path; the anonymization pass MUST decode-and-re-encode device-section to clear its leaves. The byte-identical device-section conformance gate is therefore defined on the device's `rsp diag.bundle` output and on the non-anonymized bundle, NOT on the post-anonymization bundle. Enforcement is by CI assertion (planned diag-bundle-eth-test.sh decodes an anonymized bundle and fails if any listed leaf is non-empty) since CDDL alone cannot prove the pass ran (a "" and a real string are both tstr).

## Design — v0 (harp-probe diag-bundle, buildable today, zero device changes)

MINIMAL harp-probe diag-bundle BUILDABLE TODAY (host-only, zero device changes, reuses live diag.counters + do_hello).

WHY it builds now: harp-probe already has both halves. do_hello (host/harp-probe.c:209 -> harp_client_hello) returns harp_client_identity (host/client.h:38: vendor/product/vendor_id/product_id/serial/fw/engine_id/engine_ver/param_map_hash/caps). cmd_counters (host/harp-probe.c:648) already round-trips `req diag.counters` and gets the 16-pair text-keyed map back in e.body/e.body_len (envelope.h:24). The CBOR encoder guarantees RFC8949 deterministic output (cbor.h:1-11). File write copies write_wav16's fopen/fwrite/fclose pattern (harp-probe.c:268-299).

CONCRETE STEPS, all inside one new cmd_diag_bundle(probe *p, const char *outfile, bool anon) added after cmd_counters (~harp-probe.c:678):

(1) THE CMD: parse `diag-bundle [--anonymize] [outfile.cbor]` in main dispatch after the "counters" branch (host/harp-probe.c:1007). Default outfile = "harp-diag.cbor". Flag --anonymize sets a local bool.

(2) THE REQUEST: harp_client_identity id = do_hello(p);  then exactly cmd_counters' flow — req_head(p,&req,"diag.counters",false); harp_env e = request(p,&req,&rsp); — but DON'T decode; keep e.body/e.body_len, the verbatim encoded counters map (this IS device-section key 1).

(3) THE ASSEMBLE (deterministic CBOR, integer keys, mirrors encode_bundle at runtime.cpp:2028):
 top map(5): 0=>"harpd"; 1=>uint 1; 2=>bundle-meta{0=>[epoch,0]} (gettimeofday; msc 0 in v0; +?1=>"harp-probe 0.4.0"); 3=>bool anon; 4=>device-section.
 device-section map(2): 0=>identity re-encoded from `id` per Appendix A identity rule — reuse the exact encode_identity key shape (session.c:176): vendor{0=>vendor_id,1=>vendor}, product{0=>product_id,1=>product}, 2=>serial, 3=>fw, engine{0=>engine_id,1=>engine_ver,2=>param_map_hash bytes}, 6=>caps array; 1=>VERBATIM counters via harp_cbuf_put(&out, e.body, e.body_len) (byte-identical embed — the device conformance gate).
 v0 OMITS optional keys 5..15 and device-section key 2/3 (a v0 reader skips unknown; a vN writer omits unfilled — schema's V0-EXTENSIBILITY rule). Required-from-day-one set 0,1,2,3,4 is exactly what this writes.

(4) THE FILE WRITE: FILE *f=fopen(outfile,"wb"); fwrite(out.buf,1,out.len,f); fclose(f); print "wrote N bytes to PATH" (harp-probe.c:270-298 pattern). harp_cbuf_free both.

(5) THE --anonymize FLAG (v0 scope = device-section only, since that's all v0 writes): before re-encoding identity, if anon: emit "" for serial (identity key 2), vendor name (vendor key 1), product name (product key 1); RETAIN vendor_id/product_id/param_map_hash/engine_id/caps (reveal whether not what, §16). Empty-string-in-place, NOT key omission (schema ANONYMIZATION rule: "" is shortest-form so deterministic length/order survive). Set top-level key 3=true. NOTE the seam exception: v0 anonymize must NOT embed counters-verbatim under an anon identity inconsistently — counters carry no PII so they stay verbatim either way; only identity leaves change. This is the host-side §16 pass in miniature; the full assembler (host_modules below) generalizes it to all listed leaves.

## Design — host modules (full §14.4/§14.3)

- **cmd_diag_bundle (harp-probe v0 + exporter)** (`host/harp-probe.c (new fn after cmd_counters ~:678; dispatch after :1007)`): v0 entry point: do_hello + req diag.counters, assemble the 5-key bundle (0..4), embed counters verbatim (harp_cbuf_put e.body), apply --anonymize to identity leaves, fopen/fwrite to outfile. Is the file EXPORTER for the CLI path. For v1+ it instead issues `req diag.bundle` to the device and stores the returned device-section under key 4 verbatim.
- **DiagBundleAssembler (HarpRuntime::getDiagBundle)** (`shell/runtime.cpp + shell/runtime.h (new public method, mirrors getStateBundle :2089 and encode_bundle :2028)`): §14.4 host assembler. Issues `req diag.bundle` under ctlMutex_ (request() :583), takes the device's rsp body as device-section key 4 (verbatim in non-anon path), then appends host-owned keys 5..13: host-counters (key5) from underruns_/padSamples_/evDrops_/framesSent_/framesRecv_ (runtime.h:629-631,802) + session generation (723) + renegCount (737); audio-config (key9) from sample rate/block/bitExact_/freeRunning_/wantHostPaced_/unionWidth_ (runtime.h:602-608) + transport_ (589); session-history (key6) and runtime logs (key8) from the new ring buffers below; clock-stats (key11) and net-topology (key13) from the time-correlation subsystem. Emits deterministic CBOR (cbor.c).
- **HostContextGatherer (USB topology + clock/drift snapshot)** (`host/usb_io.c + host/usb_io.h (topology walk); shell/runtime.cpp (clock snapshot, calls into host/freerun.c)`): §14.4 host-context. usb-topology (key10): libusb_get_port_numbers / get_bus_number / get_device_address / speed -> {0 controller,1 bus,2 addr,3 port-chain,4 speed,?6 VID,?7 PID,?8 serial} (ABSENT on Ethernet binding). clock-stats (key11): current clock_drift_ppb, offset+uncertainty, clock-recovery enum (host-paced/asrc/rate-lock from bitExact_/deviceRateLock_), asrc-stats from host/freerun.c libsamplerate state, ratelock-stats from last audio.trim, ptp-stats if net.ptp. net-topology (key13): HARP_ETH_DEVICE host:port + mDNS + jitter depth when transport_ is Ethernet.
- **SessionHistory + RuntimeLog ring buffers** (`shell/runtime.h + shell/runtime.cpp (new members + recording at sessionUp/sessionDown/reneg/reconcile sites)`): §14.4 instrumentation the device cannot see. state-transition records {tstamp, from, to, ?reason, ?detail} (key6) appended at each §12.1 transition (connect/hello/audio.start/stop/reset/transport-error). RuntimeLog ring (>=64KiB, runtime.h:629-631 log_msg sites rerouted) -> log-record{0 msc,1 level,2 tag,3 msg,?4 wall} (key8). These feed the assembler; device logs (device-section key2) are a separate device-owned ring drained over stream `log`.
- **BundleAnonymizer (§16 decode-reencode pass)** (`shell/runtime.cpp (new anonymizeDiagBundle) or new shell/diag-anon.cpp`): §16 normative pass when key3==true. Decode-and-RE-ENCODE the whole bundle INCLUDING device-section (resolves the embed-verbatim-vs-§16 seam: verbatim store is ONLY the non-anon path) overwriting the fixed tstr leaf list with "" in place (bundle-meta.2; identity.2 serial, vendor.1/product.1 names, channel-map 2/3/4, identity.9 build-id; all log-record.3 msg; state-transition.4 detail; usb-topology.0/.8; hub-node.4; net-topology.0/.1), RETAINING all counters/tstamps/hashes (incl engine.2)/VID-PID/tags. Preserves CBOR structure+map order so it stays schema-valid + deterministic. Enforced by CI assertion, not the type system.
- **LoopbackMeasurer (§14.3 round-trip)** (`shell/runtime.cpp (new measureLoopback) + host/harp-probe.c (cmd_loopback CLI)`): §14.3 host measurement. diag.loopback.start{in,out,mode,?rate}; check start-rsp.armed + effective routing (soft-fallback to §6.4 on slot mismatch). Emit a one-sample impulse/chirp on out-slot at a known H->D timestamp (tone_hz generator exists device-side device.h:253 for reference). Cross-correlate the D->H in-slot capture; round-trip = rx_ts - tx_ts; subtract stream depth + prebuffer (from audio-config) AND start-rsp key5 device-internal loop latency; compare to latency-profile[rate] within ±1ms (T11). diag.loopback.stop. Persist as loopback-result (bundle key12).

## Design — device hooks (eth-agent)

- **handle_diag_bundle** (`device/session.c (new handler; register in dispatch at :1346 alongside diag.counters)`): Assemble device-section EXACTLY (the contract): map{0=>identity (call encode_identity, session.c:176, the same 13-key map), 1=>counters (the same 16-pair map handle_diag_counters builds, session.c:688-735 — factor that map-body into a shared emit_counters(harp_cbuf*) so diag.counters AND diag.bundle emit byte-identical counters), ?2=>recent device logs from the §4.2 `log` ring, ?3=>device-audio-config present iff a stream is live (from audio_state: rate/nsamples/mode/out_slots/reanchors/rate_trim_ppb, device.h:243-266)}. Return this map VERBATIM as the body of `rsp diag.bundle`. The byte-identical round-trip of device-section is the device conformance gate. Advertise "diag.bundle" in encode_identity capabilities array (session.c:214-247).
- **handle_diag_loopback_start** (`device/session.c (new handler; register at :1346)`): Parse body{0 in-slot,1 out-slot,2 mode "digital"/"analog",?3 rate}. Reject with error "unsupported" if the mode's capability wasn't advertised, "bad-slot" if out of channel map, "busy"/"state" per the diag.loopback CDDL. Set a loopback flag + (in,out) in audio_state (device.h:219-276, new fields loopback_on/loopback_in/loopback_out). Reply start-rsp{0 armed,1 effective mode,2 eff in-slot,3 eff out-slot,4 eff rate,?5 device-internal loop latency ~0 for digital}. Advertise "diag.loopback.digital" (and .analog if converters) in encode_identity caps (session.c:214).
- **handle_diag_loopback_stop** (`device/session.c (new handler; register at :1346)`): Clear the audio_state loopback flag, restore normal routing. Reply stop-rsp (empty body = success) or optional {0 stopped,?1 device-observed round-trip,?2 frames echoed} for host corroboration. No request body (diag-loopback-stop-req = null).
- **engine loopback routing flag (render hook)** (`device/engine.c (render_output :1528 / render_part_slots :1408) + device/device.h audio_state (:219)`): When audio_state.loopback_on && mode==digital: COPY the host's H->D input on the chosen in-slot path back to the D->H out-slot BEFORE the (absent) analog stage — a pure-DSP echo, no converter — so the host's cross-correlation finds its impulse. Gated entirely on the new flag so the default render path (the byte-identical golden path at engine.c:1548) is untouched.

## Design — test plan

- **scripts/diag-bundle-eth-test.sh — v0 assembler + counters-verbatim round-trip** [eth.yml]: start_dev on a unique port (47945); HARP_ETH_DEVICE; perl -e 'alarm 20' guards the host; harp-probe diag-bundle writes harp-diag.cbor. Decode: key0=="harpd", key1==1, key3==false, device-section.identity.serial==SIM-0001, device-section.counters CONTAINS the 16 mandatory keys (assert KEYS present, NOT real values — refdev hardcodes audio_underruns/late/msc/usb/drift to 0 at session.c:706-722). Counters-verbatim gate: bytes of device-section key1 == bytes of a standalone `req diag.counters` response.
- **scripts/diag-bundle-eth-test.sh (anonymize case) — §16 pass enforcement** [eth.yml]: harp-probe diag-bundle --anonymize: decode and FAIL if any listed leaf is non-empty (identity.2 serial=="", vendor.1=="", product.1==""); RETAINED fields still present and non-empty (counters values, param_map_hash bytes, vendor_id/product_id, caps, every `tag`). key3==true. CBOR re-decodes cleanly (structure/order preserved). This is the CI assertion the schema mandates because CDDL can't prove the pass ran.
- **scripts/diag-bundle-eth-test.sh (host-section case, v1+) — full §14.4 assembler** [eth.yml]: After an 8s streaming session via the shell, the runtime-assembled bundle carries host-counters (key5: frames_sent/recv > 0), audio-config (key9: rate/block/transport==ethernet/bit-exact bool), session-history (key6: at least DETACHED->...->STREAMING transitions), runtime logs (key8 non-empty). usb-topology (key10) ABSENT on the Ethernet binding; net-topology (key13) present.
- **scripts/diag-loopback-eth-test.sh — §14.3 round-trip vs §6.4 profile (T11)** [eth.yml]: Query hello for diag.loopback.digital cap + latency-profile. diag.loopback.start{in,out,digital}; emit impulse; cross-correlate D->H capture; round-trip - transport buffering - start-rsp.key5 ≈ latency-profile[48000] within ±1ms. Assert start-rsp.armed==true and effective routing matches request. Hang-proof: unique port (47946), kill-by-pid, perl alarm. STRICT ±1ms oracle LINUX-ONLY (TIMER_ABSTIME); macOS/Windows assert armed+echo-detected only (relative-nanosleep jitter — matches eth-tests.sh:120 gating pattern).
- **diag-bundle ASan run (decode/encode + anonymize re-encode memory safety)** [eth-asan.yml]: The bundle assembler + anonymizer decode-and-re-encode path (touches device-section bytes) runs clean under ASan: no leaks in harp_cbuf, no OOB in the §16 leaf-clearing walk, no use-after-free on e.body (which points into the parsed rsp buffer — assert it's consumed before rsp_buf is freed).
- **scripts/diag-loopback-test.sh (real device, analog mode) — hw oracle** [hw.yml]: On PI4B-0002 over USB: if the refdev advertises diag.loopback.analog, a cabled round-trip includes converter latency (start-rsp.key5 > 0) and measured-vs-declared delta within ±1ms. Digital-only refdev SKIPs analog. This is the authoritative latency oracle the loopback test corroborates.

## Design — sequencing

SCHEMA-AS-CONTRACT FIRST (shared, blocks nothing): append the diag-bundle + diag.loopback CDDL to spec/harp.cddl (after recall-bundle, harp.cddl:49) and Appendix A. FREEZE the integer key assignments (top-level 0-13, host-counters 0-9) and all new enum numberings before any vendor tooling depends on them — these are design, not pre-fixed by spec prose (open question in the synthesis). The frozen CDDL is the narrow contract both agents code to independently.

HOST-SIDE LANDS FIRST, fully testable with ZERO device changes (the whole point of v0):
1. v0 harp-probe cmd_diag_bundle (host/harp-probe.c) — builds TODAY against the EXISTING diag.counters + do_hello. Embeds the live counters body verbatim; re-encodes identity from harp_client_identity; --anonymize clears identity leaves. Ship + green diag-bundle-eth-test.sh (v0 + anonymize cases) on eth.yml using the CURRENT harp-deviced. This proves the envelope, the counters-verbatim gate, and the §16 assertion harness before the device grows a single handler.
2. Host BundleAnonymizer generalized (decode-reencode, all leaves) + ASan coverage — still device-independent.
3. Host instrumentation (SessionHistory + RuntimeLog rings, HostContextGatherer USB/clock) + HarpRuntime::getDiagBundle — lands the host sections (keys 5..13) which the device never touches, so it can't be blocked by eth-agent.

DEVICE-SIDE HANDOFF (eth-agent codes to the frozen device-section CDDL):
4. handle_diag_bundle + the shared emit_counters() refactor (so diag.counters and diag.bundle emit byte-identical counters) + "diag.bundle" capability. Once shipped, getDiagBundle switches from "host re-encodes identity itself (v0)" to "store the device's rsp device-section verbatim under key4" — the byte-identical round-trip becomes the device conformance gate.
5. diag.loopback.start/.stop handlers + engine render flag (engine.c:1528/1408, gated so the golden path is untouched) + capability advert. Then the host LoopbackMeasurer + diag-loopback-eth-test.sh (T11) goes green.

CI-GREEN PATH: every script is hang-proofed (unique port, kill-by-pid, perl alarm) per the eth-tests.sh idiom; bundle tests assert KEYS-present not real values (refdev hardcodes 6 counters to 0 at session.c:706-722); the strict ±1ms loopback oracle is Linux-only (TIMER_ABSTIME), macOS/Windows assert connect+armed+echo only — matching the existing eth.yml MAC/WIN gating. Each step keeps main green: steps 1-3 need no device change and run against the current binary; steps 4-5 are additive (new dispatch branches + a render flag defaulting off), so the golden/eth suites stay byte-identical.

## Open questions — FREEZE before spec adoption

1. KEY/ENUM FREEZE: the top-level integer key assignments (0-13), the host-counters keys (0-9), and ALL new enum numberings (session-state, transition-reason, usb-speed, clock-recovery, clock-mode, transport, loopback-mode, log-level) are my design — not pre-fixed by any spec prose. eth-agent + spec editor must review/FREEZE these before any vendor tooling depends on them, and bump diag-bundle key 1 (version) if the layout changes after first adoption.
1. MAGIC STRING: I chose "harpd" (parallels recall-bundle "harpb"); P3 proposed "harpdiag". Confirm no existing/future artifact claims "harpd" and that the team prefers the 5-char parallel over the longer tag — changing a magic after ship is painful.
1. OPEN-MAP CDDL PORTABILITY: the `counters` rule mixes named string keys with a wildcard `* vendor-counter-key => uint/int` plus a .regexp socket. Not every CDDL validator handles wildcard-plus-named-keys identically. Validate against the spec repo's actual linter before declaring it normative; if it rejects, fall back to `{ * tstr => uint/int }` and move the mandatory member list to prose (loses schema-level documentation of the mandatory set).
1. COUNTERS STRICTNESS: I kept `counters` permissive (named members are documented but a missing mandatory counter would still validate, matching the device emitting 0 for untracked counters). A stricter schema would use a cut group on the required set. Decide deliberately whether v0 conformance should reject a bundle missing a mandatory §14.2 counter.
1. device-section EMBED-VERBATIM vs ANONYMIZATION: I resolved this by defining the byte-identical gate on the device rsp + non-anon bundle and having the anon pass re-encode device-section. Confirm eth-agent and host team accept this split (the alternative — host never reaches into device-section, requiring the DEVICE to offer an anonymized variant — pushes §16 onto the device, which the brief assigns to the host).
1. loopback start-rsp key 5 (device-internal loop latency) is beyond the bare §14.3 text. If eth-agent implements to the literal spec it may omit it (it is optional) — but then the host loses the one figure it cannot measure, degrading T11 accuracy. Coordinate so the device fills key 5 (~0 for digital, converter latency for analog). Also confirm eth-agent wants to device-side-measure at all (stop-rsp key 1) vs. host cross-correlation being the sole oracle.
1. MULTI-DEVICE SCOPE: this bundle is per-device (one identity, one device-section, one usb-topology). §15.1 runtimes own >=8 sessions. A multi-device support export would need an array-of-bundles wrapper. I scoped per-device deliberately (the §14.4 one-button promise reads per-device); confirm that is the intended scope.
1. clock_drift_ppb appears in three places — device counters (canonical gauge), clock-stats key 0 (host-measured), and clock-stats key 4 mirrors stream_reanchors. These can disagree if sampled at different instants. I treat the device counters value as canonical; confirm that precedence and whether the host-measured clock-stats.0 should instead be the authority when present.
1. DEVICE EMITS HARDCODED ZEROS today (audio_underruns, audio_late_frames, msc_discontinuities, evt_stale_epoch, usb_errors, clock_drift_ppb all 0 at device/session.c:706-722). The CDDL requires the KEYS (correct) but v0 bundles will look suspiciously clean. CI bundle tests must assert on keys present, not on real values for these, until the refdev TODOs are filled.
1. TEXT-KEYED vs INTEGER-KEYED inconsistency: counters and log-record use text/integer keys faithful to their existing wire formats, while every other map is integer-keyed. A reviewer will notice the mix. It is deliberate (changing counters to integer keys breaks the live diag.counters method and the harp-probe parser), but confirm the spec editor accepts documenting it as an intentional exception.
