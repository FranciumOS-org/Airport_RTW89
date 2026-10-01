/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * AirportRTW88.hpp — IO80211Controller subclass; native Wi-Fi menu support
 * for the rtw88 macOS port (Feixiao), mirroring OpenIntelWireless/itlwm's
 * AirportItlwm.
 *
 * This is a SEPARATE kext target from rtw88.kext (which stays IOEthernet-
 * based, like itlwm.kext), same as AirportItlwm.kext is separate from
 * itlwm.kext. Do not load both against the same PCI device.
 *
 * IOClass in this target's Info.plist should be AirportRTW88, IOProviderClass
 * stays IOPCIDevice with the same IOPCIMatch entries as rtw88.kext.
 */
#pragma once

#include <IOKit/80211/IO80211Controller.h>
#include <IOKit/80211/IO80211Interface.h>
#include <IOKit/80211/IO80211VirtualInterface.h>
#include <IOKit/80211/IO80211P2PInterface.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <kern/thread.h>

#include "RTW88IEEE80211.hpp"   /* for RTW88EventDelegate, RTW88BSS, RTW88State */
#include "RTW88UserClient.hpp"  /* for struct RTW88StateResult */
#include "RTW88AWDLManager.hpp"


/* Ventura MacKernelSDK exposes APPLE80211_IOC_ROAM_PROFILE (216) but
 * does not publish the corresponding payload type. Keep the ABI local
 * instead of depending on newer IO80211 headers. Layout matches the
 * legacy AirportItlwm payload and is exactly 76 bytes. */
struct RTW88RoamProfile {
    int8_t      flags;
    int8_t      trigger;
    int8_t      rssi_lower;
    int8_t      rssi_boost_delta;
    int8_t      rssi_boost_thresh;
    int8_t      delta;
    uint16_t    backoff_multiplier;
    uint16_t    full_scan_period;
    uint16_t    init_scan_period;
    uint16_t    nfscan;
    uint16_t    max_scan_period;
} __attribute__((packed));

struct RTW88RoamProfileBandData {
    uint32_t    version;
    uint32_t    flags;
    uint32_t    profile_cnt;
    RTW88RoamProfile profiles[4];
} __attribute__((packed));

static_assert(sizeof(RTW88RoamProfileBandData) == 76,
              "RTW88 roam profile ABI must be 76 bytes");

/* Ventura's public-ish header has the selector numbers below but omits a few
 * payload definitions that AirportItlwm v2.2.0 uses.  Keep private ABI shims
 * local to this driver rather than replacing MacKernelSDK with a newer header. */
struct RTW88VHTMCSIndexSetData {
    uint32_t version;
    uint16_t mcs_map;
} __attribute__((packed));

struct RTW88MCSVHTData {
    uint32_t version;
    uint32_t index;
    uint32_t nss;
    uint32_t bw;
    uint32_t guard_interval;
} __attribute__((packed));

struct RTW88StaRoamData {
    uint32_t version;
    uint8_t  rcc_channels;
    uint8_t  unk1;
    uint8_t  target_channel;
    uint8_t  target_bssid[APPLE80211_ADDR_LEN];
} __attribute__((packed));

struct RTW88IEData {
    uint32_t version;
    uint32_t frame_type_flags;
    uint32_t add;
    uint32_t signature_len;
    uint32_t ie_len;
    uint32_t pad1;
    uint8_t  ie[2048];
} __attribute__((packed));

static_assert(sizeof(RTW88VHTMCSIndexSetData) == 6, "VHT MCS map ABI changed");
static_assert(sizeof(RTW88MCSVHTData) == 20, "MCS/VHT ABI changed");
static_assert(sizeof(RTW88StaRoamData) == 13, "STA roam ABI changed");
static_assert(sizeof(RTW88IEData) == 2072, "IE ABI changed");

class AirportRTW88Interface;

