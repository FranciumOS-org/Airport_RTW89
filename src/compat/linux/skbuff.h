/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_SKBUFF_H
#define _RTW88_COMPAT_SKBUFF_H

#include "types.h"
#include "slab.h"
#include "spinlock.h"
#include <string.h>

/*
 * sk_buff shim for rtw88 macOS port.
 *
 * The real implementation in the kext routes through mbuf_t, but the driver
 * C files see this struct. The kext sets up the sk_buff from mbuf data before
 * calling driver TX functions, and converts back on RX.
 */
struct sk_buff {
    u8       *head;     /* allocation base */
    u8       *data;     /* data pointer */
    u8       *tail;     /* end of data */
    u8       *end;      /* end of allocation */
    u32       len;      /* data length */
    u32       data_len; /* fragment length */
    u16       protocol;
    u8        pkt_type;
    u32       priority;       /* 802.11 TID — becomes the TX-desc qsel */
    u16       queue_mapping;  /* AC index — selects the hw TX ring (ac_to_hwq) */
    u32       ip_summed;
    /* rtw89: offsets from head, set by the TX glue (ip_hdr/udp_hdr classify ARP/DHCP/ICMP) */
    u16       network_header;
    u16       transport_header;
    u16       mac_header;

    /* Control block — 48 bytes, used by IEEE80211/driver for private data */
    char      cb[64] __attribute__((aligned(8)));

    struct list_head list;

    /* For DMA — physical address of data */
    dma_addr_t  dma_addr;

    /* Reference to mbuf for kext use */
    void       *mbuf_ref;
};

/* sk_buff_head — queue */
struct sk_buff_head {
    struct list_head list;
    spinlock_t lock;
    u32 qlen;
};

static inline void skb_queue_head_init(struct sk_buff_head *list)
{
    INIT_LIST_HEAD(&list->list);
    spin_lock_init(&list->lock);
    list->qlen = 0;
}

static inline bool skb_queue_empty(const struct sk_buff_head *list)
{
    return list_empty(&list->list);
}

static inline u32 skb_queue_len(const struct sk_buff_head *list)
{
    return list->qlen;
}

static inline void skb_queue_tail(struct sk_buff_head *list, struct sk_buff *skb)
{
    spin_lock_bh(&list->lock);
    list_add_tail(&skb->list, &list->list);
    list->qlen++;
    spin_unlock_bh(&list->lock);
}

static inline struct sk_buff *skb_dequeue(struct sk_buff_head *list)
{
    spin_lock_bh(&list->lock);
    if (list_empty(&list->list)) {
        spin_unlock_bh(&list->lock);
        return NULL;
    }
    struct sk_buff *skb = container_of(list->list.next, struct sk_buff, list);
    list_del(&skb->list);
    list->qlen--;
    spin_unlock_bh(&list->lock);
    return skb;
}

static inline void skb_unlink(struct sk_buff *skb, struct sk_buff_head *list)
{
    spin_lock_bh(&list->lock);
    list_del(&skb->list);
    list->qlen--;
    spin_unlock_bh(&list->lock);
}

/* forward — kfree_skb defined below */
static inline void kfree_skb(struct sk_buff *skb);

static inline void skb_queue_purge(struct sk_buff_head *list)
{
    struct sk_buff *skb;
    while ((skb = skb_dequeue(list)) != NULL)
        kfree_skb(skb);
}

#define skb_queue_walk_safe(queue, skb, tmp) \
    for ((skb) = container_of((queue)->list.next, struct sk_buff, list), \
         (tmp) = container_of((skb)->list.next, struct sk_buff, list); \
         &(skb)->list != &(queue)->list; \
         (skb) = (tmp), (tmp) = container_of((tmp)->list.next, struct sk_buff, list))

