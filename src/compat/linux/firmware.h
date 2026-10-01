/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_FIRMWARE_H
#define _RTW88_COMPAT_FIRMWARE_H

#include "types.h"
#include "kernel.h"
#include "slab.h"

struct firmware {
    size_t    size;
    const u8 *data;
    size_t    _alloc_size;  /* private — do not use in driver code */
};

struct module;
struct device;

/* Implemented in rtw88_compat.c — backed by VFS read from kext Resources/ */
int  request_firmware_nowait(struct module *module, bool uevent,
                              const char *name, struct device *device,
                              gfp_t gfp, void *context,
                              void (*cont)(const struct firmware *fw, void *ctx));

void release_firmware(const struct firmware *fw);

/* rtw88_load_firmware_sync: VFS-based synchronous read, defined in rtw88_compat.c */
int rtw88_load_firmware_sync(const char *name, const struct firmware **fw_out);

static inline int request_firmware(const struct firmware **fw_out,
                                    const char *name, struct device *dev)
{
    (void)dev;
    return rtw88_load_firmware_sync(name, fw_out);
}

static inline int firmware_request_nowarn(const struct firmware **fw,
                                           const char *name, struct device *dev)
{
    return request_firmware(fw, name, dev);
}

#endif /* _RTW88_COMPAT_FIRMWARE_H */
