/* host/rt_sched.h — real-time scheduling promotion for HARP's audio-path threads.
 *
 * One portable entry point, harp_thread_set_realtime(), that lifts the CALLING
 * thread onto the host's hard real-time class (macOS THREAD_TIME_CONSTRAINT,
 * Linux SCHED_FIFO, Windows MMCSS "Pro Audio") so it is never descheduled by ordinary time-share work under
 * host CPU load. This is the scheduling half of the §4.3 USB low-latency feed —
 * factored OUT of host/usb_io.c (which is compiled only when libusb is present)
 * so that:
 *   - the host-paced shell runtime's feed/pump/reader threads adopt the SAME
 *     class even in a build configured WITHOUT the USB transport, and
 *   - the determinism it grants can be regression-gated with NO USB hardware
 *     (tools/rt-jitter-bench.c, scripts/rt-jitter-gate.sh) — a periodic thread's
 *     wakeup-lateness tail under synthetic CPU contention is the host-side proxy
 *     for the USB producer-cadence (out_gap) tail that #133 collapsed.
 *
 * Behaviour is byte-for-byte the function that shipped in usb_io.c: it honours
 * the global HARP_USB_RT=0 off-switch (the A/B "before" arm + safety valve) and
 * degrades gracefully (keeps the caller's prior scheduling, logs once) where RT
 * is unavailable. */
#ifndef HARP_RT_SCHED_H
#define HARP_RT_SCHED_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* §8.3 pacing block at 48 k = 256/48000 s: the default real-time period when a
 * caller passes <=0. The kernel sizes a sub-millisecond CPU reservation from it. */
#define HARP_RT_PERIOD_US_DEFAULT 5333.0

/* Promote the CALLING thread to the host's real-time scheduling class (macOS
 * THREAD_TIME_CONSTRAINT, Linux SCHED_FIFO) sized to `period_us` (the audio block
 * cadence; <=0 uses HARP_RT_PERIOD_US_DEFAULT). The USB completion thread does
 * this for itself; a host-paced runtime should call it from EVERY thread on the
 * realtime audio path (the D->H reader, the pacing feeder, the event pump) so none
 * can be descheduled by ordinary load — that is what keeps a LOW-latency USB
 * stream dropout-free without growing any buffer. Honours HARP_USB_RT=0 (returns
 * false without touching scheduling). Degrades gracefully (keeps the caller's
 * prior scheduling, logs once) where RT is unavailable; returns true iff
 * real-time was actually granted. */
bool harp_thread_set_realtime(double period_us);

#ifdef __cplusplus
}
#endif

#endif /* HARP_RT_SCHED_H */
