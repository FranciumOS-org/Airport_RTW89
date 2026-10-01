/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_COMPLETION_H
#define _RTW88_COMPAT_COMPLETION_H

#include "types.h"
#include "jiffies.h"
#include "../iokit_shim.h"

/*
 * completion — backed by IOLock condition variables.
 *
 * All waiting and waking uses IOLockSleep / IOLockWakeup on the completion's
 * IOLock.  This is safe from any calling context: the interrupt thread_fn now
 * runs in a dedicated workqueue thread (not inside the IOWorkLoop gate), so
 * there is never a need for gate-aware commandSleep / commandWakeup.
 */

struct completion {
    volatile unsigned int done;
    IOLock               *lock;
};

/* Declare only — must call init_completion() before first use */
#define DECLARE_COMPLETION(name) struct completion name = { .done = 0, .lock = NULL }

static inline void init_completion(struct completion *c)
{
    c->done = 0;
    c->lock = IOLockAlloc();
}

static inline void complete(struct completion *c)
{
    IOLockLock(c->lock);
    c->done++;
    IOLockWakeup(c->lock, (void *)c, false);
    IOLockUnlock(c->lock);
}

static inline void complete_all(struct completion *c)
{
    IOLockLock(c->lock);
    c->done = ~0u;
    IOLockWakeup(c->lock, (void *)c, false);
    IOLockUnlock(c->lock);
}

static inline void wait_for_completion(struct completion *c)
{
    IOLockLock(c->lock);
    while (!c->done) {
        IOLockSleep(c->lock, (void *)c, THREAD_UNINT);
    }
    if (c->done != ~0u) c->done--;
    IOLockUnlock(c->lock);
}

/* Returns remaining jiffies (1) on completion, 0 on timeout */
static inline unsigned long wait_for_completion_timeout(struct completion *c,
                                                         unsigned long timeout_j)
{
    unsigned long deadline = jiffies + timeout_j;
    IOLockLock(c->lock);
    while (!c->done) {
        IOLockUnlock(c->lock);
        if (time_after_eq(jiffies, deadline)) return 0;
        IOSleep(1);
        IOLockLock(c->lock);
    }
    if (c->done != ~0u) c->done--;
    IOLockUnlock(c->lock);
    return 1;
}


static inline int completion_done(struct completion *c)
{
    return c->done > 0;
}

static inline void reinit_completion(struct completion *c)
{
    IOLockLock(c->lock);
    c->done = 0;
    IOLockUnlock(c->lock);
}

#endif /* _RTW88_COMPAT_COMPLETION_H */
