/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * rtw89 additions on top of the inherited rtw88 compat layer.
 * Force-included after rtw88_compat.h for every rtw89 driver file.
 * Contents come from tools/api_gap.py (docs/api-gap.md); keep sections in that order.
 */
#ifndef _RTW89_COMPAT_H
#define _RTW89_COMPAT_H

#include "rtw88_compat.h"

/* ------------------------------------------------------------------ */
/*  Kconfig                                                             */
/* ------------------------------------------------------------------ */

/* Same trick as linux/kconfig.h: IS_ENABLED(CONFIG_X) is 1 iff CONFIG_X is defined to 1. */
#define __ARG_PLACEHOLDER_1 0,
#define __take_second_arg(__ignored, val, ...) val
#define ____is_defined(arg1_or_junk) __take_second_arg(arg1_or_junk 1, 0)
#define ___is_defined(val) ____is_defined(__ARG_PLACEHOLDER_##val)
#define __is_defined(x) ___is_defined(x)
#define IS_ENABLED(option) __is_defined(option)
#define IS_BUILTIN(option) __is_defined(option)
#define IS_REACHABLE(option) __is_defined(option)

#define CONFIG_CFG80211 1
#define CONFIG_MAC80211 1
/* Off: CONFIG_PM (wow.c excluded until sleep/wake), CONFIG_MAC80211_LEDS,
 * CONFIG_*_DEBUGFS, CONFIG_LOCKDEP, CONFIG_OF, CONFIG_NL80211_TESTMODE. */

/* ------------------------------------------------------------------ */
/*  Compiler / annotation macros                                       */
/* ------------------------------------------------------------------ */

#ifndef __counted_by
#define __counted_by(member)
#endif
#define __counted_by_le(member)
#define __counted_by_be(member)
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
#define __acquire(x) (void)0
#define __release(x) (void)0
#define __rcu
#define __percpu
#define __bitwise
#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif
#define __cleanup(func) __attribute__((__cleanup__(func)))
#ifndef __stringify
#define __stringify_1(x) #x
#define __stringify(x) __stringify_1(x)
#endif
#define ___PASTE(a, b) a##b
#define __PASTE(a, b) ___PASTE(a, b)
#define __UNIQUE_ID(prefix) __PASTE(__PASTE(__UNIQUE_ID_, prefix), __COUNTER__)
#ifndef typecheck
#define typecheck(type, x) \
    ({ type __dummy; __typeof__(x) __dummy2; (void)(&__dummy == &__dummy2); 1; })
#endif
#ifndef container_of_const
#define container_of_const(ptr, type, member) container_of(ptr, type, member)
#endif

/* Fixed-width types the uapi headers use. */
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;
typedef uint64_t __be64;

/*
 * Scope-based cleanup (linux/cleanup.h), reduced to what cfg80211.h and rtw89 use:
 *   DEFINE_GUARD(wiphy, ...)                -- cfg80211.h
 *   guard(rcu)();                           -- RCU read side is a no-op here
 *   scoped_guard(spinlock_irqsave, &lock) { ... return; ... }
 * A guard is a variable whose cleanup attribute runs the unlock on every exit
 * from the scope, early returns included.
 */
#define DEFINE_CLASS(_name, _type, _exit, _init, _init_args...)             \
typedef _type class_##_name##_t;                                            \
static inline void class_##_name##_destructor(_type *p)                     \
{ _type _T = *p; _exit; }                                                   \
static inline _type class_##_name##_constructor(_init_args)                 \
{ _type t = _init; return t; }

#define CLASS(_name, var)                                                   \
    class_##_name##_t var __cleanup(class_##_name##_destructor) =           \
        class_##_name##_constructor

#define DEFINE_GUARD(_name, _type, _lock, _unlock)                          \
    DEFINE_CLASS(_name, _type, if (_T) { _unlock; }, ({ _lock; _T; }), _type _T)

#define guard(_name) CLASS(_name, __UNIQUE_ID(guard))
#define scoped_guard(_name, args...)                                        \
    for (CLASS(_name, __sg_scope)(args), *__sg_done = NULL;                 \
         !__sg_done; __sg_done = (void *)1)

typedef struct { int unused; } class_rcu_t;
static inline void class_rcu_destructor(class_rcu_t *g) { (void)g; }
static inline class_rcu_t class_rcu_constructor(void)
{
    class_rcu_t g = { 0 };
    return g;
}

