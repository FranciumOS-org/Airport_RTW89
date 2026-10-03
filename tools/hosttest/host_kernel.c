// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace stand-ins for the kernel symbols the driver object imports
 * (docs/kernel-imports.md), so the very same build/out/AirPort_RTW89.o that
 * goes into the kext can be linked into a normal program and exercised
 * without risking a panic. Semantics are close enough for a smoke test:
 * IOLocks are pthread mutexes, kernel threads are pthreads, thread calls are
 * one-shot timer threads.
 *
 * Built as ordinary C against libSystem: no kernel headers here.
 */
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- memory / logging / delays ---- */

void *IOMalloc(size_t size) { return malloc(size); }
void *IOMallocZero(size_t size) { return calloc(1, size); }
void IOFree(void *p, size_t size) { (void)size; free(p); }

void IOLog(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* The driver is built with -DIOLog=rtw89_iolog (its log ring in the kext). */
void rtw89_iolog(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
}

void IODelay(unsigned us) { usleep(us); }
void IOSleep(unsigned ms) { usleep(ms * 1000u); }

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    if (!size)
        return 0;
    return n < 0 ? 0 : (n >= (int)size ? (int)size - 1 : n);
}

void read_random(void *buf, unsigned len) { arc4random_buf(buf, len); }
/* hostap's buffer overflow handler in the kernel build (rtw89_hostap_abort) */
void panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    abort();
}

int PE_parse_boot_argn(const char *name, void *ptr, int size)
{
    const char *v = getenv(name);
    unsigned long val;

    if (!v)
        return 0;
    val = strtoul(v, NULL, 0);
    memcpy(ptr, &val, size > (int)sizeof(val) ? sizeof(val) : (size_t)size);
    return 1;
}

/* ---- zlib ---- */

/*
 * The kernel's zlib numbers its flush modes differently (Z_FINISH is 5 there,
 * 4 in libz, where 5 means Z_BLOCK). The firmware loader was compiled against
 * the kernel header, so translate before handing the call to libz.
 */
int inflate(void *strm, int flush)
{
    static int (*real)(void *, int);

    if (!real)
        real = (int (*)(void *, int))dlsym(RTLD_NEXT, "inflate");
    return real(strm, flush == 5 ? 4 : flush);
}

/* ---- time ---- */

static mach_timebase_info_data_t timebase;

static void timebase_init(void)
{
    if (!timebase.denom)
        mach_timebase_info(&timebase);
}

void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    timebase_init();
    *ns = abs * timebase.numer / timebase.denom;
}

void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    timebase_init();
    *deadline = mach_absolute_time() +
                (uint64_t)interval * scale * timebase.denom / timebase.numer;
}

/* ---- locks ---- */

struct host_lock {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

static struct host_lock *host_lock_alloc(bool recursive)
{
    struct host_lock *l = calloc(1, sizeof(*l));
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, recursive ? PTHREAD_MUTEX_RECURSIVE :
                                                 PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&l->mutex, &attr);
    pthread_cond_init(&l->cond, NULL);
    return l;
}

static void host_lock_free(struct host_lock *l)
{
    pthread_mutex_destroy(&l->mutex);
    pthread_cond_destroy(&l->cond);
    free(l);
}

static void host_lock_lock(struct host_lock *l, const char *what)
{
    int err = pthread_mutex_lock(&l->mutex);

    if (err) {
        fprintf(stderr, "HOST: %s lock failed: %s (self-deadlock?)\n", what, strerror(err));
        abort();
    }
}

static void host_lock_unlock(struct host_lock *l, const char *what)
{
    int err = pthread_mutex_unlock(&l->mutex);

    if (err) {
        fprintf(stderr, "HOST: %s unlock failed: %s (not the owner?)\n", what, strerror(err));
        abort();
    }
}

void *IOLockAlloc(void) { return host_lock_alloc(false); }
void IOLockFree(void *l) { host_lock_free(l); }
void IOLockLock(void *l) { host_lock_lock(l, "IOLock"); }
void IOLockUnlock(void *l) { host_lock_unlock(l, "IOLock"); }
int IOLockTryLock(void *l) { return pthread_mutex_trylock(&((struct host_lock *)l)->mutex) == 0; }

/* One condition per lock: every waiter on the lock wakes and re-checks, which
 * all callers tolerate (they sleep in loops). */
