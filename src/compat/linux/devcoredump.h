/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#pragma once
#include "types.h"
#include "slab.h"

struct device;

/* dev_coredump stubs — kernel cannot dump core files; just release the data */
static inline void dev_coredumpv(struct device *d, void *data, size_t len, gfp_t gfp)
{
    kfree(data);
}

static inline void dev_coredumpm(struct device *d, struct module *m,
                                   void *data, size_t len, gfp_t gfp,
                                   ssize_t (*read_fn)(char *, loff_t, size_t, void *, size_t),
                                   void (*free_fn)(void *))
{
    if (free_fn) free_fn(data);
}
