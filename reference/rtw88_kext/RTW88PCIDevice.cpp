// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// RTW88PCIDevice.cpp — IOEthernetController for PCIe rtw88 adapters

#include "RTW88PCIDevice.hpp"
#include "RTW88IEEE80211.hpp"
#include "RTW88UserClient.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/network/IONetworkMedium.h>

/* The Linux-compat pci_ops that route through this class */
extern "C" {
#include "../compat/rtw88_compat.h"
}

extern "C" void rtw88_trigger_interrupt(void);
extern "C" boolean_t preemption_enabled(void);

#define super IOEthernetController
OSDefineMetaClassAndStructors(RTW88PCIDevice, IOEthernetController)

static constexpr unsigned int kRTW88TxStallAvail = 96;
static constexpr unsigned int kRTW88TxResumeAvail = 160;

/* ------------------------------------------------------------------ */
/*  PCI ops shim (C linkage, called from driver C code)                */
/* ------------------------------------------------------------------ */

RTW88HwOps *g_pci_dev_instance = nullptr;  /* ya no static: AirportRTW88 tambien lo setea */

static int compat_pci_read_config_byte(struct pci_dev *dev, int where, u8 *val)
{
    if (!g_pci_dev_instance) { *val = 0xff; return -1; }
    *val = g_pci_dev_instance->pciReadByte(where);
    return 0;
}
static int compat_pci_read_config_word(struct pci_dev *dev, int where, u16 *val)
{
    if (!g_pci_dev_instance) { *val = 0xffff; return -1; }
    *val = g_pci_dev_instance->pciReadWord(where);
    return 0;
}
static int compat_pci_read_config_dword(struct pci_dev *dev, int where, u32 *val)
{
    if (!g_pci_dev_instance) { *val = 0xffffffff; return -1; }
    *val = g_pci_dev_instance->pciReadDword(where);
    return 0;
}
static int compat_pci_write_config_byte(struct pci_dev *dev, int where, u8 val)
{
    if (!g_pci_dev_instance) return -1;
    g_pci_dev_instance->pciWriteByte(where, val);
    return 0;
}
static int compat_pci_write_config_word(struct pci_dev *dev, int where, u16 val)
{
    if (!g_pci_dev_instance) return -1;
    g_pci_dev_instance->pciWriteWord(where, val);
    return 0;
}
static int compat_pci_write_config_dword(struct pci_dev *dev, int where, u32 val)
{
    if (!g_pci_dev_instance) return -1;
    g_pci_dev_instance->pciWriteDword(where, val);
    return 0;
}
static void *compat_ioremap(struct pci_dev *dev, int bar, size_t len)
{
    /* Already mapped at start; just return the cached base */
    return (void *)g_pci_dev_instance->mmioBase();
}
static void compat_iounmap(struct pci_dev *dev, void *addr) {}

static int compat_enable_msi(struct pci_dev *dev)
{
    /* Handled by IOInterruptEventSource in setupInterrupt() */
    return 0;
}
static void compat_disable_msi(struct pci_dev *dev) {}

static int compat_pci_find_capability(struct pci_dev *dev, int cap)
{
    if (!g_pci_dev_instance) return 0;
    return g_pci_dev_instance->pciFindCapability(cap);
}

struct pci_ops_rtw88 _pci_io_ops = {
    .read_config_byte   = compat_pci_read_config_byte,
    .read_config_word   = compat_pci_read_config_word,
    .read_config_dword  = compat_pci_read_config_dword,
    .write_config_byte  = compat_pci_write_config_byte,
    .write_config_word  = compat_pci_write_config_word,
    .write_config_dword = compat_pci_write_config_dword,
    .ioremap            = compat_ioremap,
    .iounmap            = compat_iounmap,
    .enable_msi         = compat_enable_msi,
    .disable_msi        = compat_disable_msi,
    .pci_find_capability = compat_pci_find_capability,
};



