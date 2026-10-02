// SPDX-License-Identifier: GPL-2.0
/*
 * The driver's own log: every line it writes with IOLog also goes into a
 * 64 KB ring here, which `rtw89ctl log` reads back (setProperties "log").
 *
 * Loaded from the EFI, the driver's IOLog lines never reach the unified log,
 * and the kernel's message buffer (dmesg) is small and fills with other
 * kexts' lines within seconds. The driver is built with -DIOLog=rtw89_iolog
 * (Makefile: LOGRING_DEF); this is the one file that calls the real IOLog.
 * Nothing secret is logged anywhere in the driver, so neither is it here.
 */
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <stdarg.h>

#undef IOLog
extern "C" void IOLog(const char *format, ...) __attribute__((format(printf, 1, 2)));

static char gRing[64 * 1024];
static size_t gHead;            /* next byte to write */
static bool gWrapped;
static IOSimpleLock *gRingLock;

extern "C" void rtw89_logring_init(void)
{
    if (!gRingLock)
        gRingLock = IOSimpleLockAlloc();
}

extern "C" void rtw89_logring_free(void)
{
    if (gRingLock) {
        IOSimpleLockFree(gRingLock);
        gRingLock = nullptr;
    }
}

static void ringAppend(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        gRing[gHead++] = s[i];
        if (gHead == sizeof(gRing)) {
            gHead = 0;
            gWrapped = true;
        }
    }
}

extern "C" void rtw89_iolog(const char *format, ...)
{
    char line[512];
    int prefix, len;
    clock_sec_t sec;
    clock_usec_t usec;
    va_list ap;

    clock_get_calendar_microtime(&sec, &usec);
    prefix = snprintf(line, sizeof(line), "%lu.%03u ", (unsigned long)sec, (unsigned)(usec / 1000));
    va_start(ap, format);
    len = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, ap);
    va_end(ap);
    if (len < 0)
        return;
    len += prefix;
    if ((size_t)len >= sizeof(line))
        len = sizeof(line) - 1;

    if (gRingLock) {
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(gRingLock);

        ringAppend(line, (size_t)len);
        if (len && line[len - 1] != '\n')
            ringAppend("\n", 1);
        IOSimpleLockUnlockEnableInterrupt(gRingLock, is);
    }
    IOLog("%s", line + prefix);
}

/* Oldest line first; the first, cut-off line of a wrapped ring is dropped.
 * Returns the length written to @out (no terminating zero). */
extern "C" size_t rtw89_logring_copy(char *out, size_t max)
{
    size_t n = 0, start, avail;

    if (!gRingLock || !out || !max)
        return 0;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(gRingLock);
    start = gWrapped ? gHead : 0;
    avail = gWrapped ? sizeof(gRing) : gHead;
    if (gWrapped) {
        /* skip to the first whole line */
        while (avail && gRing[start] != '\n') {
            start = (start + 1) % sizeof(gRing);
            avail--;
        }
        if (avail) {
            start = (start + 1) % sizeof(gRing);
            avail--;
        }
    }
    while (n < avail && n < max) {
        out[n] = gRing[(start + n) % sizeof(gRing)];
        n++;
    }
    IOSimpleLockUnlockEnableInterrupt(gRingLock, is);
    return n;
}
