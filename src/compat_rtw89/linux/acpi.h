/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * ACPI shim for rtw89's acpi.c. The kext has no ACPI namespace access, so every
 * query reports "not found": DSM/RTAG/SAR tables are absent and rtw89 falls back
 * to its built-in defaults. acpi.c itself compiles unmodified — its pure helpers
 * (rtw89_acpi_sar_get_subband etc.) are used by sar.c.
 */
#ifndef _RTW89_COMPAT_ACPI_H
#define _RTW89_COMPAT_ACPI_H

#include "uuid.h"

typedef void *acpi_handle;
typedef u32 acpi_status;
typedef char *acpi_string;
typedef u64 acpi_size;

#define AE_OK        ((acpi_status)0x0000)
#define AE_NOT_FOUND ((acpi_status)0x0005)
#define ACPI_FAILURE(s) ((s) != AE_OK)
#define ACPI_SUCCESS(s) ((s) == AE_OK)

#define ACPI_TYPE_INTEGER 0x01
#define ACPI_TYPE_STRING  0x02
#define ACPI_TYPE_BUFFER  0x03
#define ACPI_TYPE_PACKAGE 0x04

union acpi_object {
    u32 type;
    struct { u32 type; u64 value; } integer;
    struct { u32 type; u32 length; char *pointer; } string;
    struct { u32 type; u32 length; u8 *pointer; } buffer;
    struct { u32 type; u32 count; union acpi_object *elements; } package;
};

#define ACPI_ALLOCATE_BUFFER ((acpi_size)-1)
struct acpi_buffer {
    acpi_size length;
    void *pointer;
};

#define ACPI_HANDLE(dev) ((acpi_handle)NULL)
#define ACPI_FREE(p) kfree(p)

static inline acpi_status acpi_get_handle(acpi_handle parent, acpi_string path, acpi_handle *ret)
{
    (void)parent; (void)path;
    *ret = NULL;
    return AE_NOT_FOUND;
}

static inline acpi_status acpi_evaluate_object(acpi_handle h, acpi_string path,
                                               void *args, struct acpi_buffer *buf)
{
    (void)h; (void)path; (void)args;
    if (buf)
        buf->pointer = NULL;
    return AE_NOT_FOUND;
}

static inline union acpi_object *acpi_evaluate_dsm(acpi_handle h, const guid_t *guid,
                                                   u64 rev, u64 func, union acpi_object *argv4)
{
    (void)h; (void)guid; (void)rev; (void)func; (void)argv4;
    return NULL;
}

#endif