class AirportRTW88 : public IO80211Controller, public RTW88EventDelegate, public RTW88RxDelegate, public RTW88HwOps {
    OSDeclareDefaultStructors(AirportRTW88)

public:
    static void awdlTimerFired(OSObject *owner, IOTimerEventSource *timer);
    /* IOService */
    bool     init(OSDictionary *props) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    void     free() override;
    void     systemWillShutdown(IOOptionBits specifier) override;
    IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) override;
    IOReturn registerWithPolicyMaker(IOService *policyMaker) override;

    /* IONetworkController (required minimum set) */
    const OSString *newVendorString() const override;
    const OSString *newModelString() const override;
    IOReturn enable(IONetworkInterface *iface) override;
    IOReturn disable(IONetworkInterface *iface) override;
    IONetworkInterface *createInterface() override;
    bool     configureInterface(IONetworkInterface *iface) override;
    bool     createMediumTables(const IONetworkMedium **primary);
    bool     createWorkLoop() override;
    IOWorkLoop *getWorkLoop() const override;
    IOReturn selectMedium(const IONetworkMedium *medium) override;
    UInt32   getFeatures() const override;
    bool useAppleRSNSupplicant(IO80211Interface *) override { return false; }
    UInt32   outputPacket(mbuf_t m, void *param) override;
    IOReturn getHardwareAddress(IOEthernetAddress *addr) override;
    IOReturn setHardwareAddress(const IOEthernetAddress *addr) override;
    IOReturn getHardwareAddressForInterface(IO80211Interface *iface,
                                            IOEthernetAddress *addr) override;
    IOReturn getPacketFilters(const OSSymbol *, UInt32 *) const override;
    IOReturn setMulticastMode(bool active) override;
    IOReturn setMulticastList(IOEthernetAddress *, UInt32 count) override;
    IOReturn setPromiscuousMode(bool active) override;

    /* IO80211Controller — the actual Airport-style API surface.
     * Selector names/signatures must match the IO80211Family SDK headers
     * (IO80211Controller.h) exactly — check MacKernelSDK version pinned
     * in the repo, this list is representative, not exhaustive. */
    /* AirPort_RTW88 intentionally targets the Ventura IO80211FamilyLegacy ABI.
     * On Sonoma/Sequoia/Tahoe the supported configuration restores that same
     * family (plus a matching IOSkywalkFamily), so these entry points must not
     * change behavior based on the host Darwin major. */
    virtual SInt32 apple80211_ioctl(IO80211Interface *interface,
                                    IO80211VirtualInterface *virtualInterface,
                                    ifnet_t net, unsigned long cmd,
                                    void *data) override;

    virtual SInt32 apple80211Request(unsigned int request_type,
                                      int request_number,
                                      IO80211Interface *interface,
                                      void *data) override;

#if __IO80211_TARGET >= __MAC_10_15
    /* Sonoma+ with the restored Ventura Legacy/Skywalk stack sends the join
     * through this split SET ingress on the tested machine.  Keep scan and
     * discovery on the proven BSD transport; this hook only recovers a lost
     * APPLE80211_IOC_ASSOCIATE request and otherwise delegates unchanged. */
    virtual SInt32 apple80211_ioctl_set(IO80211Interface *interface,
                                        IO80211VirtualInterface *virtualInterface,
                                        IO80211SkywalkInterface *skywalkInterface,
                                        void *data) override;
    virtual SInt32 apple80211_ioctl_set(IO80211SkywalkInterface *skywalkInterface,
                                        void *data) override;
#endif

    /* Association/security dispatch is owned by IO80211FamilyLegacy,
     * matching AirportItlwm Ventura.  Do not override the family's internal split/Skywalk helpers here. */

    /* AWDL / P2P virtual interface ABI (Ventura IO80211FamilyLegacy). */
    virtual SInt32 apple80211VirtualRequest(UInt request_type, int request_number,
                                            IO80211VirtualInterface *interface,
                                            void *data) override;
    virtual IO80211VirtualInterface *createVirtualInterface(ether_addr *addr, UInt role) override;
    virtual SInt32 enableVirtualInterface(IO80211VirtualInterface *interface) override;
    virtual SInt32 disableVirtualInterface(IO80211VirtualInterface *interface) override;
    virtual int outputActionFrame(IO80211Interface *interface, mbuf_t m) override;
    virtual int bpfOutputPacket(OSObject *object, UInt dltType, mbuf_t m) override;
    virtual void requestPacketTx(void *object, UInt options) override;

    /* Puros virtuales de IO80211Controller que hay que implementar si o si
     * para que la clase no quede abstracta (encontrados por el compilador,
     * no los tenia contemplados en el primer intento). */
    virtual SInt32 stopDMA() override;
    virtual UInt32 hardwareOutputQueueDepth(IO80211Interface*) override;
    virtual SInt32 performCountryCodeOperation(IO80211Interface*, IO80211CountryCodeOp) override;
    virtual SInt32 enableFeature(IO80211FeatureCode, void*) override;
    virtual SInt32 monitorModeSetEnabled(IO80211Interface*, bool, UInt32) override;

    /* RTW88EventDelegate — async notifications from RTW88IEEE80211 */
    virtual void rtw88Event(RTW88Event ev, void *data) override;

    /* RTW88RxDelegate — antes RTW88IEEE80211 asumia un RTW88PCIDevice*
     * para esto; ahora cualquiera de los dos kexts puede implementarlo. */
    virtual mbuf_t allocateInputPacket(uint32_t len) override;
    virtual void injectRxFrame(mbuf_t m) override;
    virtual void injectRxActionFrame(const uint8_t *frame, uint32_t len, int8_t rssi, uint16_t channel) override;
    virtual void injectRxAWDLFrame(mbuf_t m) override;
    virtual IOWorkLoop *getRxWorkLoop() override;
    /* IONetworkController link-state contract used by AirportItlwm.  Defaults
     * are intentionally omitted here so the one-argument RTW88RxDelegate
     * overload remains unambiguous. */
    virtual bool setLinkStatus(UInt32 status, const IONetworkMedium *activeMedium,
                               UInt64 speed, OSData *data) override;
    virtual void setLinkStatus(UInt32 status) override;

