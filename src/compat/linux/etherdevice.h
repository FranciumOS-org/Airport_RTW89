/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_ETHERDEVICE_H
#define _RTW88_COMPAT_ETHERDEVICE_H

#include "types.h"
#include "if_ether.h"
#include <string.h>
#include <sys/random.h>

static inline void eth_broadcast_addr(u8 *addr)
{
    memset(addr, 0xff, ETH_ALEN);
}

static inline void eth_zero_addr(u8 *addr)
{
    memset(addr, 0, ETH_ALEN);
}

static inline void ether_addr_copy(u8 *dst, const u8 *src)
{
    memcpy(dst, src, ETH_ALEN);
}

static inline int ether_addr_equal(const u8 *a, const u8 *b)
{
    return memcmp(a, b, ETH_ALEN) == 0;
}

static inline int is_broadcast_ether_addr(const u8 *addr)
{
    return (addr[0] & addr[1] & addr[2] & addr[3] & addr[4] & addr[5]) == 0xff;
}

static inline int is_multicast_ether_addr(const u8 *addr)
{
    return 0x01 & addr[0];
}

static inline int is_unicast_ether_addr(const u8 *addr)
{
    return !is_multicast_ether_addr(addr);
}

static inline int is_zero_ether_addr(const u8 *addr)
{
    return !(addr[0] | addr[1] | addr[2] | addr[3] | addr[4] | addr[5]);
}

static inline int is_valid_ether_addr(const u8 *addr)
{
    return !is_multicast_ether_addr(addr) && !is_zero_ether_addr(addr);
}

static inline void eth_random_addr(u8 *addr)
{
    /* Generate a locally administered unicast address */
    read_random(addr, ETH_ALEN);
    addr[0] &= 0xfe; /* clear multicast bit */
    addr[0] |= 0x02; /* set locally administered bit */
}

static inline int ether_addr_equal_masked(const u8 *a, const u8 *b,
                                           const u8 *mask)
{
    for (int i = 0; i < ETH_ALEN; i++)
        if ((a[i] ^ b[i]) & mask[i])
            return 0;
    return 1;
}

void get_random_mask_addr(u8 *, const u8 *, const u8 *);

#endif /* _RTW88_COMPAT_ETHERDEVICE_H */
