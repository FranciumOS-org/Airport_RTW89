/* SPDX-License-Identifier: GPL-2.0 */
/* The stdio.h functions Mbed TLS and hostap use, for the kernel build only:
 * the kernel exports these two. */
#ifndef RTW89_KERNEL_STDIO_H
#define RTW89_KERNEL_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int snprintf(char *str, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *str, size_t size, const char *format, va_list ap) __attribute__((format(printf, 3, 0)));

#endif