private:
    /* IO80211 payload adapters. Protected STA authentication is delegated to Apple RSN. */
    __attribute__((__noinline__)) SInt32 handleNativeRequest(unsigned int request_type, int request_number, IO80211Interface *interface, void *data);
    __attribute__((__noinline__)) IOReturn handleRequestCOUNTRY_CODE(bool isSet, void *data);
    __attribute__((__noinline__)) IOReturn handleRequestPHY_MODE(bool isSet, void *data);
    __attribute__((__noinline__)) IOReturn handleRequestRATE(bool isSet, void *data);
    __attribute__((__noinline__)) IOReturn handleRequestRATE_SET(bool isSet, void *data);
    __attribute__((__noinline__)) IOReturn handleRequestCHANNELS_INFO(bool isSet, void *data);
    __attribute__((__noinline__)) IOReturn handleRequestHW_SUPPORTED_CHANNELS(bool isSet, void *data);
    IOReturn handleSSID(bool set, struct apple80211_ssid_data *data);
    IOReturn handleAUTH_TYPE(bool set, struct apple80211_authtype_data *data);
    IOReturn handleASSOCIATE(struct apple80211_assoc_data *data);
    IOReturn handleRSN_IE(bool set, struct apple80211_rsn_ie_data *data);
    IOReturn handleAP_IE_LIST(struct apple80211_ap_ie_data *data);
    IOReturn handleCIPHER_KEY(struct apple80211_key *key);
    IOReturn handleDISASSOCIATE();
    IOReturn handleSCAN_REQ(void *data);
    IOReturn handleSCAN_RESULT(struct apple80211_scan_result **data);
    IOReturn handleCURRENT_NETWORK(struct apple80211_scan_result *data);
    void fillScanResultFromBSS(const RTW88BSS &b, struct apple80211_scan_result *data, bool fullIEs);
    IOReturn handleSTATE(struct apple80211_state_data *data);
    IOReturn handleCHANNEL(struct apple80211_channel_data *data);
    IOReturn handlePOWER(bool set, struct apple80211_power_data *data);
    IOReturn handleVIRTUAL_IF_CREATE(struct apple80211_virt_if_create_data *data);
    IOReturn handleVIRTUAL_IF_DELETE(struct apple80211_virt_if_delete_data *data);
    SInt32 handleAWDLVirtualRequest(UInt request_type, int request_number,
                                    IO80211VirtualInterface *interface, void *data);
    bool ensureAWDLVirtualInterface();

    apple80211_scan_result _scanResult = {};
    uint32_t _scanCursor = 0;
    uint32_t _authLower = APPLE80211_AUTHTYPE_OPEN;
    uint32_t _authUpper = APPLE80211_AUTHTYPE_NONE;
    uint8_t  _countryCode[APPLE80211_MAX_CC_LEN] = {'Z', 'Z', 0};
    RTW88RoamProfileBandData _roamProfile = {};
    bool _roamProfileValid = false;

    /* 1.0.1: AWDL/P2P state is isolated from STA state. IO80211 still owns
     * virtual-interface lifetime; the manager only observes those pointers. */
    RTW88AWDLManager       *_awdlManager = nullptr;

    bool _shutdown = false;
    bool _dmaStopped = false;
    volatile bool _txStalled = false;

    /* 2.0.0: explicit IOKit power-management state.  A hibernation resume
     * restores kernel memory but the PCI function/firmware may be completely
     * cold, so never infer hardware state from restored C++ objects. */
    volatile bool _pmTransition = false;
    unsigned long _pmPowerState = 1;
    bool _pmRadioWasPowered = false;
    bool _pmAWDLSyncWasEnabled = true;
    uint32_t _pmSleepCount = 0;
    uint32_t _pmWakeCount = 0;
    IOReturn _pmLastSleepResult = kIOReturnSuccess;
    IOReturn _pmLastWakeResult = kIOReturnSuccess;
    IOPCIDevice           *_pciDev      = nullptr;
    AirportRTW88Interface  *_netif       = nullptr;
    RTW88IEEE80211         *_ieee80211   = nullptr;  /* reused as-is from rtw88 core */

    IO80211WorkLoop       *_workLoop    = nullptr;

    IOEthernetAddress       _macAddr;
    bool                    _scanInProgress = false;
    UInt32                  _powerSaveLevel = APPLE80211_POWERSAVE_MODE_DISABLED;
    volatile UInt32         _nativeAssocDispatchCount = 0;
    uint32_t                _awdlPeerPresencePosts = 0;
    uint32_t                _awdlPeerAbsencePosts = 0;
    uint32_t                _awdlPeerIPv6Posts = 0;
    /* Sonoma + OCLP legacy IO80211 can service CoreWiFi scans entirely from
     * the family cache and issue only GET SCAN_RESULT calls.  Remember whether
     * we already performed the one demand-driven bootstrap scan for an empty
     * cache so an RF-empty environment cannot create a tight rescan loop. */
    bool                    _scanCacheBootstrapAttempted = false;
    /* Incremented synchronously by apple80211Request().  apple80211_ioctl()
     * uses it to detect whether IO80211FamilyLegacy already dispatched a BSD
     * request before considering the compatibility fallback.  This is based
     * on observed transport behavior, never on the host Darwin version. */
    bool                    _linkUp = false;
    bool                    _assocDoneReported = false;
    bool                    _rsnHandshakeReported = false;

    /* Acceso real a hardware -- antes esto se pasaba como nullptr a
     * RTW88IEEE80211::create(), causando "pci bus timeout" en cada
     * acceso a registro. Replica lo que ya hace RTW88PCIDevice. */
    IOMemoryMap             *_mmioMap      = nullptr;
    volatile void           *_mmioBase     = nullptr;
    IOInterruptEventSource  *_intrSrc      = nullptr;
    struct pci_dev          *_compatPciDev = nullptr;

    bool setupInterrupt();
    bool failStart(IOService *provider, const char *reason);
    void teardown();
    void releaseDMAEntries();
    void handleInterrupt(IOInterruptEventSource *src, int count);

    /* RTW88HwOps -- acceso real a hardware, antes simulado con nullptr */
    UInt8  pciReadByte(int offset) override;
    UInt16 pciReadWord(int offset) override;
    UInt32 pciReadDword(int offset) override;
    void   pciWriteByte(int offset, UInt8 val) override;
    void   pciWriteWord(int offset, UInt16 val) override;
    void   pciWriteDword(int offset, UInt32 val) override;
    int    pciFindCapability(int cap) override;
    volatile void *mmioBase() const override { return _mmioBase; }
    void  *allocCoherent(size_t size, IOPhysicalAddress *phys) override;
    void   freeCoherent(size_t size, void *virt, IOPhysicalAddress phys) override;
    void   freeCoherentByPhys(IOPhysicalAddress phys) override;
    void   setBounceOrigVA(IOPhysicalAddress phys, void *orig_va) override;
    void   syncBounceForCpu(IOPhysicalAddress dma, size_t size) override;
    void   resumeTxIfStalled() override;
    void   drainPendingFree();
    IOReturn quiesceForSystemSleep();
    IOReturn restoreAfterSystemWake();

    RTW88DMAEntry *_dmaList        = nullptr;
    IOSimpleLock  *_dmaLock        = nullptr;
    RTW88DMAEntry *_dmaPendingFree = nullptr;
    IOSimpleLock  *_pendingFreeLock = nullptr;
    bool _superStarted      = false;
    bool _compatInitialized = false;
    bool _ieeeStarted       = false;
    bool _netifAttached     = false;
};
