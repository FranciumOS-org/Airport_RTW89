// SPDX-License-Identifier: GPL-2.0
#include "AirPortRTW89.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSAtomic.h>
#include <sys/kpi_mbuf.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(AirPort_RTW89, IOEthernetController)

#define LOG(fmt, ...) IOLog("AirPort_RTW89: " fmt "\n", ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Platform services for the Linux side                                */
/* ------------------------------------------------------------------ */

uint32_t AirPort_RTW89::cfgRead(void *ctx, unsigned int offset, unsigned int width)
{
    IOPCIDevice *pci = static_cast<AirPort_RTW89 *>(ctx)->_pci;

    /* The extended accessors reach all 4 KB of PCIe config space; the driver
     * uses registers above 0xff (ASPM/L1 controls at 0x7xx). */
    switch (width) {
    case 1:  return pci->extendedConfigRead8(offset);
    case 2:  return pci->extendedConfigRead16(offset);
    default: return pci->extendedConfigRead32(offset);
    }
}

void AirPort_RTW89::cfgWrite(void *ctx, unsigned int offset, unsigned int width,
                             uint32_t value)
{
    IOPCIDevice *pci = static_cast<AirPort_RTW89 *>(ctx)->_pci;

    switch (width) {
    case 1:  pci->extendedConfigWrite8(offset, (UInt8)value); break;
    case 2:  pci->extendedConfigWrite16(offset, (UInt16)value); break;
    default: pci->extendedConfigWrite32(offset, value); break;
    }
}

/*
 * Wired, physically contiguous, below 4 GB. The physical address is used as
 * the bus address, which is right as long as no IOMMU remaps this device: the
 * case on this machine (macOS has no AMD-Vi support) and on Intel hackintoshes
 * run with DisableIoMapper.
 */
void *AirPort_RTW89::dmaAlloc(void *ctx, size_t size, uint64_t *busAddr, void **cookie)
{
    IOBufferMemoryDescriptor *desc;
    IOByteCount segLen = 0;
    addr64_t phys;

    desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        size, 0x00000000FFFFF000ULL);
    if (!desc)
        return nullptr;

    if (desc->prepare() != kIOReturnSuccess) {
        desc->release();
        return nullptr;
    }

    phys = desc->getPhysicalSegment(0, &segLen);
    if (!phys || segLen < size) {
        desc->complete();
        desc->release();
        return nullptr;
    }

    *busAddr = phys;
    *cookie = desc;
    return desc->getBytesNoCopy();
}

void AirPort_RTW89::dmaFree(void *ctx, void *cookie)
{
    IOBufferMemoryDescriptor *desc = static_cast<IOBufferMemoryDescriptor *>(cookie);

    desc->complete();
    desc->release();
}

/* ------------------------------------------------------------------ */
/*  Interrupt                                                           */
/* ------------------------------------------------------------------ */

bool AirPort_RTW89::setupInterrupt()
{
    int index = 0, msiIndex = -1, type = 0;

    /* Prefer MSI: the legacy line is often not routed on these boards. */
    while (_pci->getInterruptType(index, &type) == kIOReturnSuccess) {
        if (type & kIOInterruptTypePCIMessaged) {
            msiIndex = index;
            break;
        }
        index++;
    }

    _workLoop = IOWorkLoop::workLoop();
    if (!_workLoop)
        return false;

    /* The action runs on the work loop thread, not at interrupt level, which
     * is what the driver's handler needs (it takes sleeping locks). */
    _interrupt = IOInterruptEventSource::interruptEventSource(
        this,
        OSMemberFunctionCast(IOInterruptEventSource::Action, this,
                             &AirPort_RTW89::interruptOccurred),
        _pci, msiIndex >= 0 ? msiIndex : 0);
    if (!_interrupt)
        return false;

    if (_workLoop->addEventSource(_interrupt) != kIOReturnSuccess)
        return false;

    LOG("using %s interrupt (index %d)", msiIndex >= 0 ? "MSI" : "legacy",
        msiIndex >= 0 ? msiIndex : 0);
    return true;
}

