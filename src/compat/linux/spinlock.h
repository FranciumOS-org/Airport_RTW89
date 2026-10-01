/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_SPINLOCK_H
#define _RTW88_COMPAT_SPINLOCK_H

#include "types.h"
#include "../iokit_shim.h"

/*
 * spinlock_t — backed by IORecursiveLock.
 *
 * The Linux driver mixes spin_lock_bh() and spin_lock_irqsave() on the same
 * lock (irq_lock, hwirq_lock) across callchains that re-enter the same lock on
 * the same thread:
 *
 *   rtw_pci_interrupt_threadfn() {
 *       spin_lock_bh(&irq_lock);           // outer acquire
 *       rtw_pci_tx_isr() {
 *           rtw_pci_tx_wake_queues() {
 *               spin_lock_bh(&irq_lock);   // re-entrant acquire on same thread!
 *               ...
 *               spin_unlock_bh(&irq_lock);
 *           }
 *       }
 *       spin_unlock_bh(&irq_lock);         // outer release
 *   }
 *
 * In Linux this works because spin_lock_irqsave/bh variants know their
 * nesting context and don't deadlock within a single thread.
 *
 * With IOLock (non-recursive) these would deadlock. IORecursiveLock allows
 * the same thread to acquire the same lock multiple times, matching Linux
 * spinlock re-entrancy semantics for our single-threaded interrupt model.
 */
typedef struct {
    IORecursiveLock *lock;
} spinlock_t;

static inline void spin_lock_init(spinlock_t *sl)
{
    sl->lock = IORecursiveLockAlloc();
}

static inline void spin_lock(spinlock_t *sl)
{
    IORecursiveLockLock(sl->lock);
}

static inline void spin_unlock(spinlock_t *sl)
{
    IORecursiveLockUnlock(sl->lock);
}

#define spin_lock_bh(sl)    spin_lock(sl)
#define spin_unlock_bh(sl)  spin_unlock(sl)
#define spin_lock_irq(sl)   spin_lock(sl)
#define spin_unlock_irq(sl) spin_unlock(sl)

typedef unsigned long rtw88_irq_flags_t;

#define spin_lock_irqsave(sl, flags) \
    do { (void)(flags); IORecursiveLockLock((sl)->lock); } while (0)

#define spin_unlock_irqrestore(sl, flags) \
    do { (void)(flags); IORecursiveLockUnlock((sl)->lock); } while (0)

/*
 * rwlock_t — plain IOLock (read/write parallelism not needed for rtw88).
 */
typedef struct {
    IOLock *lock;
} rwlock_t;

static inline void rwlock_init(rwlock_t *rw)    { rw->lock = IOLockAlloc(); }
static inline void read_lock(rwlock_t *rw)       { IOLockLock(rw->lock); }
static inline void read_unlock(rwlock_t *rw)     { IOLockUnlock(rw->lock); }
static inline void write_lock(rwlock_t *rw)      { IOLockLock(rw->lock); }
static inline void write_unlock(rwlock_t *rw)    { IOLockUnlock(rw->lock); }
#define read_lock_bh(rw)    read_lock(rw)
#define read_unlock_bh(rw)  read_unlock(rw)
#define write_lock_bh(rw)   write_lock(rw)
#define write_unlock_bh(rw) write_unlock(rw)

/* lockdep stubs */
#define lockdep_assert_held(l)      do {} while (0)
#define assert_spin_locked(l)       do {} while (0)
#define lockdep_set_class(l, k)     do {} while (0)

#endif /* _RTW88_COMPAT_SPINLOCK_H */
