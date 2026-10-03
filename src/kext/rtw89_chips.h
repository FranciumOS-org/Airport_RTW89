/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The rtw89 PCIe cards by PCI device ID (vendor 10ec), for the names macOS
 * shows. Shared by the driver and the front; the IDs are the front ends'
 * id_tables (third_party/rtw89/rtw8*e.c) and the Info.plists' IOPCIMatch.
 */
#ifndef _RTW89_CHIPS_H
#define _RTW89_CHIPS_H

#include <stdint.h>

static inline const char *rtw89_chip_name(uint16_t device)
{
    switch (device) {
    case 0xb851:                return "RTL8851BE";
    case 0x8852: case 0xa85a:   return "RTL8852AE";
    case 0xb852: case 0xb85b:   return "RTL8852BE";
    case 0xb520:                return "RTL8852BTE";
    case 0xc852:                return "RTL8852CE";
    case 0x8922: case 0x892b:   return "RTL8922AE";
    case 0x892d: case 0x882d:
    case 0x895d:                return "RTL8922DE";
    default:                    return "rtw89";
    }
}

#endif
