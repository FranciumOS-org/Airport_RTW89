/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW89_COMPAT_IF_ARP_H
#define _RTW89_COMPAT_IF_ARP_H

#include "../../compat/linux/types.h"

#define ARPHRD_ETHER 1
#define ARPOP_REQUEST 1
#define ARPOP_REPLY   2

struct arphdr {
    __be16 ar_hrd;
    __be16 ar_pro;
    u8     ar_hln;
    u8     ar_pln;
    __be16 ar_op;
} __attribute__((packed));

#endif
