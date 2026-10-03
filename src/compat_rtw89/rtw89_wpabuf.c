// SPDX-License-Identifier: GPL-2.0
/*
 * hostap's utils/wpabuf.c, unchanged, with the kext's includes.h taking the
 * place of hostap's: wpabuf.c's own #include "includes.h" finds hostap's
 * header next to it, which wants sockets and stdio, and is then skipped by
 * the shared include guard.
 */
#include "includes.h"
#include "../../third_party/hostap/src/utils/wpabuf.c"
