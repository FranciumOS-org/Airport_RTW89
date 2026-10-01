/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW89_COMPAT_UUID_H
#define _RTW89_COMPAT_UUID_H

#include "../../compat/linux/types.h"

typedef struct { u8 b[16]; } guid_t;

#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                  \
    ((guid_t){ { (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff,         \
                 ((a) >> 24) & 0xff, (b) & 0xff, ((b) >> 8) & 0xff,           \
                 (c) & 0xff, ((c) >> 8) & 0xff,                               \
                 (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) } })

#endif