/* DMA ops shim */
static void *compat_dma_alloc(struct device *dev, size_t size,
                               dma_addr_t *dma_handle, gfp_t flag)
{
    if (!g_pci_dev_instance) return nullptr;
    IOPhysicalAddress phys = 0;
    void *virt = g_pci_dev_instance->allocCoherent(size, &phys);
    if (dma_handle) *dma_handle = (dma_addr_t)phys;
    return virt;
}
static void compat_dma_free(struct device *dev, size_t size,
                             void *cpu_addr, dma_addr_t dma_handle)
{
    if (g_pci_dev_instance)
        g_pci_dev_instance->freeCoherent(size, cpu_addr,
                                          (IOPhysicalAddress)dma_handle);
}
static dma_addr_t compat_dma_map(struct device *dev, void *ptr,
                                   size_t size, int dir)
{
    /*
     * Always bounce: allocate a physically-contiguous <4GB buffer, record the
     * original CPU VA in orig_va, and hand the chip the bounce PA.
     *
     * DMA_TO_DEVICE (1): copy ptr→bounce now; chip reads from bounce.
     * DMA_FROM_DEVICE (0): don't copy now; chip writes to bounce.
     *   dma_sync_single_for_cpu() will copy bounce→ptr after chip is done.
     *
     * This is the standard software bounce-buffer strategy.  It costs a memcpy
     * per RX packet but is always correct regardless of where IOMalloc places
     * the skb data pages.
     */
    if (!g_pci_dev_instance || !ptr) return 0;

    IOPhysicalAddress phys = 0;
    void *bounce = g_pci_dev_instance->allocCoherent(size, &phys);
    if (!bounce) return 0;

    /* Store the original VA so sync_for_cpu can copy the received data back */
    g_pci_dev_instance->setBounceOrigVA((IOPhysicalAddress)phys, ptr);

    /* TX: feed the chip data now */
    if (dir == 1 /* DMA_TO_DEVICE */ || dir == 2 /* DMA_BIDIRECTIONAL */)
        memcpy(bounce, ptr, size);

    return (dma_addr_t)phys;
}
static void compat_dma_unmap(struct device *dev, dma_addr_t addr,
                               size_t size, int dir)
{
    /* Free the bounce buffer; freeCoherentByPhys is a no-op if not found */
    if (g_pci_dev_instance)
        g_pci_dev_instance->freeCoherentByPhys((IOPhysicalAddress)addr);
}
static void compat_dma_sync_cpu(struct device *dev, dma_addr_t addr,
                                  size_t size, int dir)
{
    /*
     * RX: chip finished writing to the bounce buffer.  Copy it into the
     * original skb->data buffer so the driver can read it from there.
     */
    if (dir == 0 /* DMA_FROM_DEVICE */ && g_pci_dev_instance)
        g_pci_dev_instance->syncBounceForCpu((IOPhysicalAddress)addr, size);
}
static void compat_dma_sync_dev(struct device *dev, dma_addr_t addr,
                                  size_t size, int dir)
{
    /* Re-arm for next DMA: bounce is already in place, nothing to do */
}

struct rtw88_dma_alloc_ops _dma_ops = {
    .alloc_coherent         = compat_dma_alloc,
    .free_coherent          = compat_dma_free,
    .map_single             = compat_dma_map,
    .unmap_single           = compat_dma_unmap,
    .sync_single_for_cpu    = compat_dma_sync_cpu,
    .sync_single_for_device = compat_dma_sync_dev,
};

const char *RTW88PCIDevice::chipDisplayName() const
{
    if (!_compatPciDev)
        return "Realtek Wireless";

    switch (_compatPciDev->device) {
    case 0xB822:
        return "RTL8822BE";
    case 0xC822:
    case 0xC82F:
        return "RTL8822CE";
    case 0xC821:
    case 0xB821:
        return "RTL8821CE";
    default:
        return "Realtek Wireless";
    }
}

const OSString *RTW88PCIDevice::newVendorString() const
{
    return OSString::withCString("Realtek");
}

const OSString *RTW88PCIDevice::newModelString() const
{
    return OSString::withCString(chipDisplayName());
}

