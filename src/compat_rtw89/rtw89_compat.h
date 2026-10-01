/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * rtw89 additions on top of the inherited rtw88 compat layer.
 * Force-included after rtw88_compat.h for every rtw89 driver file.
 * Contents come from tools/api_gap.py (docs/api-gap.md); keep sections in that order.
 */
#ifndef _RTW89_COMPAT_H
#define _RTW89_COMPAT_H

#include "rtw88_compat.h"

/* ------------------------------------------------------------------ */
/*  Compiler / annotation macros                                       */
/* ------------------------------------------------------------------ */

#ifndef __counted_by
#define __counted_by(member)
#endif
#ifndef __acquires
#define __acquires(x)
#endif
#ifndef __releases
#define __releases(x)
#endif
#ifndef __cond_acquires
#define __cond_acquires(x)
#endif
#ifndef __must_hold
#define __must_hold(x)
#endif
#ifndef __stringify
#define __stringify_1(x) #x
#define __stringify(x) __stringify_1(x)
#endif
#ifndef typecheck
#define typecheck(type, x) \
    ({ type __dummy; __typeof__(x) __dummy2; (void)(&__dummy == &__dummy2); 1; })
#endif
#ifndef container_of_const
#define container_of_const(ptr, type, member) container_of(ptr, type, member)
#endif

/*
 * Scope-based locking. rtw89 uses exactly two forms:
 *   guard(rcu)();                          -- RCU read side is a no-op here
 *   scoped_guard(spinlock_irqsave, &lock) { ... return; ... }
 * The spinlock form must unlock on early return, hence the cleanup attribute.
 */
#define guard(_name) __rtw89_guard_##_name
#define __rtw89_guard_rcu() do { } while (0)

