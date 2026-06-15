/* host/sync_compat.h — minimal cross-platform threading for the USB transport.
 *
 * usb_io.c needs a mutex, a condition variable with an absolute-deadline timed
 * wait, one background thread, and an acquire/release flag. POSIX maps 1:1 to
 * pthreads — the hardware-validated completion-reaping path is byte-for-byte
 * unchanged. Windows uses SRWLOCK + CONDITION_VARIABLE + _beginthreadex.
 *
 * Deadlines are opaque absolute timestamps in a per-platform clock domain:
 * harp_deadline_ms(ms) stamps one and harp_cond_timedwait waits until it. On
 * POSIX that is CLOCK_REALTIME ns fed straight to pthread_cond_timedwait (what
 * the original deadline_in did); on Windows it is the monotonic harp_now_ns
 * domain, converted to a relative-ms timeout for SleepConditionVariableSRW.
 * Either way the TOTAL wait is bounded by the deadline across spurious wakeups. */
#ifndef HARP_SYNC_COMPAT_H
#define HARP_SYNC_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h> /* _beginthreadex */

#include "harp/plat.h" /* harp_now_ns for the deadline domain */

typedef SRWLOCK harp_mutex;
typedef CONDITION_VARIABLE harp_cond;
typedef HANDLE harp_thread;
typedef volatile long harp_flag;
#define HARP_THREAD_RET unsigned __stdcall
typedef unsigned(__stdcall *harp_thread_fn)(void *);

static inline void harp_mutex_init(harp_mutex *m) { InitializeSRWLock(m); }
static inline void harp_mutex_destroy(harp_mutex *m) { (void)m; }
static inline void harp_mutex_lock(harp_mutex *m) { AcquireSRWLockExclusive(m); }
static inline void harp_mutex_unlock(harp_mutex *m) { ReleaseSRWLockExclusive(m); }

static inline void harp_cond_init(harp_cond *c) { InitializeConditionVariable(c); }
static inline void harp_cond_destroy(harp_cond *c) { (void)c; }
static inline void harp_cond_broadcast(harp_cond *c) { WakeAllConditionVariable(c); }
static inline void harp_cond_wait(harp_cond *c, harp_mutex *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}

static inline uint64_t harp_deadline_ms(unsigned ms) {
    return harp_now_ns() + (uint64_t)ms * 1000000ull;
}
/* returns true if the wait ended WITHOUT a signal (timed out / error) */
static inline bool harp_cond_timedwait(harp_cond *c, harp_mutex *m, uint64_t deadline_ns) {
    uint64_t now = harp_now_ns();
    DWORD ms = 0;
    if (deadline_ns > now) {
        /* round UP: a sub-millisecond remainder must still wait ~1 ms, not floor
         * to 0 — else the 1 ms USB-backpressure waits busy-spin and give up
         * immediately instead of yielding for a slot/byte to arrive. */
        uint64_t rem = (deadline_ns - now + 999999ull) / 1000000ull;
        ms = rem > 0xFFFFFFFEull ? 0xFFFFFFFEu : (DWORD)rem;
    }
    return SleepConditionVariableSRW(c, m, ms, 0) ? false : true;
}

static inline int harp_thread_create(harp_thread *t, harp_thread_fn fn, void *arg) {
    uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    *t = (harp_thread)h;
    return h ? 0 : -1;
}
static inline void harp_thread_join(harp_thread t) {
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}

static inline int harp_flag_load_acq(harp_flag *f) {
    return (int)InterlockedCompareExchange(f, 0, 0);
}
static inline void harp_flag_store_rel(harp_flag *f, int v) {
    InterlockedExchange(f, (long)v);
}

#else /* POSIX — 1:1 with the original pthread code */

#include <errno.h>
#include <pthread.h>
#include <time.h>

typedef pthread_mutex_t harp_mutex;
typedef pthread_cond_t harp_cond;
typedef pthread_t harp_thread;
typedef int harp_flag;
#define HARP_THREAD_RET void *
typedef void *(*harp_thread_fn)(void *);

static inline void harp_mutex_init(harp_mutex *m) { pthread_mutex_init(m, NULL); }
static inline void harp_mutex_destroy(harp_mutex *m) { pthread_mutex_destroy(m); }
static inline void harp_mutex_lock(harp_mutex *m) { pthread_mutex_lock(m); }
static inline void harp_mutex_unlock(harp_mutex *m) { pthread_mutex_unlock(m); }

static inline void harp_cond_init(harp_cond *c) { pthread_cond_init(c, NULL); }
static inline void harp_cond_destroy(harp_cond *c) { pthread_cond_destroy(c); }
static inline void harp_cond_broadcast(harp_cond *c) { pthread_cond_broadcast(c); }
static inline void harp_cond_wait(harp_cond *c, harp_mutex *m) { pthread_cond_wait(c, m); }

static inline uint64_t harp_deadline_ms(unsigned ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec +
           (uint64_t)ms * 1000000ull;
}
static inline bool harp_cond_timedwait(harp_cond *c, harp_mutex *m, uint64_t deadline_ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    return pthread_cond_timedwait(c, m, &ts) != 0; /* nonzero = timed out / error */
}

static inline int harp_thread_create(harp_thread *t, harp_thread_fn fn, void *arg) {
    return pthread_create(t, NULL, fn, arg);
}
static inline void harp_thread_join(harp_thread t) { pthread_join(t, NULL); }

static inline int harp_flag_load_acq(harp_flag *f) {
    return __atomic_load_n(f, __ATOMIC_ACQUIRE);
}
static inline void harp_flag_store_rel(harp_flag *f, int v) {
    __atomic_store_n(f, v, __ATOMIC_RELEASE);
}

#endif

#endif /* HARP_SYNC_COMPAT_H */
