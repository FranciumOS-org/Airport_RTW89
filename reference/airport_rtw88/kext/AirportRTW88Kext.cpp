// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// AirportRTW88Kext.cpp — top-level IOService, delegates to PCI or USB device classes

#include "AirportRTW88Kext.hpp"
#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(AirportRTW88Kext, IOService)

bool AirportRTW88Kext::init(OSDictionary *props)
{
    IOLog("rtw88: AirportRTW88Kext::init\n");
    return super::init(props);
}

IOService *AirportRTW88Kext::probe(IOService *provider, SInt32 *score)
{
    IOLog("rtw88: AirportRTW88Kext::probe\n");
    return super::probe(provider, score);
}

bool AirportRTW88Kext::start(IOService *provider)
{
    IOLog("rtw88: AirportRTW88Kext::start\n");
    if (!super::start(provider)) return false;
    registerService();
    return true;
}

void AirportRTW88Kext::stop(IOService *provider)
{
    IOLog("rtw88: AirportRTW88Kext::stop\n");
    super::stop(provider);
}

void AirportRTW88Kext::free()
{
    super::free();
}
