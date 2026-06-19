/* compat_pthread.h — Windows (_WIN32) shim for the SMALL pthread surface the
 * device uses, so every device .c (engine.c, state.c, session.c, panel.c,
 * harp-deviced.c) compiles UNCHANGED on Windows. device.h includes this in
 * place of <pthread.h> when _WIN32 is defined; POSIX builds never see it.
 *
 * Scope = exactly the primitives grep proved the device uses, nothing more:
 *   pthread_mutex_t / _init / _lock / _unlock / _destroy / PTHREAD_MUTEX_INITIALIZER
 *   pthread_t / pthread_create / pthread_join / pthread_detach
 * (No cond vars, TLS, attrs, trylock, rwlock, or pthread_self exist in device/.)
 *
 * THE ONE TRAP (why pthread_mutex_t is an aggregate, not a bare CRITICAL_SECTION):
 * PTHREAD_MUTEX_INITIALIZER is used at STATIC scope (engine.c g_evq_mu), where a
 * CRITICAL_SECTION has no static initializer and calling EnterCriticalSection on a
 * zeroed section is undefined behavior. So the mutex carries an INIT_ONCE guard and
 * lazily InitializeCriticalSection's on first lock/init. This makes the static-init
 * sites and the runtime pthread_mutex_init sites (harp-deviced.c send_mu/state_mu)
 * identical and safe, at the cost of one InterlockedCompareExchange per lock after
 * the first.
 */
#ifndef HARP_DEVICE_COMPAT_PTHREAD_H
#define HARP_DEVICE_COMPAT_PTHREAD_H
#ifdef _WIN32

#include <windows.h>
#include <stdlib.h> /* malloc/free for the thread thunk (device.h includes it later) */

typedef SSIZE_T ssize_t; /* MSVC has no ssize_t; SSIZE_T (from windows.h) is the right width */

/* ---------- mutex ---------- */
typedef struct {
    INIT_ONCE once;      /* lazy one-time init of cs — covers static initializer */
    CRITICAL_SECTION cs;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { INIT_ONCE_STATIC_INIT, {0} }

static BOOL CALLBACK harp__cs_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)ctx;
    InitializeCriticalSection((CRITICAL_SECTION *)param);
    return TRUE;
}
static __inline void harp__cs_ensure(pthread_mutex_t *m) {
    InitOnceExecuteOnce(&m->once, harp__cs_init, &m->cs, NULL);
}
static __inline int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    INIT_ONCE z = INIT_ONCE_STATIC_INIT;
    (void)attr;
    m->once = z;            /* fresh guard; the ensure below does the real init */
    harp__cs_ensure(m);
    return 0;
}
static __inline int pthread_mutex_lock(pthread_mutex_t *m) {
    harp__cs_ensure(m);     /* safe whether statically- or runtime-initialized */
    EnterCriticalSection(&m->cs);
    return 0;
}
static __inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    LeaveCriticalSection(&m->cs);
    return 0;
}
static __inline int pthread_mutex_destroy(pthread_mutex_t *m) {
    DeleteCriticalSection(&m->cs);
    return 0;
}

/* ---------- threads ---------- */
typedef HANDLE pthread_t;

typedef struct { void *(*fn)(void *); void *arg; } harp__thunk;
static DWORD WINAPI harp__thread_tramp(LPVOID p) {
    harp__thunk t = *(harp__thunk *)p; /* adapt void*(*)(void*) -> DWORD WINAPI */
    free(p);
    t.fn(t.arg);
    return 0;
}
static __inline int pthread_create(pthread_t *th, const void *attr,
                                   void *(*fn)(void *), void *arg) {
    harp__thunk *t = (harp__thunk *)malloc(sizeof *t);
    (void)attr;
    if (!t) return 1;
    t->fn = fn; t->arg = arg;
    {
        HANDLE h = CreateThread(NULL, 0, harp__thread_tramp, t, 0, NULL);
        if (!h) { free(t); return 1; }
        *th = h;
    }
    return 0;
}
static __inline int pthread_join(pthread_t th, void **ret) {
    (void)ret; /* the device never reads a thread return value */
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    return 0;
}
static __inline int pthread_detach(pthread_t th) {
    CloseHandle(th);
    return 0;
}

#endif /* _WIN32 */
#endif /* HARP_DEVICE_COMPAT_PTHREAD_H */
