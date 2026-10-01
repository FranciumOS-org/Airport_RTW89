/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_MUTEX_H
#define _RTW88_COMPAT_MUTEX_H

#include "types.h"
#include "../iokit_shim.h"

/* struct mutex — IOLock sleepable mutex */
struct mutex {
    IOLock *m;
};

/* Static initializer: lock pointer is NULL; must call mutex_init() before use */
#define DEFINE_MUTEX(name) struct mutex name = { .m = NULL }

static inline void mutex_init(struct mutex *lock)
{
    lock->m = IOLockAlloc();
}

static inline void mutex_destroy(struct mutex *lock)
{
    if (lock->m) { IOLockFree(lock->m); lock->m = NULL; }
}

static inline void mutex_lock(struct mutex *lock)
{
    IOLockLock(lock->m);
}

static inline int mutex_trylock(struct mutex *lock)
{
    return IOLockTryLock(lock->m);
}

static inline void mutex_unlock(struct mutex *lock)
{
    IOLockUnlock(lock->m);
}

static inline int mutex_is_locked(struct mutex *lock)
{
    int r = IOLockTryLock(lock->m);
    if (r) IOLockUnlock(lock->m);
    return !r;
}

/*
 * Semaphore — implemented with IOLock + counter + IOLockSleep/IOLockWakeup.
 */
struct semaphore {
    IOLock      *lock;
    volatile int count;
};

static inline void sema_init(struct semaphore *sem, int val)
{
    sem->lock  = IOLockAlloc();
    sem->count = val;
}

static inline void down(struct semaphore *sem)
{
    IOLockLock(sem->lock);
    while (sem->count <= 0)
        IOLockSleep(sem->lock, (void *)&sem->count, THREAD_UNINT);
    sem->count--;
    IOLockUnlock(sem->lock);
}

static inline int down_trylock(struct semaphore *sem)
{
    int ret = -1;
    IOLockLock(sem->lock);
    if (sem->count > 0) { sem->count--; ret = 0; }
    IOLockUnlock(sem->lock);
    return ret;
}

static inline void up(struct semaphore *sem)
{
    IOLockLock(sem->lock);
    sem->count++;
    IOLockWakeup(sem->lock, (void *)&sem->count, true);
    IOLockUnlock(sem->lock);
}

#endif /* _RTW88_COMPAT_MUTEX_H */
