/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_DELAY_H
#define _RTW88_COMPAT_DELAY_H

#include "../iokit_shim.h"

/* IODelay: busy-wait (interrupt-context safe), max ~1ms per call */
static inline void udelay(unsigned long usecs)
{
    /* IODelay spins; large delays are split into 1000us chunks */
    while (usecs > 1000) {
        IODelay(1000);
        usecs -= 1000;
    }
    if (usecs) IODelay((unsigned)usecs);
}

static inline void ndelay(unsigned long nsecs)
{
    /* Round up to 1 µs */
    udelay((nsecs + 999) / 1000);
}

static inline void mdelay(unsigned long msecs)
{
    udelay(msecs * 1000);
}

/* IOSleep: sleepable, cannot be called from interrupt context */
static inline void usleep_range(unsigned long min_us, unsigned long max_us)
{
    unsigned long avg = (min_us + max_us) / 2;
    if (avg >= 1000)
        IOSleep((unsigned)(avg / 1000));
    else
        IODelay((unsigned)avg);
}

static inline void msleep(unsigned int msecs)
{
    IOSleep(msecs);
}

static inline void ssleep(unsigned int secs)
{
    IOSleep(secs * 1000u);
}

static inline void msleep_interruptible(unsigned int msecs)
{
    IOSleep(msecs);
}

#endif /* _RTW88_COMPAT_DELAY_H */