void RTW88PCIDevice::publishHardwareIdentity()
{
    const char *chip = chipDisplayName();

    if (_ieee80211)
        _ieee80211->getMACAddress(_macAddr.bytes);

    setName(chip);
    setProperty(kIOVendor, "Realtek");
    setProperty(kIOModel, chip);
    setProperty(kIORevision, "rtw88");
    setProperty(kIOBuiltin, kOSBooleanTrue);
    setProperty(kIOLocation, "Internal");
    setProperty("IOUserVisibleName", chip);
    setProperty("Product Name", chip);
    setProperty("Device Name", chip);
    setProperty("Model", chip);
    setProperty("Vendor", "Realtek");

    if (_macAddr.bytes[0] || _macAddr.bytes[1] || _macAddr.bytes[2] ||
        _macAddr.bytes[3] || _macAddr.bytes[4] || _macAddr.bytes[5]) {
        OSData *mac = OSData::withBytes(_macAddr.bytes, sizeof(_macAddr.bytes));
        if (mac) {
            setProperty(kIOMACAddress, mac);
            mac->release();
        }
    }

    if (_iface) {
        _iface->setName(chip);
        _iface->setProperty(kIOVendor, "Realtek");
        _iface->setProperty(kIOModel, chip);
        _iface->setProperty(kIORevision, "rtw88");
        _iface->setProperty(kIOBuiltin, kOSBooleanTrue);
        _iface->setProperty(kIOPrimaryInterface, kOSBooleanTrue);
        _iface->setProperty(kIOLocation, "Internal");
        _iface->setProperty("IOUserVisibleName", chip);
        _iface->setProperty("Product Name", chip);
        _iface->setProperty("Device Name", chip);
        _iface->setProperty("Model", chip);
        _iface->setProperty("Vendor", "Realtek");

        if (_macAddr.bytes[0] || _macAddr.bytes[1] || _macAddr.bytes[2] ||
            _macAddr.bytes[3] || _macAddr.bytes[4] || _macAddr.bytes[5]) {
            OSData *mac = OSData::withBytes(_macAddr.bytes, sizeof(_macAddr.bytes));
            if (mac) {
                _iface->setProperty(kIOMACAddress, mac);
                mac->release();
            }
        }
    }
}

/* C-linkage trampoline registered with the compat layer; the IRQ bottom-half
 * calls it after tx_isr to resume a flow-control-stalled output queue. */
extern "C" void rtw88_tx_resume_trampoline(void)
{
    if (g_pci_dev_instance)
        g_pci_dev_instance->resumeTxIfStalled();
}

/* ------------------------------------------------------------------ */
/*  IOService lifecycle                                                 */
/* ------------------------------------------------------------------ */

bool RTW88PCIDevice::init(OSDictionary *props)
{
    IOLog("rtw88: RTW88PCIDevice::init\n");
    if (!super::init(props)) return false;
    _dmaLock        = IOSimpleLockAlloc();
    _pendingFreeLock = IOSimpleLockAlloc();
    return _dmaLock != nullptr && _pendingFreeLock != nullptr;
}

bool RTW88PCIDevice::start(IOService *provider)
{
    IOLog("rtw88: RTW88PCIDevice::start\n");
    if (!super::start(provider)) return false;

    _pciDev = OSDynamicCast(IOPCIDevice, provider);
    if (!_pciDev) {
        IOLog("rtw88: provider is not IOPCIDevice\n");
        return false;
    }
    _pciDev->retain();

    /* Enable bus mastering & memory space */
    _pciDev->setBusMasterEnable(true);
    _pciDev->setMemoryEnable(true);

    /* Map BAR2 — rtw88 driver hardcodes bar_id=2 in pci.c */
    _mmioMap = _pciDev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!_mmioMap) {
        IOLog("rtw88: failed to map BAR2\n");
        return false;
    }
    _mmioBase = (volatile void *)_mmioMap->getVirtualAddress();
    IOLog("rtw88: BAR2 mapped at %p, size 0x%llx\n",
          (void *)_mmioBase, (unsigned long long)_mmioMap->getLength());

    /* Install global compat ops */
    g_pci_dev_instance = this;
    rtw88_pci_io_ops   = &_pci_io_ops;
    rtw88_dma_ops      = &_dma_ops;

    /* Initialise compat runtime (workqueues, timers) */
    rtw88_compat_init();

    /* Build the pci_dev struct for the Linux driver */
    _compatPciDev = (struct pci_dev *)IOMallocZero(sizeof(struct pci_dev));
    if (!_compatPciDev) return false;

    _compatPciDev->vendor  = _pciDev->configRead16(0x00);
    _compatPciDev->device  = _pciDev->configRead16(0x02);
    _compatPciDev->kext_dev = this;
    _compatPciDev->resource[2]     = (resource_size_t)_mmioBase;
    _compatPciDev->resource_len[2] = (resource_size_t)_mmioMap->getLength();

    IOLog("rtw88: PCI device %04x:%04x\n",
          _compatPciDev->vendor, _compatPciDev->device);

    /* Set up IOWorkLoop */
    _workLoop = IOWorkLoop::workLoop();
    if (!_workLoop) return false;

    _cmdGate = IOCommandGate::commandGate(this);
    if (!_cmdGate) return false;
    _workLoop->addEventSource(_cmdGate);

    /* Set up interrupt */
    if (!setupInterrupt()) {
        IOLog("rtw88: setupInterrupt failed\n");
        return false;
    }

    /* Locate firmware Resources/ and set fw dir */
    rtw88_find_fw_dir();

    /* Create 802.11 state machine */
    _ieee80211 = RTW88IEEE80211::create(this, _compatPciDev);
    if (!_ieee80211) {
        IOLog("rtw88: failed to create RTW88IEEE80211\n");
        return false;
    }

    /* Force-disable BT coexistence.  rtw_pci_probe (inside create()) has
     * already run rtw_core_init which filled rtwdev->efuse.btcoex from
     * the chip's eFuse — but the actual coex setup happens later in
     * rtw_power_on (via _ieee80211->start() below).  Override now so the
     * coex driver initialises with wifi_only=true and never enables the
     * BT-side H2C/C2H exchange that appears to be wedging BE TX. */
    rtw88_force_wifi_only();

    /* Run the full probe now so the MAC address is populated before
     * the Ethernet interface is attached.  enable() will call
     * rtw_core_start() to power on the hardware for TX/RX. */
    IOReturn probeRet = _ieee80211->start();
    if (probeRet != kIOReturnSuccess) {
        IOLog("rtw88: probe failed (0x%08x)\n", probeRet);
        return false;
    }

    /* Attach Ethernet interface — MAC is now known */
    if (!attachDevice()) return false;

    _initialized = true;
    /* Wire TX flow-control resume: fired from the IRQ bottom-half after tx_isr
     * frees BE ring slots, so a stalled output queue gets re-serviced. */
    rtw88_set_tx_resume_cb(rtw88_tx_resume_trampoline);
    if (_intrSrc) _intrSrc->enable();

    /* Debug: poll BE ring + HISR/HIMR every second. Logs distinguish chip
     * stop vs interrupt loss vs IMR masking when TX appears stuck. */
    _debugTimer = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action,
                                   this, &RTW88PCIDevice::debugTimerFired));
    if (_debugTimer) {
        _workLoop->addEventSource(_debugTimer);
        _debugTimer->setTimeoutMS(1000);
    }

    IOLog("rtw88: device started successfully\n");
    registerService();   /* publish IOKit port for IOServiceOpen / rtw88ctl */
    return true;
}

