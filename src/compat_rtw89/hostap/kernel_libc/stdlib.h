/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The few stdlib.h functions Mbed TLS and hostap use, for the kernel build
 * only (this directory is not on the userspace test's include path).
 */
#ifndef RTW89_KERNEL_STDLIB_H
#define RTW89_KERNEL_STDLIB_H

#include <stddef.h>

void *rtw89_hostap_calloc(size_t n, size_t size);
void rtw89_hostap_free(void *ptr);
/* hostap calls it when a buffer would overflow: memory is no longer sound */
__attribute__((noreturn)) void rtw89_hostap_abort(void);

#define calloc(n, s) rtw89_hostap_calloc((n), (s))
#define free(p) rtw89_hostap_free(p)
#define abort() rtw89_hostap_abort()

static inline int abs(int v)
{
    return v < 0 ? -v : v;
}

#endif