void AirPort_RTW89::interruptOccurred(IOInterruptEventSource *source, int count)
{
    rtw89_glue_interrupt();
}

void AirPort_RTW89::irqEnable(void *ctx, bool enable)
{
    IOInterruptEventSource *source = static_cast<AirPort_RTW89 *>(ctx)->_interrupt;

    if (!source)
        return;
    if (enable)
        source->enable();
    else
        source->disable();
}

/* ------------------------------------------------------------------ */
/*  Network interface                                                   */
/* ------------------------------------------------------------------ */

bool AirPort_RTW89::createWorkLoop()
{
    if (!_netWorkLoop)
        _netWorkLoop = IOWorkLoop::workLoop();
    return _netWorkLoop != nullptr;
}

IOWorkLoop *AirPort_RTW89::getWorkLoop() const
{
    return _netWorkLoop;
}

const OSString *AirPort_RTW89::newVendorString() const
{
    return OSString::withCString("Realtek");
}

const OSString *AirPort_RTW89::newModelString() const
{
    return OSString::withCString("RTL8852BE");
}

IOReturn AirPort_RTW89::getHardwareAddress(IOEthernetAddress *addr)
{
    *addr = _mac;
    return kIOReturnSuccess;
}

/* The chip passes unicast for us, broadcast and the multicast of the network
 * it has joined; there is nothing to program per group address. */
IOReturn AirPort_RTW89::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    if (group == gIONetworkFilterGroup) {
        *filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast |
                   kIOPacketFilterMulticast | kIOPacketFilterMulticastAll;
        return kIOReturnSuccess;
    }
    return super::getPacketFilters(group, filters);
}

IOReturn AirPort_RTW89::setPromiscuousMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn AirPort_RTW89::setMulticastMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn AirPort_RTW89::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
    return kIOReturnSuccess;
}

bool AirPort_RTW89::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *data;

    if (!super::configureInterface(netif))
        return false;

    /* The stack keeps the output queue (with its own scheduling and TCP
     * back-pressure) and outputStart() pulls from it as the driver has room. */
    if (netif->configureOutputPullModel(256, 0, 0,
            IONetworkInterface::kOutputPacketSchedulingModelNormal) != kIOReturnSuccess) {
        LOG("could not set up the output queue");
        return false;
    }

    data = netif->getParameter(kIONetworkStatsKey);
    if (!data)
        return false;
    _netStats = const_cast<IONetworkStats *>(static_cast<const IONetworkStats *>(data->getBuffer()));
    if (!_netStats)
        return false;
    return true;
}

/*
 * The stack brings the interface up or down. The radio is not tied to that:
 * it is started and a network joined with rtw89ctl, and the link state tells
 * the stack when there is somewhere to send to.
 */
IOReturn AirPort_RTW89::enable(IONetworkInterface *netif)
{
    _netifEnabled = true;
    netif->startOutputThread();
    return kIOReturnSuccess;
}

IOReturn AirPort_RTW89::disable(IONetworkInterface *netif)
{
    _netifEnabled = false;
    /* returns once no thread is inside outputStart() */
    netif->stopOutputThread();
    netif->flushOutputQueue();
    return kIOReturnSuccess;
}

/* One Ethernet frame from the stack: copied into the driver's own buffer and
 * the mbuf freed. False if the frame was dropped. */
bool AirPort_RTW89::transmit(mbuf_t m)
{
    size_t len = mbuf_pkthdr_len(m);
    uint8_t *frame = nullptr;
    void *handle;
    int ret = -1;

    if (_dataReady && (handle = rtw89_glue_tx_alloc(len, &frame))) {
        if (mbuf_copydata(m, 0, len, frame) == 0) {
            ret = rtw89_glue_tx(handle);
        } else {
            rtw89_glue_tx_cancel(handle);
        }
    }

    freePacket(m);
    if (_netStats) {
        if (ret)
            _netStats->outputErrors++;
        else
            _netStats->outputPackets++;
    }
    return ret == 0;
}

/*
 * The stack's output thread: frames are waiting in the interface's queue. Take
 * as many as the driver has room for. kIOReturnNoResources leaves the rest
 * queued until txWake() says there is room again.
 */