void RTW88PCIDevice::debugTimerFired(IOTimerEventSource *src)
{
    unsigned int avail = rtw88_be_tx_avail();
    if (_txStalled && avail >= kRTW88TxResumeAvail)
        resumeTxIfStalled();
    if (_txStalled || avail < kRTW88TxStallAvail)
        rtw88_debug_dump_tx_state();
    src->setTimeoutMS(1000);   /* re-arm */
}

void RTW88PCIDevice::stop(IOService *provider)
{
    IOLog("rtw88: RTW88PCIDevice::stop\n");
    teardown();
    super::stop(provider);
}

void RTW88PCIDevice::free()
{
    if (_compatPciDev) { IOFree(_compatPciDev, sizeof(*_compatPciDev)); _compatPciDev = nullptr; }
    if (_pendingFreeLock) { drainPendingFree(); IOSimpleLockFree(_pendingFreeLock); _pendingFreeLock = nullptr; }
    if (_dmaLock)         { IOSimpleLockFree(_dmaLock);        _dmaLock        = nullptr; }
    super::free();
}

void RTW88PCIDevice::teardown()
{
    /* Stop the IRQ bottom-half from calling back into us before we tear down
     * the output queue it services. */
    rtw88_set_tx_resume_cb(nullptr);

    if (_debugTimer)
        _debugTimer->cancelTimeout();
    if (_intrSrc)
        _intrSrc->disable();
    if (_txQueue) {
        _txQueue->stop();
        _txQueue->flush();
    }

    if (_enabled) disable(_iface);
    drainPendingFree();   /* release any bounce bufs deferred during TX ISR */

    if (_ieee80211)  { _ieee80211->stop(); _ieee80211->release(); _ieee80211 = nullptr; }

    rtw88_compat_exit();

    if (_debugTimer) { _debugTimer->cancelTimeout(); _workLoop->removeEventSource(_debugTimer); _debugTimer->release(); _debugTimer = nullptr; }
    if (_intrSrc)  { _workLoop->removeEventSource(_intrSrc); _intrSrc->release();  _intrSrc = nullptr; }
    if (_cmdGate)  { _workLoop->removeEventSource(_cmdGate); _cmdGate->release();  _cmdGate = nullptr; }
    if (_txQueue)  { _txQueue->release();   _txQueue = nullptr; }
    if (_iface)    { detachInterface(_iface); _iface->release(); _iface = nullptr; }
    if (_workLoop) { _workLoop->release();   _workLoop = nullptr; }
    if (_mmioMap)  { _mmioMap->release();    _mmioMap = nullptr; _mmioBase = nullptr; }
    if (_pciDev)   { _pciDev->release();     _pciDev = nullptr; }

    g_pci_dev_instance = nullptr;
    rtw88_pci_io_ops   = nullptr;
    rtw88_dma_ops      = nullptr;
}

