/* SPDX-License-Identifier: GPL-2.0 */
/*
 * AirPortRTW89Front: the Wi-Fi device as macOS sees it. See rtw89_front_api.h
 * for why this is a kext of its own and what it leaves to the driver proper.
 * The IO80211Controller lifecycle follows AirPort_RTW88 (reference/), which
 * follows AirportItlwm.
 */
#pragma once

#include <IOKit/80211/IO80211Controller.h>
#include <IOKit/80211/IO80211Interface.h>
#include <IOKit/80211/IO80211WorkLoop.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>

#include "rtw89_front_api.h"

class AirPortRTW89FrontInterface : public IO80211Interface {
    OSDeclareDefaultStructors(AirPortRTW89FrontInterface)

public:
    /* Received frames are plain Ethernet by the time they get here. IO80211
     * only gets to see EAPOL frames, and only while its supplicant is the
     * one doing the key handshake. */
    UInt32 inputPacket(mbuf_t packet, UInt32 length = 0, IOOptionBits options = 0,
                       void *param = 0) override;
};

class AirPortRTW89Front : public IO80211Controller {
    OSDeclareDefaultStructors(AirPortRTW89Front)

public:
    /* IOService */
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                  void *param1, void *param2, void *param3,
                                  void *param4) override;
    IOReturn registerWithPolicyMaker(IOService *policyMaker) override;
    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;

    /* IONetworkController / IOEthernetController */
    bool createWorkLoop() override;
    IOWorkLoop *getWorkLoop() const override;
    const OSString *newVendorString() const override;
    const OSString *newModelString() const override;
    IONetworkInterface *createInterface() override;
    IOReturn enable(IONetworkInterface *iface) override;
    IOReturn disable(IONetworkInterface *iface) override;
    IOReturn selectMedium(const IONetworkMedium *medium) override;
    UInt32 outputPacket(mbuf_t m, void *param) override;
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    IOReturn setHardwareAddress(const IOEthernetAddress *addr) override;
    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    IOReturn setMulticastMode(bool active) override;
    IOReturn setMulticastList(IOEthernetAddress *addrs, UInt32 count) override;
    IOReturn setPromiscuousMode(bool active) override;

    /* IO80211Controller */
    IOReturn getHardwareAddressForInterface(IO80211Interface *iface,
                                            IOEthernetAddress *addr) override;
    bool useAppleRSNSupplicant(IO80211Interface *) override;
    SInt32 apple80211_ioctl(IO80211Interface *interface, IO80211VirtualInterface *vif,
                            ifnet_t net, unsigned long cmd, void *data) override;
    SInt32 apple80211_ioctl_set(IO80211Interface *interface, IO80211VirtualInterface *vif,
                                IO80211SkywalkInterface *skywalk, void *data) override;
    SInt32 apple80211Request(unsigned int type, int number, IO80211Interface *interface,
                             void *data) override;
    SInt32 stopDMA() override;
    UInt32 hardwareOutputQueueDepth(IO80211Interface *interface) override;
    SInt32 performCountryCodeOperation(IO80211Interface *interface,
                                       IO80211CountryCodeOp op) override;
    SInt32 enableFeature(IO80211FeatureCode feature, void *data) override;
    SInt32 monitorModeSetEnabled(IO80211Interface *interface, bool enabled, UInt32 mode) override;

private:
    IOReturn connectBack(const struct rtw89_back_ops *back, struct rtw89_front_ops *front);
    IOReturn disconnectBack();
    bool publishMedium();

    /* rtw89_front_ops */
    static void opPostMessage(void *ctx, unsigned int msg, void *data, size_t len);
    static void opSetLink(void *ctx, bool up, uint64_t bps, unsigned int reason);
    static mbuf_t opAllocPacket(void *ctx, unsigned int len);
    static void opInputPacket(void *ctx, mbuf_t m);
    static void opTxWake(void *ctx);
    static int opSuperIoctl(void *ctx, void *interface, void *vif, void *ifnet,
                            unsigned long cmd, void *data);
    static int opSuperIoctlSet(void *ctx, void *interface, void *vif, void *skywalk, void *data);
    static void opSetAppleRsn(void *ctx, bool on);

    IOPCIDevice *_pci = nullptr;
    IO80211WorkLoop *_workLoop = nullptr;
    AirPortRTW89FrontInterface *_netif = nullptr;
    const IONetworkMedium *_medium = nullptr;
    IOEthernetAddress _mac = {};
    bool _haveMac = false;
    bool _superStarted = false;

    /* The back's table: read-locked around every call into it, write-locked
     * to change it. */
    bool backEnter();
    void backLeave();

    IOLock *_backLock = nullptr;         /* connecting and disconnecting */
    volatile SInt32 _backCalls = 0;     /* calls into the driver proper in progress */
    struct rtw89_back_ops _back = {};
    volatile bool _connected = false;
};