typedef struct { spinlock_t *lock; unsigned long flags; } class_spinlock_irqsave_t;
static inline void class_spinlock_irqsave_destructor(class_spinlock_irqsave_t *g)
{
    spin_unlock_irqrestore(g->lock, g->flags);
}
static inline class_spinlock_irqsave_t class_spinlock_irqsave_constructor(spinlock_t *l)
{
    class_spinlock_irqsave_t g = { l, 0 };
    spin_lock_irqsave(g.lock, g.flags);
    return g;
}

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

/* FIELD_PREP usable in static initialisers: builtins only, so it constant-folds
 * (the inherited FIELD_PREP calls the inline __ffs()). */
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define FIELD_PREP_CONST(_mask, _val) \
    (((__typeof__(_mask))(_val) << __bf_shf(_mask)) & (_mask))
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

#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define __RTW89_BITMAP_LONGS(n) BITS_TO_LONGS(n)

static inline void __set_bit(unsigned long nr, unsigned long *addr)
{
    addr[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG);
}
static inline void __clear_bit(unsigned long nr, unsigned long *addr)
{
    addr[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG));
}

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

/*
 * Linux's BUILD_BUG_ON fires only if the optimiser proves the condition, so
 * drivers use it on things that are not C constant expressions (rtw89 checks
 * members of a const table). Assert when clang can fold it, otherwise pass.
 */