IOReturn AirPort_RTW89::outputStart(IONetworkInterface *netif, IOOptionBits options)
{
    IOReturn result = kIOReturnSuccess;

    OSIncrementAtomic(&_txBusy);
    for (;;) {
        unsigned int room = _dataReady ? rtw89_glue_tx_room() : 16;
        mbuf_t m = nullptr;

        if (!room) {
            result = kIOReturnNoResources;
            break;
        }
        if (netif->dequeueOutputPackets(room < 16 ? room : 16, &m) != kIOReturnSuccess)
            break;                      /* the queue is empty */
        while (m) {
            mbuf_t next = mbuf_nextpkt(m);

            mbuf_setnextpkt(m, nullptr);
            transmit(m);
            m = next;
        }
    }
    OSDecrementAtomic(&_txBusy);
    return result;
}

/* Not used with the pull model; kept for callers of the old entry point. */
UInt32 AirPort_RTW89::outputPacket(mbuf_t m, void *param)
{
    bool sent;

    OSIncrementAtomic(&_txBusy);
    sent = transmit(m);
    OSDecrementAtomic(&_txBusy);
    return sent ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}

/* The driver has room for frames again. A driver thread. */
void AirPort_RTW89::txWake(void *ctx)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);

    if (me->_dataReady && me->_netifEnabled)
        me->_netif->signalOutputThread();
}

/* One received Ethernet frame, on the driver's receive thread. */
void AirPort_RTW89::rxFrame(void *ctx, const uint8_t *frame, size_t len)
{
    AirPort_RTW89 *me = static_cast<AirPort_RTW89 *>(ctx);
    mbuf_t m;

    if (!me->_dataReady || !me->_netifEnabled)
        return;

    m = me->allocatePacket((UInt32)len);
    if (!m) {
        me->_netStats->inputErrors++;
        return;
    }
    if (mbuf_copyback(m, 0, len, frame, MBUF_DONTWAIT) != 0) {
        me->freePacket(m);
        me->_netStats->inputErrors++;
        return;
    }
    me->_netif->inputPacket(m, (UInt32)len);
    me->_netStats->inputPackets++;
}

bool AirPort_RTW89::setupInterface()
{
    struct rtw89_glue_info info;

    if (!rtw89_glue_get_info(&info))
        return false;
    memcpy(_mac.bytes, info.mac, sizeof(_mac.bytes));

    /* One medium: the stack only needs to know whether the link is up. */
    _mediumDict = OSDictionary::withCapacity(1);
    _medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 0);
    if (!_mediumDict || !_medium)
        return false;
    IONetworkMedium::addMedium(_mediumDict, _medium);
    _medium->release();         /* the dictionary keeps it */
    if (!publishMediumDictionary(_mediumDict) || !setCurrentMedium(_medium))
        return false;
    setLinkStatus(kIONetworkLinkValid, _medium, 0);

    if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&_netif), false) || !_netif)
        return false;
    _dataReady = true;
    _netif->registerService();
    return true;
}

/* ------------------------------------------------------------------ */
/*  IOService                                                           */
/* ------------------------------------------------------------------ */

