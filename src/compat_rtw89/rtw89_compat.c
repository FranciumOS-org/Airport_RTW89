// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Runtime pieces of the rtw89 compat additions (see rtw89_compat.h). */

#include "rtw89_compat.h"
#include <kern/clock.h>
#include <sys/random.h>

/* ------------------------------------------------------------------ */
/*  Time / random                                                       */
/* ------------------------------------------------------------------ */

u64 rtw89_compat_ktime_ns(void)
{
    u64 ns;

    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

u32 rtw89_compat_random_u32(void)
{
    u32 v;

    read_random(&v, sizeof(v));
    return v;
}

void rtw89_compat_random_bytes(void *buf, size_t len)
{
    read_random(buf, (u_int)len);
}

/* ------------------------------------------------------------------ */
/*  sk_buff                                                             */
/* ------------------------------------------------------------------ */

int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp)
{
    size_t used = (size_t)(skb->tail - skb->head);
    size_t size = (size_t)(skb->end - skb->head) + nhead + ntail;
    size_t data_off = (size_t)(skb->data - skb->head);
    u8 *head;

    if (nhead < 0 || ntail < 0)
        return -EINVAL;

    head = kmalloc(size, gfp);
    if (!head)
        return -ENOMEM;

    memcpy(head + nhead, skb->head, used);
    kfree(skb->head);

    skb->head = head;
    skb->data = head + nhead + data_off;
    skb->tail = skb->data + skb->len;
    skb->end = head + size;
    /* header offsets are relative to head */
    skb->network_header += nhead;
    skb->transport_header += nhead;
    skb->mac_header += nhead;
    return 0;
}
