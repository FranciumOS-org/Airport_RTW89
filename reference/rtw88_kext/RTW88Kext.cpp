// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// RTW88Kext.cpp — top-level IOService, delegates to PCI or USB device classes

#include "RTW88Kext.hpp"
#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(RTW88Kext, IOService)

bool RTW88Kext::init(OSDictionary *props)
{
    IOLog("rtw88: RTW88Kext::init\n");
    return super::init(props);
}

IOService *RTW88Kext::probe(IOService *provider, SInt32 *score)
{
    IOLog("rtw88: RTW88Kext::probe\n");
    return super::probe(provider, score);
}

bool RTW88Kext::start(IOService *provider)
{
    IOLog("rtw88: RTW88Kext::start\n");
    if (!super::start(provider)) return false;
    registerService();
    return true;
}

void RTW88Kext::stop(IOService *provider)
{
    IOLog("rtw88: RTW88Kext::stop\n");
    super::stop(provider);
}

void RTW88Kext::free()
{
    super::free();
}
