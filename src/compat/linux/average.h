/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_AVERAGE_H
#define _RTW88_COMPAT_AVERAGE_H

#include "types.h"

/* Exponentially Weighted Moving Average (EWMA) */
/*
 * Matches include/linux/average.h: _weight_rcp is the reciprocal weight (a power
 * of two), so the shift amount is ilog2(_weight_rcp), and the new sample is
 * blended as (old * (w - 1) + val) / w in fixed point.
 */
#define DECLARE_EWMA(name, _precision, _weight_rcp)                        \
    struct ewma_##name {                                                    \
        unsigned long internal;                                             \
    };                                                                      \
    _Static_assert(((_weight_rcp) & ((_weight_rcp) - 1)) == 0 &&            \
                   (_weight_rcp) > 1, "EWMA weight must be a power of 2");  \
    static inline void ewma_##name##_init(struct ewma_##name *e)           \
    {                                                                       \
        e->internal = 0;                                                    \
    }                                                                       \
    static inline unsigned long ewma_##name##_read(struct ewma_##name *e)  \
    {                                                                       \
        return e->internal >> (_precision);                                 \
    }                                                                       \
    static inline void ewma_##name##_add(struct ewma_##name *e,            \
                                          unsigned long val)                \
    {                                                                       \
        unsigned long internal = e->internal;                               \
        unsigned long weight_rcp = __builtin_ctzl((unsigned long)(_weight_rcp)); \
        e->internal = internal ?                                            \
            (((internal << weight_rcp) - internal) +                        \
             (val << (_precision))) >> weight_rcp :                         \
            (val << (_precision));                                          \
    }

#endif /* _RTW88_COMPAT_AVERAGE_H */
