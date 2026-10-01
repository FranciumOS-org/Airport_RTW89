/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_VMALLOC_H
#define _RTW88_COMPAT_VMALLOC_H

#include "slab.h"   /* kmalloc / kzalloc / kfree (IOMalloc-backed) */

static inline void *vmalloc(size_t size)
{
    return kmalloc(size, GFP_KERNEL);
}

static inline void *vzalloc(size_t size)
{
    return kzalloc(size, GFP_KERNEL);
}

static inline void vfree(const void *ptr)
{
    kfree(ptr);
}

static inline void *vmalloc_node(size_t size, int node)
{
    return kmalloc(size, GFP_KERNEL);
}

#endif /* _RTW88_COMPAT_VMALLOC_H */
