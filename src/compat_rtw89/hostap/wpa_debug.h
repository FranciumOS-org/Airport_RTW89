/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hostap's wpa_debug.h, reduced. Messages from MSG_INFO up go to the driver's
 * log; debug messages and every hex dump are dropped, so no key material can
 * ever be logged.
 */
#ifndef RTW89_HOSTAP_WPA_DEBUG_H
#define RTW89_HOSTAP_WPA_DEBUG_H

#include <stddef.h>
#include "wpabuf.h"         /* hostap's own wpa_debug.h brings it in */

enum { MSG_EXCESSIVE, MSG_MSGDUMP, MSG_DEBUG, MSG_INFO, MSG_WARNING, MSG_ERROR };

void rtw89_hostap_printf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define wpa_printf(level, ...) rtw89_hostap_printf((level), __VA_ARGS__)
#define wpa_hexdump(l, t, b, n) do { (void)(l); (void)(t); (void)(b); (void)(n); } while (0)
#define wpa_hexdump_key(l, t, b, n) wpa_hexdump(l, t, b, n)
#define wpa_hexdump_ascii(l, t, b, n) wpa_hexdump(l, t, b, n)
#define wpa_hexdump_ascii_key(l, t, b, n) wpa_hexdump(l, t, b, n)
#define wpa_hexdump_buf(l, t, b) do { (void)(l); (void)(t); (void)(b); } while (0)
#define wpa_hexdump_buf_key(l, t, b) wpa_hexdump_buf(l, t, b)

#endif
