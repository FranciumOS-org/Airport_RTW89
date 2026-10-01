/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AirPort_RTW89: the IOKit side of the driver.
 *
 * Current scope: match the RTL8852BE, hand the platform services (config
 * space, BAR 2, DMA memory, the interrupt) to the Linux side through
 * rtw89_glue.h and let the driver's own probe() bring the chip up once. The
 * radio is started, a scan run and a network joined on request (setProperties,
 * used by tools/rtw89ctl).
 *
 * To the rest of macOS the card is an Ethernet controller: it publishes an
 * IOEthernetInterface (enX) whose link comes up once a network is joined and
 * its keys are in place. Frames are converted to and from 802.11 on the Linux
 * side (rtw89_data.c). Nothing here is Wi-Fi as far as the system can tell,
 * so there is no Wi-Fi menu; that needs IO80211Family and comes later.
 */
#pragma once

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>
#include <IOKit/network/IONetworkStats.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "rtw89_glue.h"

class AirPort_RTW89 : public IOEthernetController {
    OSDeclareDefaultStructors(AirPort_RTW89)

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    /* IONetworkController */
    bool createWorkLoop() override;
    IOWorkLoop *getWorkLoop() const override;
    bool configureInterface(IONetworkInterface *netif) override;
    IOReturn enable(IONetworkInterface *netif) override;
    IOReturn disable(IONetworkInterface *netif) override;
    UInt32 outputPacket(mbuf_t m, void *param) override;
    const OSString *newVendorString() const override;
    const OSString *newModelString() const override;

    /* IOEthernetController */
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    IOReturn setPromiscuousMode(bool active) override;
    IOReturn setMulticastMode(bool active) override;
    IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;

    /* { "RTW89Command" = "up" | "down" | "scan" | "results" | "leave" }, or
     * { "RTW89Command" = "join", "RTW89SSID" = <data> [, "RTW89Passphrase" = <data>] }.
     * Administrators only. */
    IOReturn setProperties(OSObject *properties) override;

private:
    bool setupInterrupt();
    bool setupInterface();
    void interruptOccurred(IOInterruptEventSource *source, int count);
    void publishInfo();
    void publishScanResults();
    void publishLink();
    void teardown();

    /* rtw89_glue_platform callbacks; ctx is the AirPort_RTW89 instance. */
    static uint32_t cfgRead(void *ctx, unsigned int offset, unsigned int width);
    static void cfgWrite(void *ctx, unsigned int offset, unsigned int width,
                         uint32_t value);
    static void *dmaAlloc(void *ctx, size_t size, uint64_t *busAddr, void **cookie);
    static void dmaFree(void *ctx, void *cookie);
    static void irqEnable(void *ctx, bool enable);
    static void linkChanged(void *ctx);
    static void rxFrame(void *ctx, const uint8_t *frame, size_t len);

    IOPCIDevice *_pci = nullptr;
    IOMemoryMap *_mmio = nullptr;
    IOWorkLoop *_workLoop = nullptr;    /* the interrupt's own thread */
    IOInterruptEventSource *_interrupt = nullptr;

    /* The network stack's calls (enable, disable, filters) are serialised on
     * this one. It is separate so that a call that waits for the chip cannot
     * hold up the interrupt it is waiting for. */
    IOWorkLoop *_netWorkLoop = nullptr;
    IOEthernetInterface *_netif = nullptr;
    IONetworkStats *_netStats = nullptr;
    OSDictionary *_mediumDict = nullptr;
    IONetworkMedium *_medium = nullptr;     /* owned by _mediumDict */
    IOEthernetAddress _mac = {};
    bool _netifEnabled = false;             /* the stack has the interface up */
    bool _linkActive = false;
    volatile bool _dataReady = false;       /* frames may pass between driver and stack */
    volatile SInt32 _txBusy = 0;            /* threads inside outputPacket() */
    IOLock *_commandLock = nullptr;     /* serialises setProperties and teardown */
    bool _pciOpen = false;
    bool _probed = false;
};
