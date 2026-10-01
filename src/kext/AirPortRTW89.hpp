/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AirPort_RTW89: the IOKit side of the driver.
 *
 * Current scope: match the RTL8852BE, hand the platform services (config
 * space, BAR 2, DMA memory, the interrupt) to the Linux side through
 * rtw89_glue.h and let the driver's own probe() bring the chip up once. The
 * radio can then be started and a scan run on request (setProperties, used by
 * tools/rtw89ctl). No network interface is published yet.
 */
#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "rtw89_glue.h"

class AirPort_RTW89 : public IOService {
    OSDeclareDefaultStructors(AirPort_RTW89)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    /* { "RTW89Command" = "up" | "down" | "scan" | "results" }, administrators only. */
    IOReturn setProperties(OSObject *properties) override;

private:
    bool setupInterrupt();
    void interruptOccurred(IOInterruptEventSource *source, int count);
    void publishInfo();
    void publishScanResults();
    void teardown();

    /* rtw89_glue_platform callbacks; ctx is the AirPort_RTW89 instance. */
    static uint32_t cfgRead(void *ctx, unsigned int offset, unsigned int width);
    static void cfgWrite(void *ctx, unsigned int offset, unsigned int width,
                         uint32_t value);
    static void *dmaAlloc(void *ctx, size_t size, uint64_t *busAddr, void **cookie);
    static void dmaFree(void *ctx, void *cookie);
    static void irqEnable(void *ctx, bool enable);

    IOPCIDevice *_pci = nullptr;
    IOMemoryMap *_mmio = nullptr;
    IOWorkLoop *_workLoop = nullptr;
    IOInterruptEventSource *_interrupt = nullptr;
    IOLock *_commandLock = nullptr;     /* serialises setProperties and teardown */
    bool _pciOpen = false;
    bool _probed = false;
};