typedef struct { spinlock_t *lock; unsigned long flags; } __rtw89_sg_spinlock_irqsave_t;
static inline __rtw89_sg_spinlock_irqsave_t __rtw89_sg_spinlock_irqsave_enter(spinlock_t *l)
{
    __rtw89_sg_spinlock_irqsave_t g = { l, 0 };
    spin_lock_irqsave(g.lock, g.flags);
    return g;
}
static inline void __rtw89_sg_spinlock_irqsave_exit(__rtw89_sg_spinlock_irqsave_t *g)
{
    spin_unlock_irqrestore(g->lock, g->flags);
}
#define scoped_guard(_name, _lock)                                  \
    for (__rtw89_sg_##_name##_t __sg                                \
             __attribute__((cleanup(__rtw89_sg_##_name##_exit))) =  \
             __rtw89_sg_##_name##_enter(_lock),                     \
             *__sg_once = (__rtw89_sg_##_name##_t *)1;              \
         __sg_once; __sg_once = NULL)

/* ------------------------------------------------------------------ */
/*  Bit / field helpers                                                 */
/* ------------------------------------------------------------------ */

#define upper_32_bits(n) ((u32)(((u64)(n)) >> 32))
#define lower_32_bits(n) ((u32)((u64)(n) & 0xffffffffULL))
#ifndef BITS_PER_BYTE
#define BITS_PER_BYTE 8
#endif
#define BITS_PER_TYPE(type) (sizeof(type) * BITS_PER_BYTE)
#define BITS_TO_BYTES(nr) (((nr) + BITS_PER_BYTE - 1) / BITS_PER_BYTE)

/* FIELD_PREP for constant masks in initialisers; same arithmetic here. */
#define FIELD_PREP_CONST(_mask, _val) FIELD_PREP(_mask, _val)
/* Extract the field, then sign-extend from the field's width. */
#define FIELD_GET_SIGNED(_mask, _val)                               \
    ((s32)((s64)((u64)FIELD_GET(_mask, _val)                        \
                 << (64 - __builtin_popcountll((u64)(_mask))))      \
           >> (64 - __builtin_popcountll((u64)(_mask)))))
/* Non-constant-mask variants (linux/bitfield.h field_get/field_prep). */
#define field_get(_mask, _val) FIELD_GET(_mask, _val)
#define field_prep(_mask, _val) FIELD_PREP(_mask, _val)

static inline u8 u8_replace_bits(u8 old, u8 val, u8 mask)
{
    return (old & (u8)~mask) | u8_encode_bits(val, mask);
}
static inline u16 u16_replace_bits(u16 old, u16 val, u16 mask)
{
    return (old & (u16)~mask) | u16_encode_bits(val, mask);
}
static inline u32 u32_replace_bits(u32 old, u32 val, u32 mask)
{
    return (old & ~mask) | u32_encode_bits(val, mask);
}
static inline u64 u64_get_bits(u64 v, u64 mask)
{
    return (u64)FIELD_GET(mask, v);
}
static inline u64 u64_replace_bits(u64 old, u64 val, u64 mask)
{
    return (old & ~mask) | u64_encode_bits(val, mask);
}
static inline __le16 le16_encode_bits(u16 v, u16 mask)
{
    return (__le16)u16_encode_bits(v, mask);
}
static inline void le16p_replace_bits(__le16 *p, u16 val, u16 mask)
{
    *p = (__le16)u16_replace_bits((u16)*p, val, mask);
}
static inline __le32 le32_replace_bits(__le32 old, u32 val, u32 mask)
{
    return (__le32)u32_replace_bits((u32)old, val, mask);
}
static inline void le16_add_cpu(__le16 *var, u16 val)
{
    *var = (__le16)((u16)*var + val);
}

static inline s32 sign_extend32(u32 value, int index)
{
    u8 shift = 31 - index;
    return (s32)(value << shift) >> shift;
}
/* Macros, not functions: rtw89 uses hweight32() inside static_assert(). */
#undef hweight8
#undef hweight16
#undef hweight32
#define hweight8(w)  ((unsigned int)__builtin_popcount((u8)(w)))
#define hweight16(w) ((unsigned int)__builtin_popcount((u16)(w)))
#define hweight32(w) ((unsigned int)__builtin_popcount((u32)(w)))

/* ------------------------------------------------------------------ */
/*  Bitmaps                                                             */
/* ------------------------------------------------------------------ */

#define __RTW89_BITMAP_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static inline void bitmap_fill(unsigned long *dst, unsigned int nbits)
{
    memset(dst, 0xff, __RTW89_BITMAP_LONGS(nbits) * sizeof(unsigned long));
}
static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits)
{
    memcpy(dst, src, __RTW89_BITMAP_LONGS(nbits) * sizeof(unsigned long));
}
static inline void bitmap_or(unsigned long *dst, const unsigned long *a,
                             const unsigned long *b, unsigned int nbits)
{
    for (unsigned int i = 0; i < __RTW89_BITMAP_LONGS(nbits); i++)
        dst[i] = a[i] | b[i];
}
static inline bool bitmap_empty(const unsigned long *src, unsigned int nbits)
{
    for (unsigned int i = 0; i < nbits; i++)
        if (src[i / BITS_PER_LONG] & (1UL << (i % BITS_PER_LONG)))
            return false;
    return true;
}
static inline unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits)
{
    unsigned int w = 0;
    for (unsigned int i = 0; i < nbits; i++)
        w += !!(src[i / BITS_PER_LONG] & (1UL << (i % BITS_PER_LONG)));
    return w;
}
static inline bool __test_and_set_bit(unsigned long nr, unsigned long *addr)
{
    unsigned long mask = 1UL << (nr % BITS_PER_LONG);
    unsigned long *p = addr + nr / BITS_PER_LONG;
    bool old = !!(*p & mask);
    *p |= mask;
    return old;
}

/* ------------------------------------------------------------------ */
/*  Math / misc                                                         */
/* ------------------------------------------------------------------ */

#define umin(a, b) min((unsigned long long)(a), (unsigned long long)(b))
#define umax(a, b) max((unsigned long long)(a), (unsigned long long)(b))
#define roundup_u64(x, y) ((((u64)(x) + (u64)(y) - 1) / (u64)(y)) * (u64)(y))

static inline u64 div_u64_rem(u64 dividend, u32 divisor, u32 *remainder)
{
    *remainder = (u32)(dividend % divisor);
    return dividend / divisor;
}

static inline void *memchr_inv(const void *start, int c, size_t bytes)
{
    const u8 *p = (const u8 *)start;
    for (size_t i = 0; i < bytes; i++)
        if (p[i] != (u8)c)
            return (void *)(p + i);
    return NULL;
}

/* Insertion sort — rtw89 only sorts short tables (ACPI/SAR entries). */
static inline void sort(void *base, size_t num, size_t size,
                        int (*cmp)(const void *, const void *),
                        void (*swap_fn)(void *, void *, int))
{
    u8 tmp[64];
    u8 *b = (u8 *)base;
    (void)swap_fn;
    if (size > sizeof(tmp))
        return;
    for (size_t i = 1; i < num; i++) {
        memcpy(tmp, b + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(b + (j - 1) * size, tmp) > 0) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
}

u32 rtw89_compat_random_u32(void);
#define get_random_u32() rtw89_compat_random_u32()

static inline const char *str_yes_no(bool v) { return v ? "yes" : "no"; }
static inline const char *str_enable_disable(bool v) { return v ? "enable" : "disable"; }
static inline const char *str_enabled_disabled(bool v) { return v ? "enabled" : "disabled"; }
static inline const char *str_on_off(bool v) { return v ? "on" : "off"; }

/* ------------------------------------------------------------------ */
/*  Flexible arrays / sizes                                             */
/* ------------------------------------------------------------------ */

#define struct_size(p, member, count) \
    (sizeof(*(p)) + sizeof(*(p)->member) * (size_t)(count))
#define struct_size_t(type, member, count) \
    (sizeof(type) + sizeof(((type *)0)->member[0]) * (size_t)(count))
#define flex_array_size(p, member, count) \
    (sizeof(*(p)->member) * (size_t)(count))
#define DECLARE_FLEX_ARRAY(type, name) \
    struct { struct { } __empty_##name; type name[]; }
#define kzalloc_flex(p, member, count, gfp) kzalloc(struct_size(p, member, count), gfp)
#define kzalloc_objs(p, count, gfp) kcalloc(count, sizeof(*(p)), gfp)

/* ------------------------------------------------------------------ */
/*  Atomics / refcount                                                  */
/* ------------------------------------------------------------------ */

typedef struct { volatile s64 counter; } atomic64_t;
static inline void atomic64_set(atomic64_t *v, s64 i) { __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_read(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_inc_return(atomic64_t *v) { return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }

typedef struct { atomic_t refs; } refcount_t;
static inline bool refcount_inc_not_zero(refcount_t *r)
{
    int old = __atomic_load_n(&r->refs.counter, __ATOMIC_RELAXED);
    while (old) {
        if (__atomic_compare_exchange_n(&r->refs.counter, &old, old + 1, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Sync / memory                                                       */
/* ------------------------------------------------------------------ */

/* No softirqs on macOS: bottom halves run from thread calls already serialized
 * by the driver's own locks. */
#define local_bh_disable() do { } while (0)
#define local_bh_enable()  do { } while (0)
#define rcu_access_pointer(p) (p)
#define kfree_rcu(ptr, field) kfree(ptr)
/* devm_*: rtw89 frees these explicitly on the paths we use; no device-managed lifetime. */
#define devm_kcalloc(dev, n, size, gfp) kcalloc(n, size, gfp)
#define devm_kfree(dev, p) kfree(p)

static inline void list_splice_tail(struct list_head *list, struct list_head *head)
{
    if (list->next == list)
        return;
    struct list_head *first = list->next, *last = list->prev, *at = head->prev;
    first->prev = at;
    at->next = first;
    last->next = head;
    head->prev = last;
}

/* ------------------------------------------------------------------ */
/*  Time                                                                */
/* ------------------------------------------------------------------ */

typedef s64 ktime_t;
u64 rtw89_compat_ktime_ns(void);
#define ktime_get() ((ktime_t)rtw89_compat_ktime_ns())
#define ktime_get_boottime_ns() rtw89_compat_ktime_ns()
static inline s64 ktime_us_delta(ktime_t later, ktime_t earlier) { return (later - earlier) / 1000; }
static inline s64 ktime_ms_delta(ktime_t later, ktime_t earlier) { return (later - earlier) / 1000000; }

/* ------------------------------------------------------------------ */
/*  Network headers                                                     */
/* ------------------------------------------------------------------ */

#define htons(x) ((u16)__builtin_bswap16((u16)(x)))

/* ------------------------------------------------------------------ */
/*  wiphy work (runs with the wiphy mutex held)                         */
/* ------------------------------------------------------------------ */

/*
 * Same model as net/wireless/core.c: each wiphy has one runner work item and a
 * list of pending wiphy_works. The runner takes the wiphy mutex and runs them one
 * at a time. Because works only run under the mutex, cancel/flush (which rtw89
 * calls with the mutex held) just unlink or run inline � they never wait, so they
 * cannot deadlock against a work that is blocked on the mutex.
 */
struct wiphy_work;
typedef void (*wiphy_work_func_t)(struct wiphy *, struct wiphy_work *);

struct wiphy_work {
    struct list_head entry;   /* on wiphy->wiphy_work_list while pending */
    wiphy_work_func_t func;
};

struct wiphy_delayed_work {
    struct wiphy_work work;
    struct wiphy *wiphy;
    struct delayed_work dwork; /* timer stage; its handler queues @work */
};

void rtw89_compat_wiphy_init(struct wiphy *wiphy);
void rtw89_compat_wiphy_exit(struct wiphy *wiphy);

static inline void wiphy_lock(struct wiphy *wiphy) { mutex_lock(&wiphy->mtx); }
static inline void wiphy_unlock(struct wiphy *wiphy) { mutex_unlock(&wiphy->mtx); }
#define lockdep_assert_wiphy(wiphy) do { (void)(wiphy); } while (0)

static inline void wiphy_work_init(struct wiphy_work *work, wiphy_work_func_t func)
{
    INIT_LIST_HEAD(&work->entry);
    work->func = func;
}
void wiphy_work_queue(struct wiphy *wiphy, struct wiphy_work *work);
void wiphy_work_cancel(struct wiphy *wiphy, struct wiphy_work *work);
void wiphy_work_flush(struct wiphy *wiphy, struct wiphy_work *work);

void rtw89_compat_wiphy_delayed_work_timer(struct work_struct *w);
static inline void wiphy_delayed_work_init(struct wiphy_delayed_work *dwork, wiphy_work_func_t func)
{
    wiphy_work_init(&dwork->work, func);
    dwork->wiphy = NULL;
    INIT_DELAYED_WORK(&dwork->dwork, rtw89_compat_wiphy_delayed_work_timer);
}
void wiphy_delayed_work_queue(struct wiphy *wiphy, struct wiphy_delayed_work *dwork,
                              unsigned long delay);
void wiphy_delayed_work_cancel(struct wiphy *wiphy, struct wiphy_delayed_work *dwork);
void wiphy_delayed_work_flush(struct wiphy *wiphy, struct wiphy_delayed_work *dwork);

/* ------------------------------------------------------------------ */
/*  Platform: ACPI / DMI — no policy on macOS yet                       */
/* ------------------------------------------------------------------ */

#include <linux/acpi.h>
#include <linux/dmi.h>

#endif /* _RTW89_COMPAT_H */
