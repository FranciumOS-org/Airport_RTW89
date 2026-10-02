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
#include <kern/thread_call.h>

#include "rtw89_glue.h"
#include "../front/rtw89_front_api.h"

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
    IOReturn outputStart(IONetworkInterface *netif, IOOptionBits options) override;
    const OSString *newVendorString() const override;
    const OSString *newModelString() const override;

    /* IOEthernetController */
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    IOReturn setPromiscuousMode(bool active) override;
    IOReturn setMulticastMode(bool active) override;
    IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;

    /* { "RTW89Command" = "up" | "down" | "scan" | "results" | "leave" | "flush-on" | "flush-off" |
     *                    "ax-on" | "ax-off" | "rejoin-on" | "rejoin-off" | "probe" | "drop" }, or
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
    static void txWake(void *ctx);
    bool transmit(mbuf_t m);

    /* Native Wi-Fi (AirPortRTW89Native.cpp): connected to AirPortRTW89Front
     * instead of publishing an Ethernet interface. */
    struct NativeState;
    bool connectFront();
    void disconnectFront();
    void nativeRxFrame(const uint8_t *frame, size_t len);
    void nativeTxWake();
    void nativeLink(const struct rtw89_glue_link &link);
    void nativeNotify(UInt32 events);
    void nativeEvents();
    static void nativeEventsCall(thread_call_param_t self, thread_call_param_t);
    void nativePost(unsigned int msg, void *data = nullptr, size_t len = 0);
    int nativeRequest(bool isSet, int number, void *data);
    int nativeAssociate(void *data);
    static void scanDone(void *ctx, bool aborted);
    static void backGetMac(void *ctx, uint8_t mac[6]);
    static int backSetMac(void *ctx, const uint8_t mac[6]);
    static int backRequest(void *ctx, unsigned int type, int number, void *interface, void *data);
    static int backIoctl(void *ctx, void *interface, void *vif, void *ifnet, unsigned long cmd,
                         void *data, bool *handled);
    static int backIoctlSet(void *ctx, void *interface, void *vif, void *skywalk, void *data,
                            bool *handled);
    static int backEnable(void *ctx, bool on);
    static uint32_t backOutput(void *ctx, mbuf_t m);
    static void backPower(void *ctx, bool on);

    bool _native = false;
    /* Link changes and finished scans reach IO80211 from a thread of their
     * own: the driver reports them with its locks held, and IO80211 may be
     * waiting for those very locks inside a request of its own. */
    thread_call_t _nativeCall = nullptr;
    volatile UInt32 _nativeEvents = 0;
    volatile bool _nativeClosing = false;
    volatile SInt32 _nativeRunning = 0;
    IOService *_front = nullptr;
    NativeState *_ns = nullptr;

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