/* Allocate sk_buff with headroom. */
static inline struct sk_buff *alloc_skb(u32 size, gfp_t priority)
{
    u32 alloc_size = size + 64; /* extra for head/tail room */
    u8 *data = (u8 *)kmalloc(alloc_size, priority);
    if (!data) return NULL;

    struct sk_buff *skb = (struct sk_buff *)kzalloc(sizeof(*skb), priority);
    if (!skb) { kfree(data); return NULL; }

    skb->head = data;
    skb->data = data;
    skb->tail = data;
    skb->end  = data + alloc_size;
    skb->len  = 0;
    INIT_LIST_HEAD(&skb->list);
    return skb;
}

/* Headroom Linux reserves in every skb from dev_alloc_skb()/netdev_alloc_skb()
 * (max(32, L1_CACHE_BYTES)); drivers and stacks push headers into it. */
#ifndef NET_SKB_PAD
#define NET_SKB_PAD 64
#endif

static inline struct sk_buff *netdev_alloc_skb(void *dev, u32 size)
{
    struct sk_buff *skb = alloc_skb(size + NET_SKB_PAD, GFP_ATOMIC);

    if (skb) {
        skb->data += NET_SKB_PAD;
        skb->tail += NET_SKB_PAD;
    }
    return skb;
}

static inline struct sk_buff *dev_alloc_skb(u32 size)
{
    return netdev_alloc_skb(NULL, size);
}

static inline struct sk_buff *netdev_alloc_skb_ip_align(void *dev, u32 size)
{
    return alloc_skb(size + 2, GFP_ATOMIC);
}

static inline void kfree_skb(struct sk_buff *skb)
{
    if (skb) {
        kfree(skb->head);
        kfree(skb);
    }
}

static inline void dev_kfree_skb_any(struct sk_buff *skb)
{
    kfree_skb(skb);
}

static inline void dev_kfree_skb_irq(struct sk_buff *skb)
{
    kfree_skb(skb);
}

static inline void consume_skb(struct sk_buff *skb)
{
    kfree_skb(skb);
}

/* Data manipulation */

/* Like Linux's skb_over_panic()/skb_under_panic(): writing past either end of
 * the buffer is heap corruption, so stop at the call that would do it. */
void rtw88_skb_panic(const struct sk_buff *skb, u32 len, const char *what)
    __attribute__((noreturn));

static inline void *skb_put(struct sk_buff *skb, u32 len)
{
    void *tmp = skb->tail;
    if (len > (u32)(skb->end - skb->tail))
        rtw88_skb_panic(skb, len, "skb_put");
    skb->tail += len;
    skb->len  += len;
    return tmp;
}

static inline void *skb_put_zero(struct sk_buff *skb, u32 len)
{
    void *tmp = skb_put(skb, len);
    memset(tmp, 0, len);
    return tmp;
}

static inline void skb_put_data(struct sk_buff *skb, const void *data, u32 len)
{
    void *tmp = skb_put(skb, len);
    memcpy(tmp, data, len);
}

static inline void *skb_push(struct sk_buff *skb, u32 len)
{
    if (len > (u32)(skb->data - skb->head))
        rtw88_skb_panic(skb, len, "skb_push");
    skb->data -= len;
    skb->len  += len;
    return skb->data;
}

static inline void *skb_pull(struct sk_buff *skb, u32 len)
{
    skb->data += len;
    skb->len  -= len;
    return skb->data;
}

static inline void skb_reserve(struct sk_buff *skb, int len)
{
    skb->data += len;
    skb->tail += len;
}

static inline void skb_trim(struct sk_buff *skb, u32 len)
{
    if (skb->len > len) {
        skb->tail -= skb->len - len;
        skb->len   = len;
    }
}

static inline u32 skb_headroom(const struct sk_buff *skb)
{
    return (u32)(skb->data - skb->head);
}

static inline u32 skb_tailroom(const struct sk_buff *skb)
{
    return (u32)(skb->end - skb->tail);
}

/* Same head- and tailroom as the original, like Linux: callers append to the
 * copy relying on the room they reserved in the original. */
