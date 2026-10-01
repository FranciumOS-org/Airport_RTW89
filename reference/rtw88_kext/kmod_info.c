/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * XNU OSKext::start/stop owns C++ constructors/destructors on x86_64.
 * Module callbacks must not initialize or finalize that runtime again.
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern int printf(const char *format, ...);
#ifdef RTW88_AIRPORT_KMOD
KMOD_EXPLICIT_DECL(com.rtw88.airport, "2.0.0", _start, _stop)
#else
KMOD_EXPLICIT_DECL(com.rtw88.driver, "1.0.1", _start, _stop)
#endif
static kern_return_t rtw88_module_start(kmod_info_t *ki, void *data)
{
    (void)data;
    printf("rtw88: module start %s %s (C++ managed by XNU)\n", ki->name, ki->version);
    return KERN_SUCCESS;
}
static kern_return_t rtw88_module_stop(kmod_info_t *ki, void *data)
{
    (void)ki; (void)data;
    return KERN_SUCCESS;
}
kmod_start_func_t *_realmain = rtw88_module_start;
kmod_stop_func_t *_antimain = rtw88_module_stop;
