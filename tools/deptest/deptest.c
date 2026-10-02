/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The smallest kext that depends on IO80211FamilyLegacy. It answers one
 * question after the old Wi-Fi stack has been put into the EFI: can a kext
 * that links against a family OpenCore injected be loaded with kmutil from
 * the running system, or does it have to be injected too (a restart per
 * test)? See tools/deptest.sh.
 */
#include <mach/mach_types.h>
#include <mach/kmod.h>

extern int printf(const char *fmt, ...);
/* IO80211Controller::gMetaClass: only its address is wanted */
extern const char io80211_controller_meta __asm("__ZN17IO80211Controller10gMetaClassE");

static kern_return_t deptest_start(kmod_info_t *ki, void *data)
{
    printf("rtw89 deptest: linked against IO80211FamilyLegacy, IO80211Controller's metaclass is at %p\n",
           (const void *)&io80211_controller_meta);
    return KERN_SUCCESS;
}

static kern_return_t deptest_stop(kmod_info_t *ki, void *data)
{
    return KERN_SUCCESS;
}

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

KMOD_EXPLICIT_DECL(com.rtw89.deptest, "0.1.0", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = deptest_start;
__private_extern__ kmod_stop_func_t *_antimain = deptest_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