/* ------------------------------------------------------------------ */
/*  Interrupt                                                           */
/* ------------------------------------------------------------------ */

bool RTW88PCIDevice::setupInterrupt()
{
    _intrSrc = IOInterruptEventSource::interruptEventSource(
        this,
        OSMemberFunctionCast(IOInterruptEventSource::Action,
                             this, &RTW88PCIDevice::handleInterrupt),
        _pciDev, 0);

    if (!_intrSrc) {
        IOLog("rtw88: failed to create interrupt event source\n");
        return false;
    }
    _workLoop->addEventSource(_intrSrc);

    return true;
}

void RTW88PCIDevice::handleInterrupt(IOInterruptEventSource *src, int count)
{
    if (_ieee80211)
        rtw88_trigger_interrupt();
}

/* ------------------------------------------------------------------ */
/*  IOEthernetController / IONetworkController                          */
/* ------------------------------------------------------------------ */

bool RTW88PCIDevice::attachDevice()
{
    if (!attachInterface((IONetworkInterface **)&_iface)) {
        IOLog("rtw88: attachInterface failed\n");
        return false;
    }

    publishHardwareIdentity();

    if (!setupMediumDict()) return false;

    _txQueue = OSDynamicCast(IOGatedOutputQueue, getOutputQueue());
    if (_txQueue) _txQueue->retain();

    _iface->registerService();
    return true;
}

bool RTW88PCIDevice::setupMediumDict()
{
    OSDictionary *mediums = OSDictionary::withCapacity(4);
    if (!mediums) return false;

    addMedium(mediums, kIOMediumEthernetAuto, 0);
    addMedium(mediums, kIOMediumEthernet10BaseT | kIOMediumOptionFullDuplex,  10);
    addMedium(mediums, kIOMediumEthernet100BaseTX | kIOMediumOptionFullDuplex, 100);
    addMedium(mediums, kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex, 1000);

    setProperty(kIOMediumDictionary, mediums);
    mediums->release();

    IONetworkMedium *autoMedium = IONetworkMedium::getMediumWithType(
        OSDynamicCast(OSDictionary, getProperty(kIOMediumDictionary)),
        kIOMediumEthernetAuto);
    setCurrentMedium(autoMedium);
    setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid,
                  IONetworkMedium::getMediumWithType(
                      OSDynamicCast(OSDictionary, getProperty(kIOMediumDictionary)),
                      kIOMediumEthernet1000BaseT | kIOMediumOptionFullDuplex));
    return true;
}

void RTW88PCIDevice::addMedium(OSDictionary *mediums, IOMediumType type, UInt64 speed)
{
    IONetworkMedium *m = IONetworkMedium::medium(type, speed * 1000000ULL);
    if (m) {
        IONetworkMedium::addMedium(mediums, m);
        m->release();
    }
}

