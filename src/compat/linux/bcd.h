/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_BCD_H
#define _RTW88_COMPAT_BCD_H

#include "types.h"

static inline u8 bcd2bin(u8 val)
{
    return (val & 0x0f) + (val >> 4) * 10;
}

static inline u8 bin2bcd(u8 val)
{
    return ((val / 10) << 4) | (val % 10);
}

#endif /* _RTW88_COMPAT_BCD_H */
