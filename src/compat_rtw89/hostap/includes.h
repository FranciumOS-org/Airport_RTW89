/* SPDX-License-Identifier: GPL-2.0 */
/* Stands in for hostap's utils/includes.h: no sockets, no stdio in the kernel. */
#ifndef INCLUDES_H /* the same guard as hostap's, so whichever comes first wins */
#define INCLUDES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/types.h>
#include <string.h>
/* in the kernel build these two are hostap/kernel_libc's */
#include <stdlib.h>
#include <stdio.h>
#ifndef KERNEL
#include <errno.h>
#include <ctype.h>
#endif

#endif