#undef BUILD_BUG_ON
#define BUILD_BUG_ON(cond) \
    _Static_assert(__builtin_choose_expr(__builtin_constant_p(cond), !(cond), 1), \
                   "BUILD_BUG_ON: " #cond)

#ifndef UINT_MAX
#define UINT_MAX (~0U)
#endif
#ifndef INT_MAX
#define INT_MAX ((int)(~0U >> 1))
#endif
#ifndef INT_MIN
#define INT_MIN (-INT_MAX - 1)
#endif
#define S16_MAX ((s16)(U16_MAX >> 1))
#define S16_MIN ((s16)(-S16_MAX - 1))
#define S32_MAX ((s32)(U32_MAX >> 1))
#define S32_MIN ((s32)(-S32_MAX - 1))
#define S64_MAX ((s64)(U64_MAX >> 1))
#define S64_MIN ((s64)(-S64_MAX - 1))

/* errno values the inherited linux/kernel.h lacks (Linux numbering, like the rest). */
#ifndef ESRCH
#define ESRCH    3
#endif
#ifndef E2BIG
#define E2BIG    7
#endif
#ifndef EACCES
#define EACCES   13
#endif
#ifndef EEXIST
#define EEXIST   17
#endif
#ifndef ERANGE
#define ERANGE   34
#endif
#ifndef ENODATA
#define ENODATA  61
#endif
#ifndef ENOLINK
#define ENOLINK  67
#endif
#ifndef EBADMSG
#define EBADMSG  74
#endif
#ifndef ENOBUFS
#define ENOBUFS  105
#endif
#ifndef ECANCELED
#define ECANCELED 125
#endif

#define DIV_ROUND_DOWN_ULL(ll, d) ((unsigned long long)(ll) / (unsigned long long)(d))
#define DIV_ROUND_UP_ULL(ll, d) \
    DIV_ROUND_DOWN_ULL((unsigned long long)(ll) + (d) - 1, (d))

/*
 * The inherited min()/max() evaluate their arguments twice. Evaluate once, as
 * Linux does, and stay usable in constant expressions when both are constants.
 */
#define __rtw89_cmp_once(op, a, b) \
    ({ __typeof__(a) __a = (a); __typeof__(b) __b = (b); __a op __b ? __a : __b; })
#define __rtw89_cmp(op, a, b)                                               \
    __builtin_choose_expr(__builtin_constant_p(a) && __builtin_constant_p(b), \
                          ((a) op (b) ? (a) : (b)), __rtw89_cmp_once(op, a, b))
#undef min
#undef max
#undef min_t
#undef max_t
#define min(a, b) __rtw89_cmp(<, a, b)
#define max(a, b) __rtw89_cmp(>, a, b)
#define min_t(type, a, b) __rtw89_cmp(<, (type)(a), (type)(b))
#define max_t(type, a, b) __rtw89_cmp(>, (type)(a), (type)(b))

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
/* A group of members addressable both directly and as one named sub-struct. */
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
    union { \
        struct { MEMBERS } ATTRS; \
        struct TAG { MEMBERS } ATTRS NAME; \
    } ATTRS
#define struct_group(NAME, MEMBERS...) \
    __struct_group(/* no tag */, NAME, /* no attrs */, MEMBERS)
#define struct_group_attr(NAME, ATTRS, MEMBERS...) \
    __struct_group(/* no tag */, NAME, ATTRS, MEMBERS)
#define struct_group_tagged(TAG, NAME, MEMBERS...) \
    __struct_group(TAG, NAME, /* no attrs */, MEMBERS)
/* Zero everything in *obj after @member. */
#define memset_after(obj, v, member) \
    memset((u8 *)(obj) + offsetofend(__typeof__(*(obj)), member), (v), \
           sizeof(*(obj)) - offsetofend(__typeof__(*(obj)), member))
/* kzalloc_objs(P, COUNT [, gfp]) / kzalloc_flex(P, FAM, COUNT [, gfp]): P is a
 * type or an lvalue of the type; gfp defaults to GFP_KERNEL. (kzalloc_obj and
 * kmalloc_obj are in the inherited linux/slab.h.) */
#define __rtw89_first_arg(a, ...) a
#define default_gfp(...) __rtw89_first_arg(__VA_ARGS__ __VA_OPT__(,) GFP_KERNEL)
#define kzalloc_objs(P, COUNT, ...) \
    ((__typeof__(P) *)kcalloc((COUNT), sizeof(__typeof__(P)), default_gfp(__VA_ARGS__)))
#define kzalloc_flex(P, FAM, COUNT, ...) \
    ((__typeof__(P) *)kzalloc(struct_size_t(__typeof__(P), FAM, COUNT), \
                              default_gfp(__VA_ARGS__)))

/* ------------------------------------------------------------------ */
/*  Atomics / refcount                                                  */
/* ------------------------------------------------------------------ */

typedef struct { volatile s64 counter; } atomic64_t;
static inline void atomic64_set(atomic64_t *v, s64 i) { __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_read(const atomic64_t *v) { return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST); }
static inline s64 atomic64_inc_return(atomic64_t *v) { return __atomic_add_fetch(&v->counter, 1, __ATOMIC_SEQ_CST); }

typedef struct { atomic_t refs; } refcount_t;
static inline void refcount_set(refcount_t *r, int n) { atomic_set(&r->refs, n); }
static inline unsigned int refcount_read(const refcount_t *r) { return (unsigned int)atomic_read(&r->refs); }
static inline void refcount_inc(refcount_t *r) { atomic_inc(&r->refs); }
static inline bool refcount_dec_and_test(refcount_t *r) { return atomic_dec_and_test(&r->refs); }
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

/*
 * The inherited spinlock_t is a heap-allocated IORecursiveLock, so unlike on
 * Linux it has to be released. Driver code never does (its locks live as long
 * as the device); the compat layer's own short-lived objects use this.
 */
extern void IORecursiveLockFree(IORecursiveLock *lock);
static inline void spin_lock_destroy(spinlock_t *sl)
{
    if (sl->lock) {
        IORecursiveLockFree(sl->lock);
        sl->lock = NULL;
    }
}

/* No softirqs on macOS: bottom halves run from thread calls already serialized
 * by the driver's own locks. */
#define local_bh_disable() do { } while (0)
#define local_bh_enable()  do { } while (0)

/* RCU: readers are no-ops (see rtw88_compat.h); frees are immediate. */
struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *head);
};
#define rcu_access_pointer(p) (p)
#define rcu_dereference_check(p, c) (p)
#define rcu_dereference_raw(p) (p)
#define kfree_rcu(ptr, field) kfree(ptr)
#define lockdep_is_held(l) 1
#define lockdep_assert_not_held(l) do { } while (0)
#define lockdep_assert_in_softirq() do { } while (0)
#define might_sleep() do { } while (0)

/* devm_*: rtw89 frees these explicitly on the paths we use; no device-managed lifetime. */
#define devm_kcalloc(dev, n, size, gfp) kcalloc(n, size, gfp)
#define devm_kfree(dev, p) kfree(p)

/* Insert the entries of @list between @prev and @next; @list itself is left stale. */
static inline void __list_splice(const struct list_head *list,
                                 struct list_head *prev, struct list_head *next)
{
    struct list_head *first = list->next, *last = list->prev;

