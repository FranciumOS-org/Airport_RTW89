/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_JIFFIES_H
#define _RTW88_COMPAT_JIFFIES_H

#include "types.h"
#include "../iokit_shim.h"

/* 1 jiffy = 1 ms */
#define HZ 1000UL

/*
 * mach_absolute_time() returns nanoseconds on ARM64 (1 unit = 1 ns).
 * On x86 it is in TSC-derived units (~1 ns at GHz frequencies).
 * Dividing by 1,000,000 gives a monotonic millisecond counter sufficient
 * for the loose timeouts used by rtw88.
 */
#include <kern/clock.h>

static inline unsigned long rtw88_get_jiffies(void)
{
    uint64_t nsecs;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nsecs);
    return (unsigned long)(nsecs / 1000000ULL);
}

#define jiffies rtw88_get_jiffies()

static inline unsigned long msecs_to_jiffies(unsigned int msecs)
{
    return (unsigned long)msecs;
}

static inline unsigned long usecs_to_jiffies(unsigned int usecs)
{
    return (unsigned long)((usecs + 999) / 1000);
}

static inline unsigned int jiffies_to_msecs(unsigned long j)
{
    return (unsigned int)j;
}

static inline unsigned int jiffies_to_usecs(unsigned long j)
{
    return (unsigned int)(j * 1000);
}

static inline int time_after(unsigned long a, unsigned long b)
{
    return (long)((b) - (a)) < 0;
}

static inline int time_before(unsigned long a, unsigned long b)
{
    return time_after(b, a);
}

static inline int time_after_eq(unsigned long a, unsigned long b)
{
    return (long)((a) - (b)) >= 0;
}

#define time_is_before_jiffies(x) time_after(jiffies, x)
#define time_is_after_jiffies(x)  time_before(jiffies, x)

#define round_jiffies_relative(x) (x)

#endif /* _RTW88_COMPAT_JIFFIES_H */