IOReturn RTW88PCIDevice::enable(IONetworkInterface *iface)
{
    IOLog("rtw88: enable\n");
    if (_enabled) return kIOReturnSuccess;
    if (!_ieee80211) return kIOReturnNotReady;

    /* Probe ran in start(); now power on the hardware for TX/RX */
    IOReturn ret = _ieee80211->powerOn();
    if (ret != kIOReturnSuccess) {
        IOLog("rtw88: powerOn failed (0x%08x)\n", ret);
        return ret;
    }

    if (_txQueue) _txQueue->start();
    if (_intrSrc) _intrSrc->enable();
    _enabled = true;
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::disable(IONetworkInterface *iface)
{
    IOLog("rtw88: disable\n");
    if (!_enabled) return kIOReturnSuccess;
    _enabled = false;
    if (_intrSrc) _intrSrc->disable();
    if (_txQueue) _txQueue->stop();
    if (_txQueue) _txQueue->flush();
    if (_ieee80211) _ieee80211->powerOff();
    return kIOReturnSuccess;
}

IOOutputQueue *RTW88PCIDevice::createOutputQueue()
{
    /*
     * Without an output queue, IONetworkController delivers outputPacket()
     * straight from the networking stack, which can call it concurrently from
     * multiple threads.  rtw_pci_tx_write_data() computes the TX buffer-
     * descriptor slot from ring->r.wp and fills it BEFORE taking irq_lock
     * (only the wp increment is locked), so two concurrent submissions fill the
     * same slot and skip the next, leaving a zeroed descriptor in the BE ring.
     * The chip then stalls its TX DMA on that zero descriptor (HW rp frozen,
     * FIFO empty) and the shared DMA wedges RX too.
     *
     * An IOGatedOutputQueue runs every outputPacket() under the work-loop gate,
     * serializing submission so the ring fill is single-threaded.
     */
    return IOGatedOutputQueue::withTarget(this, getWorkLoop(), 256);
}

UInt32 RTW88PCIDevice::outputPacket(mbuf_t m, void *param)
{
    drainPendingFree();
    if (!_enabled || !_ieee80211) {
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    /*
     * Backpressure instead of dropping.  When the BE ring is nearly full,
     * rtw_pci_tx_write_data() would return -ENOSPC and rtw_tx() would FREE the
     * skb — silently dropping it.  Under a sustained transfer those dropped
     * frames (TCP ACKs/data) stall the connection.  Returning
     * kIOReturnOutputStall makes IOGatedOutputQueue hold this exact packet and
     * stop dispatching; resumeTxIfStalled() (fired from the IRQ bottom-half
     * after tx_isr frees slots) re-services the queue.  Threshold leaves
     * headroom so rtw_tx never actually hits -ENOSPC.
     */
    if (rtw88_be_tx_avail() < kRTW88TxStallAvail) {
        _txStalled = true;
        return kIOReturnOutputStall;
    }
    return _ieee80211->outputPacket(m);
}

void RTW88PCIDevice::resumeTxIfStalled()
{
    /* Runs on the IRQ bottom-half thread (no rtw88 locks held). Use async
     * service so we never block on the output-queue gate from here. */
    if (_txStalled && rtw88_be_tx_avail() >= kRTW88TxResumeAvail) {
        _txStalled = false;
        if (_txQueue)
            _txQueue->service(IOBasicOutputQueue::kServiceAsync);
    }
}

IOReturn RTW88PCIDevice::getHardwareAddress(IOEthernetAddress *addr)
{
    if (!_ieee80211) return kIOReturnNotReady;
    _ieee80211->getMACAddress(addr->bytes);
    memcpy(_macAddr.bytes, addr->bytes, 6);
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::setHardwareAddress(const IOEthernetAddress *addr)
{
    memcpy(_macAddr.bytes, addr->bytes, 6);
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::getMaxPacketSize(UInt32 *maxSize) const
{
    *maxSize = 2346; /* IEEE80211 max MSDU */
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::setMaxPacketSize(UInt32 maxSize)
{
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::selectMedium(const IONetworkMedium *medium)
{
    setCurrentMedium(medium);
    return kIOReturnSuccess;
}

bool RTW88PCIDevice::configureInterface(IONetworkInterface *iface)
{
    if (!super::configureInterface(iface)) return false;
    IONetworkData *nd = iface->getNetworkData(kIONetworkStatsKey);
    if (nd) nd->setAccessTypes(kIONetworkDataAccessTypeRead);
    return true;
}

IOReturn RTW88PCIDevice::getPacketFilters(const OSSymbol *group,
                                            UInt32 *filters) const
{
    if (group->isEqualTo(kIOEthernetWakeOnLANFilterGroup)) {
        *filters = 0;
        return kIOReturnSuccess;
    }
    return super::getPacketFilters(group, filters);
}

IOReturn RTW88PCIDevice::setMulticastMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::setPromiscuousMode(bool active)
{
    return kIOReturnSuccess;
}

IOReturn RTW88PCIDevice::powerStateWillChangeTo(IOPMPowerFlags flags,
                                                  unsigned long state,
                                                  IOService *actor)
{
    IOLog("rtw88: powerStateWillChangeTo %lu\n", state);
    return IOPMAckImplied;
}

/* ------------------------------------------------------------------ */
/*  RX injection (called from RTW88IEEE80211 on frame receive)         */
/* ------------------------------------------------------------------ */

mbuf_t RTW88PCIDevice::allocateInputPacket(uint32_t len)
{
    /* IONetworkController::allocatePacket returns an mbuf set up exactly the
     * way inputPacket() expects: m_len and m_pkthdr.len are both set and
     * consistent across every segment of the chain.  Hand-rolling this with
     * mbuf_allocpacket left m_len inconsistent with pkthdr.len, which the
     * dlil input validator rejects with "Failed mbuf validity check: len -14". */
    return allocatePacket(len);
}

void RTW88PCIDevice::injectRxFrame(mbuf_t m)
{
    drainPendingFree();
    if (!_iface || !_enabled) {
        freePacket(m);
        return;
    }

    /* Last line of defense: never hand the networking stack a malformed
     * packet.  Validate BOTH length fields — the dlil validator panics on
     * m_len (printed as "len"), not just pkthdr.len.  mbuf_len/mbuf_pkthdr_len
     * return size_t, so a negative m_len shows up as a huge value here. */
    size_t plen = mbuf_pkthdr_len(m);
    size_t mlen = mbuf_len(m);
    if (plen < 14 || plen > 4096 || mlen < 14 || mlen > 4096) {
        IOLog("rtw88: injectRxFrame: dropping bogus mbuf (pkthdr.len=%zu m_len=%zu)\n",
              plen, mlen);
        freePacket(m);
        return;
    }

    /* Queue + flush, matching the proven itlwm submission path.  Submitting
     * via the input queue keeps frame delivery off whatever thread called us. */
    _iface->inputPacket(m, 0, IONetworkInterface::kInputOptionQueuePacket);
    _iface->flushInputQueue();

    IONetworkData *nd = _iface->getNetworkData(kIONetworkStatsKey);
    if (nd) {
        IONetworkStats *stats = (IONetworkStats *)nd->getBuffer();
        if (stats) stats->inputPackets++;
    }
}

/* ------------------------------------------------------------------ */
/*  DMA coherent allocation                                             */
/* ------------------------------------------------------------------ */

void *RTW88PCIDevice::allocCoherent(size_t size, IOPhysicalAddress *phys)
{
    /*
     * rtw88 TX ring descriptors store DMA addresses in 32-bit fields.
     * Restrict physical allocation to the first 4GB so truncation to
     * cpu_to_le32() in the ring descriptor is lossless.
     * 0x00000000FFFFFFF0 = below 4GB, 16-byte aligned.
     */
    IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOMemoryKernelUserShared,
        size,
        0x00000000FFFFFFF0ULL);

    if (!desc) { IOLog("rtw88: dma alloc failed, size=%zu\n", size); return nullptr; }
    desc->prepare();

    IOPhysicalAddress pa = desc->getPhysicalAddress();
    void *va = desc->getBytesNoCopy();
    memset(va, 0, size);

    if (phys) *phys = pa;

    /* Allocate the tracking node OUTSIDE the spinlock — IOMallocZero is
     * sleepable, and IOSimpleLock disables preemption.  Calling a sleepable
     * allocator from a preemption-disabled context panics XNU with
     * "blocking while holding a spinlock". */
    DMAEntry *entry = (DMAEntry *)IOMallocZero(sizeof(DMAEntry));
    if (!entry) {
        desc->complete();
        desc->release();
        return nullptr;
    }
    entry->desc = desc;
    entry->virt = va;
    entry->phys = pa;
    entry->size = size;

    IOSimpleLockLock(_dmaLock);
    entry->next = _dmaList;
    _dmaList = entry;
    IOSimpleLockUnlock(_dmaLock);

    return va;
}

void RTW88PCIDevice::freeCoherent(size_t size, void *virt, IOPhysicalAddress phys)
{
    IOSimpleLockLock(_dmaLock);
    DMAEntry **prev = &_dmaList;
    for (DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->virt == virt) {
            *prev = e->next;
            IOSimpleLockUnlock(_dmaLock);
            if (preemption_enabled()) {
                e->desc->complete();
                e->desc->release();
                IOFree(e, sizeof(*e));
            } else {
                IOSimpleLockLock(_pendingFreeLock);
                e->next = _dmaPendingFree;
                _dmaPendingFree = e;
                IOSimpleLockUnlock(_pendingFreeLock);
            }
            return;
        }
        prev = &e->next;
    }
    IOSimpleLockUnlock(_dmaLock);
    IOLog("rtw88: freeCoherent: virt %p not found\n", virt);
}

void RTW88PCIDevice::freeCoherentByPhys(IOPhysicalAddress phys)
{
    IOSimpleLockLock(_dmaLock);
    DMAEntry **prev = &_dmaList;
    for (DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->phys == phys) {
            *prev = e->next;
            IOSimpleLockUnlock(_dmaLock);
            if (preemption_enabled()) {
                e->desc->complete();
                e->desc->release();
                IOFree(e, sizeof(*e));
            } else {
                IOSimpleLockLock(_pendingFreeLock);
                e->next = _dmaPendingFree;
                _dmaPendingFree = e;
                IOSimpleLockUnlock(_pendingFreeLock);
            }
            return;
        }
        prev = &e->next;
    }
    IOSimpleLockUnlock(_dmaLock);
}

void RTW88PCIDevice::drainPendingFree()
{
    if (!_pendingFreeLock) return;
    IOSimpleLockLock(_pendingFreeLock);
    DMAEntry *list      = _dmaPendingFree;
    _dmaPendingFree     = nullptr;
    IOSimpleLockUnlock(_pendingFreeLock);

    for (DMAEntry *e = list; e; ) {
        DMAEntry *next = e->next;
        e->desc->complete();
        e->desc->release();
        IOFree(e, sizeof(*e));
        e = next;
    }
}

void RTW88PCIDevice::setBounceOrigVA(IOPhysicalAddress phys, void *orig_va)
{
    IOSimpleLockLock(_dmaLock);
    for (DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->phys == phys) {
            e->orig_va = orig_va;
            break;
        }
    }
    IOSimpleLockUnlock(_dmaLock);
}

void RTW88PCIDevice::syncBounceForCpu(IOPhysicalAddress dma, size_t size)
{
    /*
     * Called by dma_sync_single_for_cpu(DMA_FROM_DEVICE) after the chip
     * has finished writing received packet data into the bounce buffer.
     * Copy bounce → original skb->data so the driver can parse the packet.
     */
    IOSimpleLockLock(_dmaLock);
    for (DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->phys == dma && e->orig_va && e->virt) {
            size_t copy_len = (size <= e->size) ? size : e->size;
            IOSimpleLockUnlock(_dmaLock);
            memcpy(e->orig_va, e->virt, copy_len);
            return;
        }
    }
    IOSimpleLockUnlock(_dmaLock);
}

/* ------------------------------------------------------------------ */
/*  PCI config space                                                    */
/* ------------------------------------------------------------------ */

UInt8 RTW88PCIDevice::pciReadByte(int offset)
{
    return _pciDev->configRead8((UInt8)offset);
}
UInt16 RTW88PCIDevice::pciReadWord(int offset)
{
    return _pciDev->configRead16((UInt8)offset);
}
UInt32 RTW88PCIDevice::pciReadDword(int offset)
{
    return _pciDev->configRead32((UInt8)offset);
}
void RTW88PCIDevice::pciWriteByte(int offset, UInt8 val)
{
    _pciDev->configWrite8((UInt8)offset, val);
}
void RTW88PCIDevice::pciWriteWord(int offset, UInt16 val)
{
    _pciDev->configWrite16((UInt8)offset, val);
}
void RTW88PCIDevice::pciWriteDword(int offset, UInt32 val)
{
    _pciDev->configWrite32((UInt8)offset, val);
}
int RTW88PCIDevice::pciFindCapability(int cap)
{
    if (!_pciDev || cap < 0 || cap > 0xff)
        return 0;

    UInt8 offset = 0;
    UInt32 value = _pciDev->findPCICapability((UInt8)cap, &offset);
    return value ? (int)offset : 0;
}

/* ------------------------------------------------------------------ */
/*  IOUserClient creation                                               */
/* ------------------------------------------------------------------ */

IOReturn RTW88PCIDevice::newUserClient(task_t owningTask, void *securityID,
                                        UInt32 type, OSDictionary *properties,
                                        IOUserClient **handler)
{
    RTW88UserClient *client = new RTW88UserClient;
    if (!client) {
        IOLog("rtw88: RTW88UserClient allocation failed\n");
        return kIOReturnNoMemory;
    }

    /* initWithTask — not init() — binds the Mach task port.
     * Without this the kernel port is never "ready for callouts" and
     * IOServiceOpen returns kIOReturnBadArgument before our code runs. */
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        IOLog("rtw88: RTW88UserClient::initWithTask failed\n");
        client->release();
        return kIOReturnBadArgument;
    }

    if (!client->attach(this)) {
        IOLog("rtw88: RTW88UserClient::attach failed\n");
        client->release();
        return kIOReturnBadArgument;
    }

    if (!client->start(this)) {
        IOLog("rtw88: RTW88UserClient::start failed\n");
        client->detach(this);
        client->release();
        return kIOReturnBadArgument;
    }

    *handler = client;
    return kIOReturnSuccess;
}