bool AirPort_RTW89::start(IOService *provider)
{
    struct rtw89_glue_platform platform = {};
    struct rtw89_glue_device device = {};
    int ret;

    if (!super::start(provider))
        return false;

    _commandLock = IOLockAlloc();
    if (!_commandLock)
        return false;

    _pci = OSDynamicCast(IOPCIDevice, provider);
    if (!_pci) {
        LOG("provider is not an IOPCIDevice");
        return false;
    }
    _pci->retain();

    device.vendor = _pci->configRead16(kIOPCIConfigVendorID);
    device.device = _pci->configRead16(kIOPCIConfigDeviceID);
    device.subsystem_vendor = _pci->configRead16(kIOPCIConfigSubSystemVendorID);
    device.subsystem_device = _pci->configRead16(kIOPCIConfigSubSystemID);
    device.revision = _pci->configRead8(kIOPCIConfigRevisionID);
    LOG("found %04x:%04x (subsystem %04x:%04x, rev %02x)", device.vendor, device.device,
        device.subsystem_vendor, device.subsystem_device, device.revision);

    if (!rtw89_glue_supports(device.vendor, device.device)) {
        LOG("not a device this driver handles");
        teardown();
        return false;
    }

    if (!_pci->open(this)) {
        LOG("could not open the PCI device (claimed by another driver?)");
        teardown();
        return false;
    }
    _pciOpen = true;

    /* A device nobody has driven yet may have been left in D3. */
    IOByteCount pmCap = 0;
    if (_pci->extendedFindPCICapability(kIOPCIPowerManagementCapability, &pmCap) && pmCap) {
        UInt16 pmcsr = _pci->extendedConfigRead16(pmCap + 4);
        if (pmcsr & 0x3) {
            LOG("device was in D%u, moving it to D0", pmcsr & 0x3);
            _pci->extendedConfigWrite16(pmCap + 4, pmcsr & ~0x3);
            IOSleep(20);
        }
    }

    _pci->setMemoryEnable(true);
    _pci->setBusMasterEnable(true);

    /* rtw89 keeps its registers in BAR 2. */
    _mmio = _pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!_mmio) {
        LOG("could not map BAR 2");
        teardown();
        return false;
    }
    device.mmio_base = reinterpret_cast<volatile void *>(_mmio->getVirtualAddress());
    device.mmio_len = _mmio->getLength();
    LOG("BAR 2 mapped, %llu bytes", (unsigned long long)device.mmio_len);

    if (!setupInterrupt()) {
        LOG("could not set up the interrupt");
        teardown();
        return false;
    }

    platform.ctx = this;
    platform.cfg_read = cfgRead;
    platform.cfg_write = cfgWrite;
    platform.dma_alloc = dmaAlloc;
    platform.dma_free = dmaFree;
    platform.irq_enable = irqEnable;
    platform.link_changed = linkChanged;
    platform.rx_frame = rxFrame;
    platform.tx_wake = txWake;

    LOG("probing");
    ret = rtw89_glue_probe(&platform, &device);
    if (ret) {
        LOG("probe failed: %d", ret);
        teardown();
        return false;
    }
    _probed = true;

    publishInfo();

    /* The interrupt source stays disabled: probe leaves the chip powered down.
     * The glue enables it (irqEnable) when the radio is brought up. */

    if (!setupInterface()) {
        LOG("could not create the network interface");
        teardown();
        return false;
    }

    registerService();
    return true;
}

void AirPort_RTW89::publishInfo()
{
    struct rtw89_glue_info info;
    char mac[18];

    if (!rtw89_glue_get_info(&info))
        return;

    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);

    LOG("RTL8852BE cut %c, RFE type %u, %uT%uR", 'A' + info.chip_cut, info.rfe_type,
        info.tx_streams, info.rx_streams);
    LOG("firmware %s (%08x)", info.fw_version, info.fw_commit);
    LOG("MAC address %s", mac);

    setProperty("RTW89 MAC Address", mac);
    setProperty("RTW89 Firmware Version", info.fw_version);
}

/* ------------------------------------------------------------------ */
/*  Commands from user space                                            */
/* ------------------------------------------------------------------ */

