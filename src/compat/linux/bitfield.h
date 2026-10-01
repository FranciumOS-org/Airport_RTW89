/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_BITFIELD_H
#define _RTW88_COMPAT_BITFIELD_H

#include "types.h"
#include "bitops.h"

/* FIELD_PREP — set a field value (pre-shifted) */
#define FIELD_PREP(_mask, _val) \
    (((u64)(_val) << __ffs((unsigned long)(_mask))) & (u64)(_mask))

/* FIELD_GET — extract a field value */
#define FIELD_GET(_mask, _val) \
    (((u64)(_val) & (u64)(_mask)) >> __ffs((unsigned long)(_mask)))

/* FIELD_FIT — check value fits in mask */
#define FIELD_FIT(_mask, _val) \
    (!((((u64)(_val)) << __ffs((unsigned long)(_mask))) & ~(u64)(_mask)))

/* Typed bitfield encode/decode (Linux 5.x+ API) */
static inline u8 u8_encode_bits(u8 v, u8 mask)
{
    return (u8)FIELD_PREP((u64)mask, (u64)v);
}
static inline u8 u8_get_bits(u8 v, u8 mask)
{
    return (u8)FIELD_GET((u64)mask, (u64)v);
}
static inline void u8p_replace_bits(u8 *p, u8 val, u8 mask)
{
    *p = (*p & (u8)~mask) | u8_encode_bits(val, mask);
}

static inline u16 u16_encode_bits(u16 v, u16 mask)
{
    return (u16)FIELD_PREP((u64)mask, (u64)v);
}
static inline u16 u16_get_bits(u16 v, u16 mask)
{
    return (u16)FIELD_GET((u64)mask, (u64)v);
}

static inline u32 u32_encode_bits(u32 v, u32 mask)
{
    return (u32)FIELD_PREP((u64)mask, (u64)v);
}
static inline u32 u32_get_bits(u32 v, u32 mask)
{
    return (u32)FIELD_GET((u64)mask, (u64)v);
}
static inline void u32p_replace_bits(u32 *p, u32 val, u32 mask)
{
    *p = (*p & ~mask) | u32_encode_bits(val, mask);
}

static inline u64 u64_encode_bits(u64 v, u64 mask)
{
    return (u64)FIELD_PREP(mask, v);
}

/* Little-endian bitfield helpers */
static inline u16 le16_get_bits(__le16 v, u16 mask)
{
    return u16_get_bits((u16)v, mask);
}
static inline u32 le32_get_bits(__le32 v, u32 mask)
{
    return u32_get_bits((u32)v, mask);
}
static inline __le32 le32_encode_bits(u32 v, u32 mask)
{
    return (__le32)u32_encode_bits(v, mask);
}
static inline void le32p_replace_bits(__le32 *p, u32 val, u32 mask)
{
    u32 tmp = (u32)*p;
    u32p_replace_bits(&tmp, val, mask);
    *p = (__le32)tmp;
}
static inline u64 le64_get_bits(__le64 v, u64 mask)
{
    return (u64)FIELD_GET(mask, (u64)v);
}

#endif /* _RTW88_COMPAT_BITFIELD_H */
