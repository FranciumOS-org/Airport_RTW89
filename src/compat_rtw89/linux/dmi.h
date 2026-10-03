/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * DMI quirk tables: never match (no SMBIOS matching from the kext yet).
 */
#ifndef _RTW89_COMPAT_DMI_H
#define _RTW89_COMPAT_DMI_H

#include "../../compat/linux/types.h"

enum dmi_field {
    DMI_NONE, DMI_BIOS_VENDOR, DMI_SYS_VENDOR, DMI_PRODUCT_NAME,
    DMI_PRODUCT_VERSION, DMI_PRODUCT_SKU, DMI_BOARD_VENDOR, DMI_BOARD_NAME,
};

struct dmi_strmatch {
    unsigned char slot:7;
    unsigned char exact_match:1;
    char substr[79];
};

struct dmi_system_id {
    int (*callback)(const struct dmi_system_id *);
    const char *ident;
    struct dmi_strmatch matches[4];
    void *driver_data;
};

#define DMI_MATCH(a, b)       { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b) { .slot = a, .substr = b, .exact_match = 1 }

static inline const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list)
{
    (void)list;
    return NULL;
}

#endif
