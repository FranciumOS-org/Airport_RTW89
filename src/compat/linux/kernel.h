/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_KERNEL_H
#define _RTW88_COMPAT_KERNEL_H

#include "types.h"
#include "bitops.h"
#include "../iokit_shim.h"

extern int rtw88_log_level;

void rtw88_printk(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define KERN_ERR   0
#define KERN_WARN  1
#define KERN_INFO  2
#define KERN_DEBUG 3

#define pr_err(fmt, ...)   rtw88_printk(KERN_ERR,   fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)  rtw88_printk(KERN_WARN,  fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)  rtw88_printk(KERN_INFO,  fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) rtw88_printk(KERN_DEBUG, fmt, ##__VA_ARGS__)
#define printk(fmt, ...)   rtw88_printk(KERN_INFO,  fmt, ##__VA_ARGS__)

/* WARN returns int so it can be used in if() conditions */
#define WARN(cond, fmt, ...) \
    ((cond) ? (rtw88_printk(KERN_WARN, "WARN at %s:%d: " fmt, \
        __FILE__, __LINE__, ##__VA_ARGS__), 1) : 0)

#define WARN_ON(cond) \
    ((cond) ? (rtw88_printk(KERN_WARN, "WARN_ON at %s:%d\n", \
        __FILE__, __LINE__), 1) : 0)

#define WARN_ON_ONCE(cond)          WARN_ON(cond)
#define WARN_ONCE(cond, fmt, ...)   WARN(cond, fmt, ##__VA_ARGS__)

#define BUG() \
    do { rtw88_printk(KERN_ERR, "BUG at %s:%d\n", __FILE__, __LINE__); \
         __builtin_trap(); } while (0)

#define BUG_ON(cond) do { if (cond) BUG(); } while (0)

#define BUILD_BUG_ON(cond)       _Static_assert(!(cond), "BUILD_BUG_ON")
#define BUILD_BUG_ON_ZERO(e)     (sizeof(struct { int:(-!!(e)); }))
#ifndef static_assert
#define static_assert(expr, ...) _Static_assert(expr, "" __VA_ARGS__)
#endif

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define clamp(val, lo, hi) min(max(val, lo), hi)
#define clamp_t(type, val, lo, hi) min_t(type, max_t(type, val, lo), hi)
#define abs(x) ((x) < 0 ? -(x) : (x))

static inline s32 div_s64(s64 dividend, s32 divisor) { return (s32)(dividend / divisor); }
static inline u32 div_u64(u64 dividend, u32 divisor) { return (u32)(dividend / divisor); }

#define EPERM          1
#define ENOENT         2
#define EIO            5
#define EAGAIN        11
#define ENOMEM        12
#define EFAULT        14
#define EBUSY         16
#define ENODEV        19
#define EINVAL        22
#define ENOSPC        28
#define EALREADY      37
#define ENOTSUPP      524
#define EOPNOTSUPP    95
#define ETIMEDOUT    110
#define ENOTSUP EOPNOTSUPP
#define EPIPE         32
#define EPROTO        71
#define EOVERFLOW     75
#define ECOMM         70
#define ESHUTDOWN    108
#define EINPROGRESS  115
#define EILSEQ        84
#define ETIME         62

#define IS_ERR_VALUE(x) ((unsigned long)(x) >= (unsigned long)-4095)

static inline void *ERR_PTR(long error)  { return (void *)error; }
static inline long  PTR_ERR(const void *ptr) { return (long)ptr; }
static inline int   IS_ERR(const void *ptr) { return IS_ERR_VALUE((unsigned long)ptr); }
static inline int   IS_ERR_OR_NULL(const void *ptr) { return !ptr || IS_ERR(ptr); }

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define ALIGN_DOWN(x, a)  ((x) & ~((a) - 1))
#define PTR_ALIGN(p, a)   ((typeof(p))ALIGN((unsigned long)(p), (a)))

/* strlcpy available from libkern */
#define round_jiffies_relative(x) (x)

void rtw88_hex_dump(const char *prefix, const void *buf, size_t len);
#define print_hex_dump(level, prefix, ptype, gsz, ll, buf, len, ascii) \
    rtw88_hex_dump(prefix, buf, len)
#define print_hex_dump_bytes(prefix, ptype, buf, len) \
    rtw88_hex_dump(prefix, buf, len)

#define do_div(n, base) ({ \
    u32 __rem = (u64)(n) % (u32)(base); \
    (n) = (u64)(n) / (u32)(base); \
    __rem; })

#define ilog2(n) (fls(n) - 1)
#define swap(a, b) do { typeof(a) _t = (a); (a) = (b); (b) = _t; } while (0)

#define READ_ONCE(x)       (*(volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, val) (*(volatile typeof(x) *)&(x) = (val))
#define smp_mb()   __sync_synchronize()
#define smp_rmb()  __sync_synchronize()
#define smp_wmb()  __sync_synchronize()
#define barrier()  __asm__ __volatile__("" ::: "memory")

#define KBUILD_MODNAME "rtw88"

/* Common size constants */
#define SZ_1K    (1024)
#define SZ_4K    (4 * 1024)
#define SZ_8K    (8 * 1024)
#define SZ_16K   (16 * 1024)
#define SZ_32K   (32 * 1024)
#define SZ_64K   (64 * 1024)
#define SZ_128K  (128 * 1024)
#define SZ_256K  (256 * 1024)
#define SZ_1M    (1024 * 1024)

/* Scheduler / task stub — used for IRQ thread detection */
struct task_struct { int dummy; };
extern struct task_struct *__rtw88_current_task;
#define current __rtw88_current_task

#define U8_MAX   ((u8)~0U)
#define U16_MAX  ((u16)~0U)
#define U32_MAX  ((u32)~0U)
#define U64_MAX  ((u64)~0ULL)
#define S8_MAX   ((s8)(U8_MAX >> 1))
#define S8_MIN   ((s8)(-S8_MAX - 1))

/* Integer math */
#define min3(a, b, c)           min(min(a, b), c)
#define max3(a, b, c)           max(max(a, b), c)
#define abs_diff(a, b)          ((a) > (b) ? (a) - (b) : (b) - (a))

#define DIV_ROUND_UP(n, d)      (((n) + (d) - 1) / (d))
#define DIV_ROUND_DOWN(n, d)    ((n) / (d))
#define DIV_ROUND_CLOSEST(n, d) (((n) + (d) / 2) / (d))
#define round_up(x, y)          ((((x) + (y) - 1) / (y)) * (y))
#define round_down(x, y)        (((x) / (y)) * (y))

/* Delay / sleep helpers (kernel context) */
static inline void fsleep(unsigned long us)
{
    if (us < 1000)
        IODelay((unsigned int)us);
    else
        IOSleep((unsigned int)(us / 1000) + 1);
}

/* read_poll_timeout_atomic — poll op(args) until cond is true or timeout */
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, \
                                  delay_before_read, args...) \
({ \
    u64 _deadline = mach_absolute_time() + (u64)(timeout_us) * 1000ULL; \
    for (;;) { \
        (val) = op(args); \
        if (cond) { break; } \
        if (mach_absolute_time() >= _deadline) { (val) = op(args); break; } \
        IODelay((unsigned int)(delay_us) < 1 ? 1 : (unsigned int)(delay_us)); \
    } \
    (cond) ? 0 : -ETIMEDOUT; \
})

/* One-shot message macros */
#define pr_err_once(fmt, ...) \
    do { static int _once; if (!_once) { _once = 1; pr_err(fmt, ##__VA_ARGS__); } } while (0)

#define dev_dbg_ratelimited(dev, fmt, ...) \
    do { (void)(dev); } while (0)

#endif /* _RTW88_COMPAT_KERNEL_H */