int IOLockSleep(void *lock, void *event, unsigned interruptible)
{
    struct host_lock *l = lock;

    (void)event; (void)interruptible;
    pthread_cond_wait(&l->cond, &l->mutex);
    return 0;
}

void IOLockWakeup(void *lock, void *event, int one)
{
    struct host_lock *l = lock;

    (void)event; (void)one;
    pthread_cond_broadcast(&l->cond);
}

void *IORecursiveLockAlloc(void) { return host_lock_alloc(true); }
void IORecursiveLockFree(void *l) { host_lock_free(l); }
void IORecursiveLockLock(void *l) { host_lock_lock(l, "IORecursiveLock"); }
void IORecursiveLockUnlock(void *l) { host_lock_unlock(l, "IORecursiveLock"); }

void *IOSimpleLockAlloc(void) { return host_lock_alloc(false); }
void IOSimpleLockFree(void *l) { host_lock_free(l); }
void IOSimpleLockLock(void *l) { host_lock_lock(l, "IOSimpleLock"); }
void IOSimpleLockUnlock(void *l) { host_lock_unlock(l, "IOSimpleLock"); }

/* ---- kernel threads ---- */

struct host_thread_start {
    void (*fn)(void *, int);
    void *arg;
};

static void *host_thread_main(void *p)
{
    struct host_thread_start s = *(struct host_thread_start *)p;

    free(p);
    s.fn(s.arg, 0);
    return NULL;
}

int kernel_thread_start(void (*fn)(void *, int), void *arg, void **thread)
{
    struct host_thread_start *s = malloc(sizeof(*s));
    pthread_t t;

    s->fn = fn;
    s->arg = arg;
    if (pthread_create(&t, NULL, host_thread_main, s))
        return 5; /* KERN_FAILURE */
    pthread_detach(t);
    *thread = (void *)t;
    return 0;
}

void *current_thread(void) { return (void *)pthread_self(); }
void thread_deallocate(void *thread) { (void)thread; }
int thread_terminate(void *thread) { (void)thread; pthread_exit(NULL); }

/* ---- thread calls ---- */

/*
 * Each submission starts a thread that sleeps until the deadline and then
 * runs the function unless the call was cancelled or resubmitted meanwhile
 * (generation check). Calls are never freed, so a sleeper can always look at
 * its call; this is a test program.
 */
struct host_call {
    pthread_mutex_t mutex;
    void (*fn)(void *, void *);
    void *param0;
    unsigned long generation;
    bool pending;
};

struct host_call_run {
    struct host_call *call;
    unsigned long generation;
    uint64_t deadline;
};

void *thread_call_allocate(void (*fn)(void *, void *), void *param0)
{
    struct host_call *c = calloc(1, sizeof(*c));

    pthread_mutex_init(&c->mutex, NULL);
    c->fn = fn;
    c->param0 = param0;
    return c;
}

static void *host_call_main(void *p)
{
    struct host_call_run r = *(struct host_call_run *)p;
    bool run;

    free(p);
    while (mach_absolute_time() < r.deadline)
        usleep(500);

    pthread_mutex_lock(&r.call->mutex);
    run = r.call->pending && r.call->generation == r.generation;
    if (run)
        r.call->pending = false;
    pthread_mutex_unlock(&r.call->mutex);

    if (run)
        r.call->fn(r.call->param0, NULL);
    return NULL;
}

int thread_call_enter_delayed(void *call, uint64_t deadline)
{
    struct host_call *c = call;
    struct host_call_run *r = malloc(sizeof(*r));
    pthread_t t;
    bool was_pending;

    pthread_mutex_lock(&c->mutex);
    was_pending = c->pending;
    c->pending = true;
    r->call = c;
    r->generation = ++c->generation;
    r->deadline = deadline;
    pthread_mutex_unlock(&c->mutex);

    pthread_create(&t, NULL, host_call_main, r);
    pthread_detach(t);
    return was_pending;
}

int thread_call_enter(void *call) { return thread_call_enter_delayed(call, 0); }

int thread_call_cancel(void *call)
{
    struct host_call *c = call;
    bool was_pending;

    pthread_mutex_lock(&c->mutex);
    was_pending = c->pending;
    c->pending = false;
    c->generation++;
    pthread_mutex_unlock(&c->mutex);
    return was_pending;
}

int thread_call_free(void *call)
{
    thread_call_cancel(call);
    return 1;
}
