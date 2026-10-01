// SPDX-License-Identifier: GPL-2.0
#include "AirPortRTW89.hpp"

#include <IOKit/IOLib.h>

#define super IOService
OSDefineMetaClassAndStructors(AirPort_RTW89, IOService)

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

    LOG("probing");
    ret = rtw89_glue_probe(&platform, &device);
    if (ret) {
        LOG("probe failed: %d", ret);
        teardown();
        return false;
    }
    _probed = true;

    publishInfo();

    /* The interrupt source stays disabled: probe leaves the chip powered down
     * and nothing brings it up again until there is an interface (M2), which
     * is when the source gets enabled around the driver's start(). */

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

void AirPort_RTW89::teardown()
{
    if (_interrupt)
        _interrupt->disable();

    if (_probed) {
        _probed = false;
        rtw89_glue_remove();
        /* The compat workqueue threads signal that they are done just before
         * they exit; let them get out of this kext's code before it can be
         * unloaded. */
        IOSleep(200);
    }

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
    super::free();
}
