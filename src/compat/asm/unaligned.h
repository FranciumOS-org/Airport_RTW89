/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#pragma once
#include "../linux/types.h"
#include <string.h>

static inline u16 get_unaligned_le16(const void *p)
{
    u16 v; memcpy(&v, p, 2); return le16_to_cpu(v);
}
static inline u32 get_unaligned_le32(const void *p)
{
    u32 v; memcpy(&v, p, 4); return le32_to_cpu(v);
}
static inline u64 get_unaligned_le64(const void *p)
{
    u64 v; memcpy(&v, p, 8); return v;
}
static inline u16 get_unaligned_be16(const void *p)
{
    u16 v; memcpy(&v, p, 2); return be16_to_cpu(v);
}
static inline u32 get_unaligned_be32(const void *p)
{
    u32 v; memcpy(&v, p, 4); return be32_to_cpu(v);
}
static inline void put_unaligned_le16(u16 val, void *p)
{
    u16 v = cpu_to_le16(val); memcpy(p, &v, 2);
}
static inline void put_unaligned_le32(u32 val, void *p)
{
    u32 v = cpu_to_le32(val); memcpy(p, &v, 4);
}
static inline void put_unaligned_be16(u16 val, void *p)
{
    u16 v = cpu_to_be16(val); memcpy(p, &v, 2);
}
static inline void put_unaligned_be32(u32 val, void *p)
{
    u32 v = cpu_to_be32(val); memcpy(p, &v, 4);
}
static inline u16 get_unaligned(const u16 *p) { return get_unaligned_le16(p); }
