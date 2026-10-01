/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_NETDEVICE_H
#define _RTW88_COMPAT_NETDEVICE_H

#include "types.h"
#include "skbuff.h"
#include "if_ether.h"

/* Minimal net_device stub needed by PCI code (napi, etc.) */
struct net_device {
    char name[16];
    u8   dev_addr[ETH_ALEN];
    unsigned int mtu;
    void *ml_priv;
};

#define CHECKSUM_NONE       0
#define CHECKSUM_UNNECESSARY 1

static inline struct net_device *alloc_netdev(int sizeof_priv, const char *name,
                                               unsigned char name_assign_type,
                                               void (*setup)(struct net_device *))
{
    return (struct net_device *)kzalloc(sizeof(struct net_device) + (size_t)sizeof_priv, GFP_KERNEL);
}

static inline void free_netdev(struct net_device *dev) { kfree(dev); }
static inline int register_netdev(struct net_device *dev) { return 0; }
static inline void unregister_netdev(struct net_device *dev) {}

static inline void netif_carrier_on(struct net_device *dev) {}
static inline void netif_carrier_off(struct net_device *dev) {}
static inline int netif_carrier_ok(struct net_device *dev) { return 1; }
static inline void netif_stop_queue(struct net_device *dev) {}
static inline void netif_wake_queue(struct net_device *dev) {}
static inline void netif_start_queue(struct net_device *dev) {}
static inline int netif_queue_stopped(struct net_device *dev) { return 0; }

static inline int netif_rx(struct sk_buff *skb) { return 0; }
static inline int netif_rx_ni(struct sk_buff *skb) { return 0; }
static inline int netif_receive_skb(struct sk_buff *skb) { return 0; }

static inline void *netdev_priv(struct net_device *dev)
{
    return (void *)(dev + 1);
}

#define SET_NETDEV_DEV(net, pdev) do {} while (0)

#endif /* _RTW88_COMPAT_NETDEVICE_H */