void AirPort_RTW89::publishScanResults()
{
    static const unsigned int kMax = 128;
    struct rtw89_glue_bss *list;
    unsigned int n;
    OSArray *array;

    list = static_cast<struct rtw89_glue_bss *>(IOMalloc(kMax * sizeof(*list)));
    if (!list)
        return;
    n = rtw89_glue_scan_results(list, kMax);

    array = OSArray::withCapacity(n ? n : 1);
    for (unsigned int i = 0; array && i < n; i++) {
        OSDictionary *dict = OSDictionary::withCapacity(6);
        char bssid[18];
        OSObject *value;

        if (!dict)
            break;
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 list[i].bssid[0], list[i].bssid[1], list[i].bssid[2],
                 list[i].bssid[3], list[i].bssid[4], list[i].bssid[5]);

        /* SSIDs are arbitrary bytes, so they travel as data, not as a string. */
        if ((value = OSData::withBytes(list[i].ssid, list[i].ssid_len))) {
            dict->setObject("ssid", value);
            value->release();
        }
        if ((value = OSString::withCString(bssid))) {
            dict->setObject("bssid", value);
            value->release();
        }
        if ((value = OSNumber::withNumber(list[i].channel, 32))) {
            dict->setObject("channel", value);
            value->release();
        }
        if ((value = OSNumber::withNumber(list[i].freq, 32))) {
            dict->setObject("freq", value);
            value->release();
        }
        if ((value = OSNumber::withNumber((unsigned long long)(long long)list[i].signal, 32))) {
            dict->setObject("rssi", value);
            value->release();
        }
        if ((value = OSNumber::withNumber(list[i].seen, 32))) {
            dict->setObject("seen", value);
            value->release();
        }
        array->setObject(dict);
        dict->release();
    }

    if (array) {
        setProperty("RTW89 Scan Results", array);
        array->release();
    }
    setProperty("RTW89 Scanning", rtw89_glue_scanning());
    setProperty("RTW89 Radio Up", rtw89_glue_is_up());
    IOFree(list, kMax * sizeof(*list));

    publishLink();
}

/* Called from the driver's own thread on every link state change, with driver
 * locks held: only publish, never take _commandLock or call back in. */
void AirPort_RTW89::linkChanged(void *ctx)
{
    static_cast<AirPort_RTW89 *>(ctx)->publishLink();
}

void AirPort_RTW89::publishLink()
{
    static const char *const kStates[] = { "down", "joining", "associated", "connected" };
    struct rtw89_glue_link link;
    char bssid[18];

    rtw89_glue_link(&link);
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", link.bssid[0],
             link.bssid[1], link.bssid[2], link.bssid[3], link.bssid[4], link.bssid[5]);
    setProperty("RTW89 Link State", kStates[link.state]);
    setProperty("RTW89 Link BSSID", bssid);
    setProperty("RTW89 Link Frequency", link.freq, 32);
    setProperty("RTW89 Link AID", link.aid, 32);
    setProperty("RTW89 Link EAPOL Frames", link.eapol_rx, 32);
    setProperty("RTW89 Link Error", (unsigned long long)(long long)link.last_error, 32);
    setProperty("RTW89 TX Frames", link.tx_frames, 32);
    setProperty("RTW89 TX Dropped", link.tx_dropped, 32);
    setProperty("RTW89 RX Frames", link.rx_frames, 32);
    setProperty("RTW89 RX Dropped", link.rx_dropped, 32);
    setProperty("RTW89 RX Undecrypted", link.rx_undecrypted, 32);
    setProperty("RTW89 RX Replayed", link.rx_replay, 32);
    setProperty("RTW89 RX Duplicates", link.rx_dup, 32);
    setProperty("RTW89 RX Reorder Timeouts", link.rx_reorder_timeout, 32);
    setProperty("RTW89 RX Late", link.rx_late, 32);
    setProperty("RTW89 RX Late With Interrupt", link.rx_late_irq, 32);
    setProperty("RTW89 RX Late Max", link.rx_late_max_ms, 32);
    setProperty("RTW89 RX Status Flushed", link.rx_ppdu_flushed, 32);
    setProperty("RTW89 RX Status Flush", link.ppdu_flush);
    setProperty("RTW89 TX Aggregation", link.tx_ba, 32);
    setProperty("RTW89 RX Aggregation", link.rx_ba, 32);

    /* The stack starts DHCP when the link comes up and forgets its addresses
     * when it goes down. */
    bool active = link.state == RTW89_GLUE_LINK_CONNECTED;
    if (_medium && active != _linkActive) {
        _linkActive = active;
        setLinkStatus(active ? (kIONetworkLinkValid | kIONetworkLinkActive) : kIONetworkLinkValid,
                      _medium, active ? 100 * 1000000ULL : 0);
        LOG("link %s", active ? "up" : "down");
    }
}

