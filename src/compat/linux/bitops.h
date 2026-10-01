/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Linux bitops shims for rtw88 macOS port
 */
#ifndef _RTW88_COMPAT_BITOPS_H
#define _RTW88_COMPAT_BITOPS_H

#include "types.h"

#define BIT(n)          (1UL << (n))
#define BIT_ULL(n)      (1ULL << (n))

#define GENMASK(h, l) \
    (((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define GENMASK_ULL(h, l) \
    (((~0ULL) - (1ULL << (l)) + 1) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))

static inline void set_bit(unsigned int nr, volatile unsigned long *addr)
{
    __sync_fetch_and_or(addr + (nr / BITS_PER_LONG),
                        1UL << (nr % BITS_PER_LONG));
}

static inline void clear_bit(unsigned int nr, volatile unsigned long *addr)
{
    __sync_fetch_and_and(addr + (nr / BITS_PER_LONG),
                         ~(1UL << (nr % BITS_PER_LONG)));
}

static inline int test_bit(unsigned int nr, const volatile unsigned long *addr)
{
    return !!(addr[nr / BITS_PER_LONG] & (1UL << (nr % BITS_PER_LONG)));
}

static inline int test_and_set_bit(unsigned int nr, volatile unsigned long *addr)
{
    unsigned long old = __sync_fetch_and_or(addr + (nr / BITS_PER_LONG),
                                            1UL << (nr % BITS_PER_LONG));
    return !!(old & (1UL << (nr % BITS_PER_LONG)));
}

static inline int test_and_clear_bit(unsigned int nr, volatile unsigned long *addr)
{
    unsigned long old = __sync_fetch_and_and(addr + (nr / BITS_PER_LONG),
                                             ~(1UL << (nr % BITS_PER_LONG)));
    return !!(old & (1UL << (nr % BITS_PER_LONG)));
}

#define DECLARE_BITMAP(name, bits) \
    unsigned long name[((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG]

static inline unsigned long __ffs(unsigned long x)
{
    return __builtin_ctzl(x);
}

static inline unsigned long __fls(unsigned long x)
{
    return (BITS_PER_LONG - 1) - __builtin_clzl(x);
}

/* ffs() and fls() are provided by <libkern/libkern.h> on macOS kernel.
 * We must not redefine them — use __builtin_* directly in fls64. */

static inline int fls64(u64 x)
{
    return x ? (64 - __builtin_clzll(x)) : 0;
}

static inline unsigned int hweight32(u32 x)
{
    return __builtin_popcount(x);
}

static inline unsigned int hweight_long(unsigned long x)
{
    return __builtin_popcountl(x);
}

#define for_each_set_bit(bit, addr, size) \
    for ((bit) = 0; (bit) < (size); (bit)++) \
        if (test_bit(bit, addr))

/* Rotate operations */
static inline u32 rol32(u32 val, u32 shift)
{
    return (val << shift) | (val >> (32 - shift));
}

static inline u32 ror32(u32 val, u32 shift)
{
    return (val >> shift) | (val << (32 - shift));
}

/* Read/modify/write bit field helpers */
static inline u32 rtw_read32_mask_helper(u32 val, u32 mask)
{
    return (val & mask) >> __ffs(mask);
}

static inline u32 rtw_set32_mask_helper(u32 val, u32 mask, u32 data)
{
    return (val & ~mask) | ((data << __ffs(mask)) & mask);
}

/* fls is declared extern in MacKernelSDK's <libkern/libkern.h>; use it as-is.
 * fls64 is defined above; no re-definition here. */

static inline unsigned int hweight8(unsigned int w)
{
    return (unsigned int)__builtin_popcount(w & 0xffu);
}

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits)
{
    unsigned int words = (nbits + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (unsigned int i = 0; i < words; i++) dst[i] = 0UL;
}

static inline unsigned long find_first_bit(const unsigned long *addr,
                                            unsigned long size)
{
    unsigned long words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (unsigned long i = 0; i < words; i++) {
        if (addr[i]) {
            unsigned long bit = i * BITS_PER_LONG + __ffs(addr[i]);
            return bit < size ? bit : size;
        }
    }
    return size;
}

static inline unsigned long find_first_zero_bit(const unsigned long *addr,
                                                  unsigned long size)
{
    unsigned long words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (unsigned long i = 0; i < words; i++) {
        unsigned long v = ~addr[i];
        if (v) {
            unsigned long bit = i * BITS_PER_LONG + __ffs(v);
            return bit < size ? bit : size;
        }
    }
    return size;
}

static inline unsigned long find_next_bit(const unsigned long *addr,
                                           unsigned long size,
                                           unsigned long offset)
{
    if (offset >= size) return size;
    unsigned long idx   = offset / BITS_PER_LONG;
    unsigned long start = offset % BITS_PER_LONG;
    unsigned long mask  = addr[idx] & (~0UL << start);
    if (mask) {
        unsigned long bit = idx * BITS_PER_LONG + __ffs(mask);
        return bit < size ? bit : size;
    }
    unsigned long words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (idx++; idx < words; idx++) {
        if (addr[idx]) {
            unsigned long bit = idx * BITS_PER_LONG + __ffs(addr[idx]);
            return bit < size ? bit : size;
        }
    }
    return size;
}

static inline unsigned long find_next_zero_bit(const unsigned long *addr,
                                                unsigned long size,
                                                unsigned long offset)
{
    if (offset >= size) return size;
    unsigned long idx   = offset / BITS_PER_LONG;
    unsigned long start = offset % BITS_PER_LONG;
    unsigned long mask  = (~addr[idx]) & (~0UL << start);
    if (mask) {
        unsigned long bit = idx * BITS_PER_LONG + __ffs(mask);
        return bit < size ? bit : size;
    }
    unsigned long words = (size + BITS_PER_LONG - 1) / BITS_PER_LONG;
    for (idx++; idx < words; idx++) {
        unsigned long v = ~addr[idx];
        if (v) {
            unsigned long bit = idx * BITS_PER_LONG + __ffs(v);
            return bit < size ? bit : size;
        }
    }
    return size;
}

#endif /* _RTW88_COMPAT_BITOPS_H */
