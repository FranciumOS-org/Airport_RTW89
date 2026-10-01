/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_IOPOLL_H
#define _RTW88_COMPAT_IOPOLL_H

#include "types.h"
#include "delay.h"
#include "jiffies.h"

/*
 * read_poll_timeout - Periodically poll an address until a condition is met
 * or a timeout occurs.
 *
 * @op:        accessor function (e.g. rtw_read32)
 * @addr:      address to read
 * @val:       symbol for the read value
 * @cond:      condition to stop polling
 * @sleep_us:  how long to sleep between polls (microseconds)
 * @timeout_us: total timeout in microseconds
 * @sleep_before_read: sleep before first read
 * @args:      args passed to @op after @addr
 */
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us,                \
                          sleep_before_read, args...)                          \
({                                                                             \
    unsigned long _start = jiffies;                                            \
    unsigned long _timeout_j = usecs_to_jiffies(timeout_us);                  \
    int _ret = 0;                                                              \
    if (sleep_before_read && sleep_us > 0) udelay(sleep_us);                  \
    for (;;) {                                                                 \
        (val) = op(args);                                                      \
        if (cond) break;                                                       \
        if (timeout_us && time_after(jiffies, _start + _timeout_j)) {         \
            (val) = op(args);                                                  \
            if (cond) break;                                                   \
            _ret = -ETIMEDOUT;                                                 \
            break;                                                             \
        }                                                                      \
        if (sleep_us > 0) udelay(sleep_us);                                   \
    }                                                                          \
    _ret;                                                                      \
})

#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us)         \
    read_poll_timeout(op, val, cond, sleep_us, timeout_us, false, addr)

#endif /* _RTW88_COMPAT_IOPOLL_H */
