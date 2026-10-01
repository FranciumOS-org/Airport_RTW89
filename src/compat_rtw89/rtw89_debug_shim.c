// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Replacement for the message half of rtw89's debug.c (the rest is debugfs,
 * which macOS doesn't have). Built with CONFIG_RTW89_DEBUGMSG so rtw89_debug()
 * calls survive; the mask comes from the boot-arg  rtw89_debug=0x...  (0 = off).
 *
 * Useful masks for bring-up (enum rtw89_debug_mask in debug.h):
 *   RTW89_DBG_FW (firmware download/H2C/C2H), RTW89_DBG_HCI (PCI rings),
 *   RTW89_DBG_TXRX, RTW89_DBG_RFK (calibration). 0xffffffff = everything.
 */
#include "core.h"
#include "debug.h"
#include <pexpert/pexpert.h>
#include <stdarg.h>

unsigned int rtw89_debug_mask;

void rtw89_compat_debug_init(void)
{
    unsigned int mask = 0;

    if (PE_parse_boot_argn("rtw89_debug", &mask, sizeof(mask)))
        rtw89_debug_mask = mask;
}

void rtw89_debug(struct rtw89_dev *rtwdev, enum rtw89_debug_mask mask,
                 const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (!(rtw89_debug_mask & mask))
        return;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    IOLog("[rtw89 DBG] %s", buf);
}
