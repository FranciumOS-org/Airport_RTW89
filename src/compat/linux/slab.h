/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_SLAB_H
#define _RTW88_COMPAT_SLAB_H

#include "types.h"
#include "../iokit_shim.h"

/* GFP flags — ignored on macOS */
#define GFP_KERNEL   0
#define GFP_ATOMIC   0
#define GFP_DMA      0
#define GFP_DMA32    0
#define GFP_NOWAIT   0
#define __GFP_ZERO   1u

/*
 * IOMalloc/IOFree require matching sizes.  We prepend a size_t header so
 * kfree() can recover the allocation size without a separate table.
 */
static inline void *kmalloc(size_t size, gfp_t flags)
{
    size_t total = size + sizeof(size_t);
    size_t *block = (size_t *)IOMalloc(total);
    if (!block) return NULL;
    *block = size;
    if (flags & __GFP_ZERO) bzero(block + 1, size);
    return block + 1;
}

static inline void *kzalloc(size_t size, gfp_t flags)
{
    return kmalloc(size, flags | __GFP_ZERO);
}

static inline void *kcalloc(size_t n, size_t size, gfp_t flags)
{
    return kzalloc(n * size, flags);
}

static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags)
{
    return kmalloc(n * size, flags);
}

#define __kzalloc_obj_1(val) kzalloc(sizeof(val), GFP_KERNEL)
#define __kzalloc_obj_2(val, flags) kzalloc(sizeof(val), flags)
#define __kzalloc_obj_chooser(_1, _2, NAME, ...) NAME
#define kzalloc_obj(...) __kzalloc_obj_chooser(__VA_ARGS__, __kzalloc_obj_2, __kzalloc_obj_1)(__VA_ARGS__)

#define __kmalloc_obj_1(val) kmalloc(sizeof(val), GFP_KERNEL)
#define __kmalloc_obj_2(val, flags) kmalloc(sizeof(val), flags)
#define __kmalloc_obj_chooser(_1, _2, NAME, ...) NAME
#define kmalloc_obj(...) __kmalloc_obj_chooser(__VA_ARGS__, __kmalloc_obj_2, __kmalloc_obj_1)(__VA_ARGS__)

static inline void kfree(const void *ptr)
{
    if (!ptr) return;
    /* Kernel pointers on x86-64 macOS have the high bit set.
     * A low address means something corrupted this pointer — log and skip. */
    if ((uintptr_t)ptr < 0xffff000000000000ULL) {
        IOLog("rtw88: kfree: bad pointer %p — skipping (corruption?)\n", ptr);
        return;
    }
    size_t *block = (size_t *)ptr - 1;
    IOFree(block, *block + sizeof(size_t));
}

static inline void kfree_const(const void *ptr)
{
    kfree(ptr);
}

/* krealloc: alloc new, copy, free old */
static inline void *krealloc(void *ptr, size_t new_size, gfp_t flags)
{
    if (!ptr) return kmalloc(new_size, flags);
    size_t *old_block = (size_t *)ptr - 1;
    size_t  old_size  = *old_block;
    void   *newp = kmalloc(new_size, flags);
    if (!newp) return NULL;
    size_t copy = old_size < new_size ? old_size : new_size;
    bcopy(ptr, newp, copy);
    kfree(ptr);
    return newp;
}

static inline char *kstrdup(const char *s, gfp_t flags)
{
    size_t len = strlen(s) + 1;
    char *p = (char *)kmalloc(len, flags);
    if (p) bcopy(s, p, len);
    return p;
}

static inline void *kmemdup(const void *src, size_t len, gfp_t flags)
{
    void *p = kmalloc(len, flags);
    if (p) bcopy(src, p, len);
    return p;
}

/* Slab cache — simplified to plain kmalloc */
struct kmem_cache {
    size_t      obj_size;
    const char *name;
};

static inline struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                                    size_t align, unsigned long flags,
                                                    void (*ctor)(void *))
{
    struct kmem_cache *c = (struct kmem_cache *)kzalloc(sizeof(*c), GFP_KERNEL);
    if (c) { c->obj_size = size; c->name = name; }
    return c;
}

static inline void *kmem_cache_alloc(struct kmem_cache *c, gfp_t flags)
{
    return kzalloc(c->obj_size, flags);
}

static inline void kmem_cache_free(struct kmem_cache *c, void *obj)
{
    kfree(obj);
}

static inline void kmem_cache_destroy(struct kmem_cache *c)
{
    kfree(c);
}

#endif /* _RTW88_COMPAT_SLAB_H */
