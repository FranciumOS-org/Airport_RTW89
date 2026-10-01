/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW89_COMPAT_IP_H
#define _RTW89_COMPAT_IP_H

#include "../../compat/linux/skbuff.h"

#ifndef __sum16_defined
#define __sum16_defined
typedef u16 __sum16;
#endif

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

struct iphdr {
    u8     ihl:4, version:4;
    u8     tos;
    __be16 tot_len;
    __be16 id;
    __be16 frag_off;
    u8     ttl;
    u8     protocol;
    __sum16 check;
    __be32 saddr;
    __be32 daddr;
} __attribute__((packed));

static inline struct iphdr *ip_hdr(const struct sk_buff *skb)
{
    return (struct iphdr *)skb_network_header(skb);
}
static inline void skb_reset_mac_header(struct sk_buff *skb)
{
    skb->mac_header = (u16)(skb->data - skb->head);
}

#endif