    first->prev = prev;
    prev->next = first;
    last->next = next;
    next->prev = last;
}
static inline void list_splice(const struct list_head *list, struct list_head *head)
{
    if (!list_empty(list))
        __list_splice(list, head, head->next);
}
static inline void list_splice_tail(struct list_head *list, struct list_head *head)
{
    if (!list_empty(list))
        __list_splice(list, head->prev, head);
}
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

#define dev_info_once(dev, fmt, ...)                                \
    do {                                                            \
        static bool __once;                                         \
        if (!__once) {                                              \
            __once = true;                                          \
            dev_info(dev, fmt, ##__VA_ARGS__);                      \
        }                                                           \
    } while (0)

/* ------------------------------------------------------------------ */
/*  Time                                                                */
/* ------------------------------------------------------------------ */

typedef s64 ktime_t;
u64 rtw89_compat_ktime_ns(void);
#define ktime_get() ((ktime_t)rtw89_compat_ktime_ns())
#define ktime_get_boottime_ns() rtw89_compat_ktime_ns()
static inline s64 ktime_us_delta(ktime_t later, ktime_t earlier) { return (later - earlier) / 1000; }
static inline s64 ktime_ms_delta(ktime_t later, ktime_t earlier) { return (later - earlier) / 1000000; }

/* hrtimer: only embedded in cfg80211's wiphy_hrtimer_work, which rtw89 does not use. */
enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };
enum hrtimer_mode { HRTIMER_MODE_ABS, HRTIMER_MODE_REL };
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
struct hrtimer {
    enum hrtimer_restart (*function)(struct hrtimer *);
};
static inline void hrtimer_setup(struct hrtimer *timer,
                                 enum hrtimer_restart (*function)(struct hrtimer *),
                                 int clock_id, enum hrtimer_mode mode)
{
    timer->function = function;
}

/* ------------------------------------------------------------------ */
/*  Network                                                             */
/* ------------------------------------------------------------------ */

#ifndef htons
#define htons(x) ((u16)__builtin_bswap16((u16)(x)))
#endif

void rtw89_compat_random_bytes(void *buf, size_t len);
#define get_random_bytes(buf, len) rtw89_compat_random_bytes(buf, len)

static inline u64 ether_addr_to_u64(const u8 *addr)
{
    u64 u = 0;

    for (int i = 0; i < ETH_ALEN; i++)
        u = u << 8 | addr[i];
    return u;
}
static inline void u64_to_ether_addr(u64 u, u8 *addr)
{
    for (int i = ETH_ALEN - 1; i >= 0; i--) {
        addr[i] = u & 0xff;
        u >>= 8;
    }
}

/* The compat sk_buff is always linear; network_header is an offset from head. */
static inline bool skb_is_nonlinear(const struct sk_buff *skb) { return false; }
static inline unsigned char *skb_network_header(const struct sk_buff *skb)
{
    return skb->head + skb->network_header;
}
static inline int skb_network_offset(const struct sk_buff *skb)
{
    return (int)(skb_network_header(skb) - skb->data);
}

/* Headroom Linux reserves in front of received frames (max(32, L1_CACHE_BYTES)). */
#define NET_SKB_PAD 64

/* Lockless sk_buff_head helpers. The inherited sk_buff_head keeps a list_head
 * plus qlen; its spinlock is only touched by the locked variants. */
static inline void __skb_queue_head_init(struct sk_buff_head *list)
{
    INIT_LIST_HEAD(&list->list);
    list->qlen = 0;
}
static inline void skb_queue_splice(const struct sk_buff_head *list,
                                    struct sk_buff_head *head)
{
    if (!skb_queue_empty(list)) {
        list_splice(&list->list, &head->list);
        head->qlen += list->qlen;
    }
}
static inline void skb_queue_splice_init(struct sk_buff_head *list,
                                         struct sk_buff_head *head)
{
    skb_queue_splice(list, head);
    __skb_queue_head_init(list);
}
/* Grow the buffer by @nhead bytes of headroom and @ntail of tailroom. */
int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp);

struct net_device *alloc_netdev_dummy(int sizeof_priv);

#define NAPI_POLL_WEIGHT 64
/* The inherited NAPI runs poll() from a work item; scheduled == queued or running. */
static inline bool napi_is_scheduled(const struct napi_struct *napi)
{
    return napi->work.pending || napi->work.executing;
}

