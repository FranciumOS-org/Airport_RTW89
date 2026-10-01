/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Kernel-safe forward declarations for IOKit / XNU C APIs.
 * Included by compat headers compiled as C with -mkernel.
 * When KERNEL is defined (C++ kext build), MacKernelSDK provides the real
 * definitions; we only emit stubs for the Linux driver C files.
 */
#ifndef _RTW88_IOKIT_SHIM_H
#define _RTW88_IOKIT_SHIM_H

#include <stddef.h>
#include <stdint.h>

/* ---- IOMalloc / IOFree / IOLog / IODelay / IOSleep ---- */
extern void    *IOMalloc(size_t size);
extern void     IOFree(void *address, size_t size);
extern void     IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
extern void     IODelay(unsigned microseconds);
extern void     IOSleep(unsigned milliseconds);

#ifndef KERNEL
/* These are provided by MacKernelSDK/IOLocks.h when KERNEL is defined.
 * For C driver files compiled without -DKERNEL we provide our own stubs. */

/* ---- IOSimpleLock (interrupt-safe spinlock) ---- */
typedef struct IOSimpleLock IOSimpleLock;
typedef unsigned long IOInterruptState;
extern IOSimpleLock    *IOSimpleLockAlloc(void);
extern void             IOSimpleLockFree(IOSimpleLock *lock);
extern void             IOSimpleLockLock(IOSimpleLock *lock);
extern void             IOSimpleLockUnlock(IOSimpleLock *lock);
extern IOInterruptState IOSimpleLockLockDisableInterrupt(IOSimpleLock *lock);
extern void             IOSimpleLockUnlockEnableInterrupt(IOSimpleLock *lock,
                                                          IOInterruptState state);

/* ---- IOLock (sleepable mutex) ---- */
typedef struct IOLock IOLock;
#ifndef THREAD_INTERRUPTIBLE
#define THREAD_INTERRUPTIBLE 1
#define THREAD_UNINT         0
#endif
extern IOLock  *IOLockAlloc(void);
extern void     IOLockFree(IOLock *lock);
extern void     IOLockLock(IOLock *lock);
extern void     IOLockUnlock(IOLock *lock);
extern int      IOLockTryLock(IOLock *lock);
extern int      IOLockSleep(IOLock *lock, void *event, unsigned interruptible);
extern void     IOLockWakeup(IOLock *lock, void *event, int oneThread);

/* ---- IORecursiveLock (recursive/reentrant mutex) ---- */
typedef struct IORecursiveLock IORecursiveLock;
extern IORecursiveLock *IORecursiveLockAlloc(void);
extern void             IORecursiveLockFree(IORecursiveLock *lock);
extern void             IORecursiveLockLock(IORecursiveLock *lock);
extern void             IORecursiveLockUnlock(IORecursiveLock *lock);
extern int              IORecursiveLockTryLock(IORecursiveLock *lock);
extern int              IORecursiveLockHaveLock(IORecursiveLock *lock);

/* ---- Kernel threads ---- */
typedef struct thread *thread_t;
typedef int wait_result_t;
typedef void (*thread_continue_t)(void *param, wait_result_t wr);
typedef int kern_return_t;
#ifndef KERN_SUCCESS
#define KERN_SUCCESS 0
#endif
extern kern_return_t kernel_thread_start(thread_continue_t continuation,
                                          void *parameter, thread_t *new_thread);
extern void          thread_deallocate(thread_t thread);
extern thread_t      current_thread(void);
extern void          thread_terminate(thread_t thread);

/* ---- thread_call (deferred one-shot work, used for timers) ---- */
#include <kern/thread_call.h>

#else /* KERNEL defined — use real XNU types from MacKernelSDK */

#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <mach/thread_act.h>

#ifndef THREAD_INTERRUPTIBLE
#define THREAD_INTERRUPTIBLE 1
#define THREAD_UNINT         0
#endif

#endif /* !KERNEL */

/* ---- Timing (available in both C and C++ kext builds) ---- */
extern uint64_t mach_absolute_time(void);

extern void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor,
                                        uint64_t *result);
#define kMillisecondScale 1000000u
#define kMicrosecondScale 1000u

#include "rtw88_thread_call.h"

#endif /* _RTW88_IOKIT_SHIM_H */
