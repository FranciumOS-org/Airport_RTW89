/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Linux type shims for rtw88 macOS kext port.
 * Kernel-safe: no userspace headers included here.
 */
#ifndef _RTW88_COMPAT_TYPES_H
#define _RTW88_COMPAT_TYPES_H

/* Use only compiler built-ins and explicit externs — no libc cascade. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <machine/endian.h>

#ifndef __LITTLE_ENDIAN
#if defined(__LITTLE_ENDIAN__) || (defined(BYTE_ORDER) && BYTE_ORDER == LITTLE_ENDIAN)
#define __LITTLE_ENDIAN 1
#endif
#endif

#ifdef KERNEL
/* In KERNEL (C++) builds, use MacKernelSDK's string.h which provides
 * memcpy/memmove/etc. as compiler builtins with bounds checking. */
#include <string.h>
#else
/* Memory/string functions — declared as extern for C driver compat builds */
extern void    *memcpy(void *dst, const void *src, size_t n);
extern void    *memset(void *s, int c, size_t n);
extern void    *memmove(void *dst, const void *src, size_t n);
extern int      memcmp(const void *a, const void *b, size_t n);
extern void    *memchr(const void *s, int c, size_t n);
extern void     bzero(void *s, size_t n);
extern void     bcopy(const void *src, void *dst, size_t n);

extern size_t   strlen(const char *s);
extern size_t   strnlen(const char *s, size_t maxlen);
extern char    *strcpy(char *dst, const char *src);
extern char    *strncpy(char *dst, const char *src, size_t n);
extern size_t   strlcpy(char *dst, const char *src, size_t size);
extern size_t   strlcat(char *dst, const char *src, size_t size);
extern int      strcmp(const char *s1, const char *s2);
extern int      strncmp(const char *s1, const char *s2, size_t n);
extern char    *strchr(const char *s, int c);
extern char    *strrchr(const char *s, int c);
extern char    *strstr(const char *haystack, const char *needle);

/* printf-family */
extern int snprintf(char *str, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
extern int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));
extern int scnprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ffs/fls — provided by XNU kernel (libkern) */
extern int ffs(unsigned int) __attribute__((const));
extern int fls(unsigned int) __attribute__((const));
#endif /* KERNEL */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;

typedef uint16_t __le16;
typedef uint32_t __le32;
typedef uint64_t __le64;
typedef uint16_t __be16;
typedef uint32_t __be32;

typedef unsigned long  ulong;
typedef unsigned long  kernel_ulong_t;
typedef unsigned long  dma_addr_t;
typedef unsigned long  phys_addr_t;
typedef unsigned long  resource_size_t;
typedef unsigned int   gfp_t;
typedef unsigned int   fmode_t;
typedef long           loff_t;
typedef unsigned long  pgoff_t;
typedef unsigned int   uint;

#define __iomem
#define __user
#define __force
#define __must_check
#define __packed         __attribute__((packed))
#define __aligned(x)     __attribute__((aligned(x)))
#define __printf(a, b)   __attribute__((format(printf, a, b)))
#define __attribute_const__ __attribute__((const))
#define noinline         __attribute__((noinline))
#define __maybe_unused   __attribute__((unused))

/* Endian — macOS x86/ARM is little-endian */
#define cpu_to_le16(x) ((u16)(x))
#define cpu_to_le32(x) ((u32)(x))
#define cpu_to_le64(x) ((u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be16(x) __builtin_bswap16(x)
#define cpu_to_be32(x) __builtin_bswap32(x)
#define be16_to_cpu(x) __builtin_bswap16(x)
#define be32_to_cpu(x) __builtin_bswap32(x)

#define le16_to_cpup(p) le16_to_cpu(*(p))
#define le32_to_cpup(p) le32_to_cpu(*(p))

/* Double-underscore variants used by some Linux driver code */
#define __le16_to_cpu(x)  le16_to_cpu(x)
#define __le32_to_cpu(x)  le32_to_cpu(x)
#define __le64_to_cpu(x)  le64_to_cpu(x)
#define __cpu_to_le16(x)  cpu_to_le16(x)
#define __cpu_to_le32(x)  cpu_to_le32(x)
#define __cpu_to_le64(x)  cpu_to_le64(x)
#define __be16_to_cpu(x)  be16_to_cpu(x)
#define __be32_to_cpu(x)  be32_to_cpu(x)

#define BITS_PER_LONG      (sizeof(long) * 8)
#define BITS_PER_LONG_LONG 64

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)
#endif

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define ALIGN(x, a)      (((x) + (a) - 1) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define ARRAY_SIZE(x)    (sizeof(x) / sizeof((x)[0]))
#define sizeof_field(t, f) (sizeof(((t *)0)->f))

/* list_head */
struct list_head { struct list_head *next, *prev; };

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list; list->prev = list;
}
static inline void __list_add(struct list_head *n,
                               struct list_head *prev, struct list_head *next)
{
    next->prev = n; n->next = next; n->prev = prev; prev->next = n;
}
static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
    __list_add(n, head->prev, head);
}
static inline void list_add(struct list_head *n, struct list_head *head)
{
    __list_add(n, head, head->next);
}
static inline void list_del(struct list_head *entry)
{
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = NULL; entry->prev = NULL;
}
static inline void list_del_init(struct list_head *entry)
{
    list_del(entry); INIT_LIST_HEAD(entry);
}
static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}
static inline void list_move_tail(struct list_head *list, struct list_head *head)
{
    list_del(list); list_add_tail(list, head);
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_last_entry(ptr, type, member)  list_entry((ptr)->prev, type, member)

#define list_first_entry_or_null(ptr, type, member) \
    ({ struct list_head *head__ = (ptr); \
       struct list_head *pos__ = head__->next; \
       pos__ != head__ ? list_entry(pos__, type, member) : NULL; })

#define offsetofend(TYPE, MEMBER) \
    (offsetof(TYPE, MEMBER) + sizeof(((TYPE *)0)->MEMBER))

#define list_for_each_entry(pos, head, member) \
    for (pos = container_of((head)->next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = container_of(pos->member.next, __typeof__(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = container_of((head)->next, __typeof__(*pos), member), \
         n   = container_of(pos->member.next, __typeof__(*pos), member); \
         &pos->member != (head); \
         pos = n, n = container_of(n->member.next, __typeof__(*n), member))

/* hlist */
struct hlist_head { struct hlist_node *first; };
struct hlist_node { struct hlist_node *next, **pprev; };
#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = HLIST_HEAD_INIT
static inline void INIT_HLIST_HEAD(struct hlist_head *h) { h->first = NULL; }
static inline int  hlist_empty(const struct hlist_head *h) { return !h->first; }
static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    n->next = h->first;
    if (h->first) h->first->pprev = &n->next;
    h->first = n; n->pprev = &h->first;
}
static inline void hlist_del_init(struct hlist_node *n)
{
    if (n->pprev) {
        *n->pprev = n->next;
        if (n->next) n->next->pprev = n->pprev;
        INIT_LIST_HEAD((struct list_head *)n);
    }
}
#define hlist_for_each_entry(pos, head, member) \
    for (pos = (head)->first ? container_of((head)->first, __typeof__(*pos), member) : NULL; \
         pos; \
         pos = pos->member.next ? container_of(pos->member.next, __typeof__(*pos), member) : NULL)

#endif /* _RTW88_COMPAT_TYPES_H */
