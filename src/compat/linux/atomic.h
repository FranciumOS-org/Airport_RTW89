/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_ATOMIC_H
#define _RTW88_COMPAT_ATOMIC_H

#include "types.h"

typedef struct { volatile int counter; } atomic_t;
typedef struct { volatile long counter; } atomic_long_t;

#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(const atomic_t *v)
{
    return __sync_fetch_and_add((volatile int *)&v->counter, 0);
}

static inline void atomic_set(atomic_t *v, int i)
{
    v->counter = i;
    __sync_synchronize();
}

static inline int atomic_inc(atomic_t *v)
{
    return __sync_add_and_fetch(&v->counter, 1);
}

static inline int atomic_dec(atomic_t *v)
{
    return __sync_sub_and_fetch(&v->counter, 1);
}

static inline int atomic_inc_return(atomic_t *v)
{
    return __sync_add_and_fetch(&v->counter, 1);
}

static inline int atomic_dec_return(atomic_t *v)
{
    return __sync_sub_and_fetch(&v->counter, 1);
}

static inline int atomic_dec_and_test(atomic_t *v)
{
    return __sync_sub_and_fetch(&v->counter, 1) == 0;
}

static inline int atomic_inc_and_test(atomic_t *v)
{
    return __sync_add_and_fetch(&v->counter, 1) == 0;
}

static inline int atomic_add_return(int i, atomic_t *v)
{
    return __sync_add_and_fetch(&v->counter, i);
}

static inline int atomic_sub_return(int i, atomic_t *v)
{
    return __sync_sub_and_fetch(&v->counter, i);
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int new_val)
{
    return __sync_val_compare_and_swap(&v->counter, old, new_val);
}

static inline long atomic_long_read(const atomic_long_t *v)
{
    return __sync_fetch_and_add((volatile long *)&v->counter, 0);
}

static inline void atomic_long_set(atomic_long_t *v, long i)
{
    v->counter = i;
    __sync_synchronize();
}

static inline long atomic_long_inc(atomic_long_t *v)
{
    return __sync_add_and_fetch(&v->counter, 1L);
}

static inline long atomic_long_dec(atomic_long_t *v)
{
    return __sync_sub_and_fetch(&v->counter, 1L);
}

static inline unsigned long xchg(volatile unsigned long *ptr, unsigned long val)
{
    return __sync_lock_test_and_set(ptr, val);
}

#define cmpxchg(ptr, old, new_val) \
    __sync_val_compare_and_swap(ptr, old, new_val)

int atomic_dec_if_positive(atomic_t *);

#endif /* _RTW88_COMPAT_ATOMIC_H */