IOReturn AirPort_RTW89::setProperties(OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    OSString *command = dict ? OSDynamicCast(OSString, dict->getObject("RTW89Command")) : nullptr;
    IOReturn result = kIOReturnSuccess;
    int ret = 0;

    if (!command)
        return kIOReturnBadArgument;
    /* anyone may ask for the published state to be refreshed */
    if (!command->isEqualTo("results") &&
        IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) !=
        kIOReturnSuccess)
        return kIOReturnNotPrivileged;

    IOLockLock(_commandLock);
    if (!_probed) {
        result = kIOReturnNotReady;
    } else if (command->isEqualTo("up")) {
        LOG("radio up");
        ret = rtw89_glue_up();
    } else if (command->isEqualTo("down")) {
        LOG("radio down");
        rtw89_glue_down();
    } else if (command->isEqualTo("scan")) {
        ret = rtw89_glue_up();
        if (!ret)
            ret = rtw89_glue_scan();
    } else if (command->isEqualTo("join")) {
        OSData *ssid = OSDynamicCast(OSData, dict->getObject("RTW89SSID"));

        OSData *pass = OSDynamicCast(OSData, dict->getObject("RTW89Passphrase"));

        if (!ssid || !ssid->getLength()) {
            result = kIOReturnBadArgument;
        } else {
            LOG("join requested");
            ret = rtw89_glue_join(static_cast<const uint8_t *>(ssid->getBytesNoCopy()),
                                  ssid->getLength(),
                                  pass ? static_cast<const char *>(pass->getBytesNoCopy()) : nullptr,
                                  pass ? pass->getLength() : 0);
        }
    } else if (command->isEqualTo("flush-on") || command->isEqualTo("flush-off")) {
        rtw89_glue_set_ppdu_flush(command->isEqualTo("flush-on"));
    } else if (command->isEqualTo("leave")) {
        rtw89_glue_leave();
    } else if (command->isEqualTo("results")) {
        /* only refreshes the properties below */
    } else {
        result = kIOReturnBadArgument;
    }

    if (ret) {
        LOG("command \"%s\" failed: %d", command->getCStringNoCopy(), ret);
        setProperty("RTW89 Last Error", (unsigned long long)(long long)ret, 32);
        result = kIOReturnError;
    }
    if (_probed)
        publishScanResults();
    IOLockUnlock(_commandLock);

    return result;
}

void AirPort_RTW89::teardown()
{
    /* No new frames into the driver, and wait for the ones on their way. */
    _dataReady = false;
    while (_txBusy)
        IOSleep(1);

    if (_commandLock)
        IOLockLock(_commandLock);

    if (_probed) {
        _probed = false;
        /* Brings the radio down first if it is up. */
        rtw89_glue_remove();
        /* The compat workqueue threads signal that they are done just before
         * they exit; let them get out of this kext's code before it can be
         * unloaded. */
        IOSleep(200);
    }

    if (_interrupt)
        _interrupt->disable();

    if (_interrupt) {
        if (_workLoop)
            _workLoop->removeEventSource(_interrupt);
        _interrupt->release();
        _interrupt = nullptr;
    }
    if (_workLoop) {
        _workLoop->release();
        _workLoop = nullptr;
    }
    if (_mmio) {
        _mmio->release();
        _mmio = nullptr;
    }
    if (_pci) {
        if (_pciOpen) {
            _pci->setBusMasterEnable(false);
            _pci->close(this);
            _pciOpen = false;
        }
        _pci->release();
        _pci = nullptr;
    }

    if (_commandLock)
        IOLockUnlock(_commandLock);

    /* The driver is gone, so nothing calls rxFrame() any more. */
    if (_netif) {
        detachInterface(_netif, true);
        _netif->release();
        _netif = nullptr;
    }
    _linkActive = false;
}

void AirPort_RTW89::stop(IOService *provider)
{
    LOG("stopping");
    teardown();
    super::stop(provider);
}

void AirPort_RTW89::free()
{
    teardown();
    if (_commandLock) {
        IOLockFree(_commandLock);
        _commandLock = nullptr;
    }
    if (_mediumDict) {
        _mediumDict->release();
        _mediumDict = nullptr;
        _medium = nullptr;
    }
    if (_netWorkLoop) {
        _netWorkLoop->release();
        _netWorkLoop = nullptr;
    }
    super::free();
}
