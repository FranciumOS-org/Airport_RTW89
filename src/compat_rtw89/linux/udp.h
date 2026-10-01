/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW89_COMPAT_UDP_H
#define _RTW89_COMPAT_UDP_H

#include "ip.h"

struct udphdr {
    __be16  source;
    __be16  dest;
    __be16  len;
    __sum16 check;
} __attribute__((packed));

static inline struct udphdr *udp_hdr(const struct sk_buff *skb)
{
    return (struct udphdr *)(skb->head + skb->transport_header);
}

#endif
