/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_DMA_MAPPING_H
#define _RTW88_COMPAT_DMA_MAPPING_H

#include "types.h"
#include "slab.h"

#define DMA_BIT_MASK(n)   ((u64)((n) < 64 ? (1ULL << (n)) - 1 : ~0ULL))
#define DMA_FROM_DEVICE    0
#define DMA_TO_DEVICE      1
#define DMA_BIDIRECTIONAL  2
#define DMA_NONE           3

struct device;  /* forward decl */

/*
 * On macOS, DMA coherent memory is allocated by the kext using
 * IOBufferMemoryDescriptor (kIOMemoryPhysicallyContiguous).
 * The C driver code calls these helpers; the kext installs rtw88_dma_ops
 * before probe so the fallback path never runs in production.
 */
struct rtw88_dma_alloc_ops {
    void      *(*alloc_coherent)(struct device *dev, size_t size,
                                  dma_addr_t *dma_handle, gfp_t flag);
    void       (*free_coherent)(struct device *dev, size_t size,
                                 void *cpu_addr, dma_addr_t dma_handle);
    dma_addr_t (*map_single)(struct device *dev, void *ptr,
                              size_t size, int direction);
    void       (*unmap_single)(struct device *dev, dma_addr_t addr,
                                size_t size, int direction);
    void       (*sync_single_for_cpu)(struct device *dev, dma_addr_t addr,
                                       size_t size, int dir);
    void       (*sync_single_for_device)(struct device *dev, dma_addr_t addr,
                                          size_t size, int dir);
};

extern struct rtw88_dma_alloc_ops *rtw88_dma_ops;

static inline void *dma_alloc_coherent(struct device *dev, size_t size,
                                        dma_addr_t *dma_handle, gfp_t flag)
{
    if (rtw88_dma_ops && rtw88_dma_ops->alloc_coherent)
        return rtw88_dma_ops->alloc_coherent(dev, size, dma_handle, flag);
    /* Fallback: plain kernel allocation (not DMA-safe) */
    void *ptr = kzalloc(size, GFP_KERNEL);
    if (ptr) *dma_handle = (dma_addr_t)(uintptr_t)ptr;
    return ptr;
}

static inline void dma_free_coherent(struct device *dev, size_t size,
                                      void *cpu_addr, dma_addr_t dma_handle)
{
    if (rtw88_dma_ops && rtw88_dma_ops->free_coherent) {
        rtw88_dma_ops->free_coherent(dev, size, cpu_addr, dma_handle);
        return;
    }
    kfree(cpu_addr);
}

static inline dma_addr_t dma_map_single(struct device *dev, void *ptr,
                                          size_t size, int direction)
{
    if (rtw88_dma_ops && rtw88_dma_ops->map_single)
        return rtw88_dma_ops->map_single(dev, ptr, size, direction);
    return (dma_addr_t)(uintptr_t)ptr;
}

static inline void dma_unmap_single(struct device *dev, dma_addr_t addr,
                                     size_t size, int direction)
{
    if (rtw88_dma_ops && rtw88_dma_ops->unmap_single)
        rtw88_dma_ops->unmap_single(dev, addr, size, direction);
}

static inline void dma_sync_single_for_cpu(struct device *dev,
                                             dma_addr_t addr,
                                             size_t size, int dir)
{
    if (rtw88_dma_ops && rtw88_dma_ops->sync_single_for_cpu)
        rtw88_dma_ops->sync_single_for_cpu(dev, addr, size, dir);
}

static inline void dma_sync_single_for_device(struct device *dev,
                                                dma_addr_t addr,
                                                size_t size, int dir)
{
    if (rtw88_dma_ops && rtw88_dma_ops->sync_single_for_device)
        rtw88_dma_ops->sync_single_for_device(dev, addr, size, dir);
}

static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask) { return 0; }
static inline int dma_set_mask(struct device *dev, u64 mask)              { return 0; }
static inline int dma_set_coherent_mask(struct device *dev, u64 mask)     { return 0; }

/* Match what compat_dma_map actually returns on failure: it returns 0 when
 * the bounce-buffer alloc fails.  Treat BOTH 0 and -1 as errors so the
 * driver doesn't silently program HW descriptors with bogus DMA addresses
 * (which corrupts the TX ring state — wp advances, HW chews garbage and
 * advances rp, eventually deadlocking at wp+1 == rp). */
static inline bool dma_mapping_error(struct device *dev, dma_addr_t addr)
{
    return addr == 0 || addr == (dma_addr_t)-1;
}

/* Page abstraction for RX ring allocations */
struct page {
    void      *virt;
    dma_addr_t phys;
};

static inline struct page *alloc_page(gfp_t flags)
{
    /* Allocate struct page + 4096-byte data in one chunk */
    struct page *p = (struct page *)kzalloc(sizeof(*p) + 4096, flags);
    if (p) {
        p->virt = (void *)((char *)p + sizeof(*p));
        p->phys = (dma_addr_t)(uintptr_t)p->virt;
    }
    return p;
}

static inline void __free_page(struct page *page)
{
    kfree(page);
}

static inline void *page_address(struct page *page)
{
    return page ? page->virt : NULL;
}

static inline dma_addr_t dma_map_page(struct device *dev, struct page *page,
                                       size_t offset, size_t size, int dir)
{
    return page->phys + offset;
}

static inline void dma_unmap_page(struct device *dev, dma_addr_t addr,
                                   size_t size, int dir) {}

#endif /* _RTW88_COMPAT_DMA_MAPPING_H */
