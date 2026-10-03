/* SPDX-License-Identifier: GPL-2.0 */
/* Stands in for hostap's utils/includes.h: no sockets, no stdio in the kernel. */
#ifndef RTW89_HOSTAP_INCLUDES_H
#define RTW89_HOSTAP_INCLUDES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/types.h>
#include <string.h>
#ifndef KERNEL
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#endif

#endif