/* Types cfg80211.h/mac80211.h only name in prototypes. */
struct netlink_ext_ack;
struct ethtool_stats;
struct ethtool_drvinfo;
struct netdev_hw_addr_list;
struct net_device_path;
struct net_device_path_ctx;
enum tc_setup_type { TC_SETUP_UNUSED };
struct ethhdr {
    u8     h_dest[ETH_ALEN];
    u8     h_source[ETH_ALEN];
    __be16 h_proto;
} __packed;

/* net_device bits cfg80211.h/mac80211.h touch. */
typedef u64 netdev_features_t;
typedef struct { } possible_net_t;
struct net;
static inline struct net *read_pnet(const possible_net_t *pnet) { return NULL; }
static inline void write_pnet(possible_net_t *pnet, struct net *net) { }
#define NETDEV_ALIGN 32
static inline bool netif_running(const struct net_device *dev) { return true; }

/* rfkill: no hardware kill switch handling on macOS. */
enum rfkill_hard_block_reasons {
    RFKILL_HARD_BLOCK_SIGNAL   = 1 << 0,
    RFKILL_HARD_BLOCK_NOT_OWNER = 1 << 1,
};
struct rfkill;
static inline void rfkill_pause_polling(struct rfkill *rfkill) { }
static inline void rfkill_resume_polling(struct rfkill *rfkill) { }

/* ------------------------------------------------------------------ */
/*  PCI                                                                 */
/* ------------------------------------------------------------------ */

/* Take register offsets and vendor/device IDs from the unmodified upstream
 * headers; drop the few the inherited linux/pci.h spells differently first. */
#undef PCI_VENDOR_ID_REALTEK
#undef PCI_VENDOR_ID_INTEL
#undef PCI_COMMAND
#undef PCI_COMMAND_MASTER
#undef PCI_COMMAND_MEMORY
#undef PCI_CAP_ID_EXP
#undef PCI_EXP_LNKCTL
#undef PCI_EXP_LNKCTL_CLKREQ_EN
#undef PCI_EXP_LNKCTL_ASPM_L0S
#undef PCI_EXP_LNKCTL_ASPM_L1
#undef PCI_EXP_DEVCTL2
#undef PCI_EXP_DEVCTL2_COMP_TMOUT_DIS
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>

/* Walk the PCIe extended capability list (config space 0x100 and up). */
static inline u16 pci_find_ext_capability(struct pci_dev *dev, int cap)
{
    int ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;
    int pos = PCI_CFG_SPACE_SIZE;
    u32 header;

    if (pci_read_config_dword(dev, pos, &header) || !header || header == 0xffffffff)
        return 0;
    while (ttl-- > 0) {
        if (PCI_EXT_CAP_ID(header) == cap)
            return (u16)pos;
        pos = PCI_EXT_CAP_NEXT(header);
        if (pos < PCI_CFG_SPACE_SIZE)
            break;
        if (pci_read_config_dword(dev, pos, &header))
            break;
    }
    return 0;
}
static inline int pci_clear_and_set_config_dword(struct pci_dev *dev, int pos,
                                                 u32 clear, u32 set)
{
    u32 val;
    int ret = pci_read_config_dword(dev, pos, &val);

    if (ret)
        return ret;
    return pci_write_config_dword(dev, pos, (val & ~clear) | set);
}

/* ------------------------------------------------------------------ */
/*  cfg80211 / mac80211 — unmodified upstream headers                   */
/* ------------------------------------------------------------------ */

/*
 * third_party/linux-include holds linux/ieee80211*.h, nl80211.h, radiotap,
 * net/cfg80211.h and net/mac80211.h exactly as upstream. Everything above this
 * line (and the empty stub headers under linux/ next to this file) exists so
 * they compile.
 *
 * ieee80211_tx_info has a member called "jiffies", which the compat layer
 * defines as a function-like expression; hide the macro while the structs are
 * declared. Nothing in rtw89 touches that member.
 */
#pragma push_macro("jiffies")
#undef jiffies
#include <net/mac80211.h>
#pragma pop_macro("jiffies")

/* ------------------------------------------------------------------ */
/*  Platform: ACPI / DMI — no policy on macOS yet                       */
/* ------------------------------------------------------------------ */

#include <linux/acpi.h>
#include <linux/dmi.h>

#endif /* _RTW89_COMPAT_H */
