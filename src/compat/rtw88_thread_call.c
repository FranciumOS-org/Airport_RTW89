/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Synchronous cancellation built on the public Mach KPI. The pending flag is
 * set before submission so cancellation also waits for dequeued callbacks
 * which have not yet entered our trampoline. No private KPI is referenced.
 */
#include "rtw88_compat.h"
#undef thread_call_allocate
#undef thread_call_enter
#undef thread_call_enter_delayed
#undef thread_call_cancel
#undef thread_call_cancel_wait
#undef thread_call_free

struct rtw88_call {
    IOLock *lock;
    thread_call_t native;
    thread_call_func_t function;
    thread_call_param_t argument;
    bool pending;
    bool canceling;
    unsigned int running;
};

static void rtw88_call_dispatch(thread_call_param_t p0, thread_call_param_t p1)
{
    struct rtw88_call *c = (struct rtw88_call *)p0;
    IOLockLock(c->lock);
    c->pending = false;
    c->running++;
    bool execute = !c->canceling;
    IOLockUnlock(c->lock);
    if (execute) c->function(c->argument, p1);
    IOLockLock(c->lock);
    c->running--;
    IOLockWakeup(c->lock, c, false);
    IOLockUnlock(c->lock);
}

thread_call_t rtw88_thread_call_allocate(thread_call_func_t fn, thread_call_param_t arg)
{
    if (!fn) return NULL;
    struct rtw88_call *c = IOMalloc(sizeof(*c));
    if (!c) return NULL;
    bzero(c, sizeof(*c));
    c->lock = IOLockAlloc();
    if (!c->lock) { IOFree(c, sizeof(*c)); return NULL; }
    c->function = fn;
    c->argument = arg;
    c->native = thread_call_allocate(rtw88_call_dispatch, c);
    if (!c->native) { IOLockFree(c->lock); IOFree(c, sizeof(*c)); return NULL; }
    return (thread_call_t)c;
}

static boolean_t rtw88_call_submit(thread_call_t handle, uint64_t deadline, bool delayed)
{
    if (!handle) return false;
    struct rtw88_call *c = (struct rtw88_call *)handle;
    IOLockLock(c->lock);
    boolean_t already = c->pending || c->canceling;
    /* Coalesce while a callback is queued or dequeued but not entered.
     * Retiming is safe only if native cancellation removes the pending call. */
    bool submit = !c->canceling && !c->pending;
    if (!c->canceling && c->pending && delayed)
        submit = thread_call_cancel(c->native);
    if (submit) {
        c->pending = true;
        if (delayed) thread_call_enter_delayed(c->native, deadline);
        else thread_call_enter(c->native);
    }
    IOLockUnlock(c->lock);
    return already;
}

boolean_t rtw88_thread_call_enter(thread_call_t h) { return rtw88_call_submit(h, 0, false); }
boolean_t rtw88_thread_call_enter_delayed(thread_call_t h, uint64_t d) { return rtw88_call_submit(h, d, true); }

boolean_t rtw88_thread_call_cancel(thread_call_t handle)
{
    if (!handle) return false;
    struct rtw88_call *c = (struct rtw88_call *)handle;
    IOLockLock(c->lock);
    boolean_t canceled = thread_call_cancel(c->native);
    if (canceled) c->pending = false;
    IOLockWakeup(c->lock, c, false);
    IOLockUnlock(c->lock);
    return canceled;
}

boolean_t rtw88_thread_call_cancel_wait(thread_call_t handle)
{
    if (!handle) return false;
    struct rtw88_call *c = (struct rtw88_call *)handle;
    IOLockLock(c->lock);
    c->canceling = true;
    boolean_t canceled = thread_call_cancel(c->native);
    if (canceled) c->pending = false;
    while (c->pending || c->running)
        IOLockSleep(c->lock, c, THREAD_UNINT);
    c->canceling = false;
    IOLockUnlock(c->lock);
    return canceled;
}

boolean_t rtw88_thread_call_free(thread_call_t handle)
{
    if (!handle) return false;
    struct rtw88_call *c = (struct rtw88_call *)handle;
    rtw88_thread_call_cancel_wait(handle);
    if (!thread_call_free(c->native)) return false;
    IOLockFree(c->lock);
    IOFree(c, sizeof(*c));
    return true;
}