static inline struct sk_buff *skb_copy(const struct sk_buff *skb, gfp_t prio)
{
    struct sk_buff *n = alloc_skb((u32)(skb->end - skb->head), prio);
    if (!n) return NULL;
    skb_reserve(n, skb_headroom(skb));
    memcpy(skb_put(n, skb->len), skb->data, skb->len);
    memcpy(n->cb, skb->cb, sizeof(n->cb));
    n->protocol = skb->protocol;
    n->pkt_type = skb->pkt_type;
    n->priority = skb->priority;
    n->queue_mapping = skb->queue_mapping;
    n->network_header = skb->network_header;
    n->transport_header = skb->transport_header;
    n->mac_header = skb->mac_header;
    return n;
}

static inline struct sk_buff *skb_clone(struct sk_buff *skb, gfp_t prio)
{
    return skb_copy(skb, prio);
}

static inline void skb_copy_from_linear_data(const struct sk_buff *skb,
                                               void *to, const u32 len)
{
    memcpy(to, skb->data, len);
}

static inline int skb_tailroom_reserve(struct sk_buff *skb, unsigned int mtu,
                                        unsigned int needed_tailroom)
{
    return (int)skb_tailroom(skb) - (int)needed_tailroom;
}

static inline u8 *skb_tail_pointer(const struct sk_buff *skb)
{
    return skb->tail;
}

static inline unsigned char *skb_mac_header(const struct sk_buff *skb)
{
    return skb->data;
}

/* skb_cb helper for ieee80211_tx_info */
#define SKB_CB(skb) ((void *)(skb)->cb)

/* pkt_type */
#define PACKET_HOST      0
#define PACKET_BROADCAST 1
#define PACKET_MULTICAST 2
#define PACKET_OTHERHOST 3

/* dev_kfree_skb — simple alias (no DMA unmap needed) */
static inline void dev_kfree_skb(struct sk_buff *skb) { kfree_skb(skb); }

/* Lockless (internal) queue operations */
static inline void __skb_queue_tail(struct sk_buff_head *list,
                                     struct sk_buff *skb)
{
    list_add_tail(&skb->list, &list->list);
    list->qlen++;
}

static inline void __skb_unlink(struct sk_buff *skb, struct sk_buff_head *list)
{
    list_del(&skb->list);
    list->qlen--;
}

static inline void skb_queue_head(struct sk_buff_head *list,
                                   struct sk_buff *skb)
{
    spin_lock_bh(&list->lock);
    list_add(&skb->list, &list->list);
    list->qlen++;
    spin_unlock_bh(&list->lock);
}

static inline struct sk_buff *skb_peek(const struct sk_buff_head *list)
{
    if (list_empty(&list->list)) return NULL;
    return container_of(list->list.next, struct sk_buff, list);
}

static inline void skb_reset_tail_pointer(struct sk_buff *skb)
{
    skb->tail = skb->data;
}

static inline void skb_copy_header(struct sk_buff *dst,
                                    const struct sk_buff *src)
{
    memcpy(dst->cb, src->cb, sizeof(dst->cb));
    dst->priority = src->priority;
}

/* queue_mapping (AC -> ring) and priority (TID -> qsel) are DISTINCT in the
 * rtw88 driver: rtw_tx_queue_mapping() uses skb_get_queue_mapping() to choose
 * the hardware ring, while rtw_tx_pkt_info_update() uses skb->priority as the
 * TX-descriptor qsel.  They must NOT alias (matches FreeBSD LinuxKPI's
 * separate ->qmap and ->priority). */
static inline u16 skb_get_queue_mapping(const struct sk_buff *skb)
{
    return skb->queue_mapping;
}

static inline void skb_set_queue_mapping(struct sk_buff *skb, u16 queue_mapping)
{
    skb->queue_mapping = queue_mapping;
}

#endif /* _RTW88_COMPAT_SKBUFF_H */
