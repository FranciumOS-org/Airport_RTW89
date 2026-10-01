/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Native IO80211 controller for the RTL8822BE port.
 */
#include "AirportRTW88.hpp"
#include "AirportRTW88Interface.hpp"
#include "RTW88AssocWire.hpp"
#include "RTW88UserClient.hpp"

extern "C" {
#include "../compat/rtw88_compat.h"
}
extern "C" boolean_t preemption_enabled(void);

extern "C" void rtw88_trigger_interrupt(void);  /* esta si es la unica con extern "C" real, ver RTW88PCIDevice.cpp */  /* struct RTW88StateResult vive aca */
#include <IOKit/network/IOOutputQueue.h>  /* kIOReturnOutputDropped */
#include <IOKit/network/IOBasicOutputQueue.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOLib.h>                  /* kprintf */
#include <libkern/copyio.h>               /* copyin/copyout for legacy ioctl bridge */
#include <sys/errno.h>                    /* EINVAL/EFAULT/ENOMEM */
#include <libkern/version.h>              /* scan-cache bootstrap compatibility only */

#define super IO80211Controller

/* Keep enough headroom in the 256-entry BE ring that rtw_tx() never reaches
 * its internal -ENOSPC/drop path. Resume with wide hysteresis so a coalesced
 * completion cannot park the IOOutputQueue on a marginal threshold. */
static constexpr unsigned int kRTW88TxStallAvail  = 96;
static constexpr unsigned int kRTW88TxResumeAvail = 160;

enum : unsigned long {
    kRTW88PowerStateOff = 0,
    kRTW88PowerStateOn = 1,
    kRTW88PowerStateCount = 2,
};

/* IONetworkController asks subclasses to register power states through
 * registerWithPolicyMaker().  State 0 intentionally advertises no device
 * capability; state 1 means the PCI function is usable. */
static IOPMPowerState gRTW88PowerStates[kRTW88PowerStateCount] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn,
      0, 0, 0, 0, 0, 0, 0, 0 },
};

static const char *rtw88AirportChipName(const struct pci_dev *pdev)
{
    if (!pdev) return "Realtek Wireless";
    switch (pdev->device) {
    case 0xB822: return "RTL8822BE";
    case 0xC822:
    case 0xC82F: return "RTL8822CE";
    case 0xC821:
    case 0xB821: return "RTL8821CE";
    default: return "Realtek Wireless";
    }
}

extern "C" void rtw88_airport_tx_resume_trampoline(void)
{
    if (g_pci_dev_instance)
        g_pci_dev_instance->resumeTxIfStalled();
}

OSDefineMetaClassAndStructors(AirportRTW88, IO80211Controller)

bool AirportRTW88::init(OSDictionary *props)
{
    return super::init(props);
}

/* Match AirportItlwm's IO80211Controller lifecycle.  IONetworkController::
 * start() calls this virtual before AirportRTW88::start() continues; using an
 * IO80211WorkLoop keeps controller IOCTL/VIF callbacks, our interrupt source
 * and RX injection on the same controller-owned workloop. */
bool AirportRTW88::createWorkLoop()
{
    if (_workLoop)
        return true;
    _workLoop = IO80211WorkLoop::workLoop();
    IOLog("AirPort_RTW88: createWorkLoop IO80211WorkLoop=%p\n", _workLoop);
    return _workLoop != nullptr;
}

IOWorkLoop *AirportRTW88::getWorkLoop() const
{
    return _workLoop;
}

bool AirportRTW88::start(IOService *provider)
{
    setProperty("DriverBuild", "2.0.0-awdl-peer-lifecycle-sta-recovery");
    setProperty("STA_V19_DARWIN_ENOTSUP_FIX", kOSBooleanTrue);
    setProperty("STA_V20_POWERSAVE_PREFLIGHT_FIX", kOSBooleanTrue);
    IOLog("AirportRTW88: start\n");
    _pciDev = OSDynamicCast(IOPCIDevice, provider);
    if (!_pciDev) {
        IOLog("AirportRTW88: provider is not IOPCIDevice\n");
        return false;
    }
    if (!super::start(provider)) {
        _pciDev = nullptr;
        return false;
    }
    _superStarted = true;
    _pciDev->retain();

    /* Locks para tracking DMA -- antes faltaban por completo */
    _dmaLock = IOSimpleLockAlloc();
    _pendingFreeLock = IOSimpleLockAlloc();
    if (!_dmaLock || !_pendingFreeLock)
        return failStart(provider, "failed to allocate DMA locks");

    /* Habilitar bus mastering y espacio de memoria */
    _pciDev->setBusMasterEnable(true);
    _pciDev->setMemoryEnable(true);

    /* Mapear BAR2 -- rtw88 hardcodea bar_id=2 en pci.c */
    _mmioMap = _pciDev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!_mmioMap) {
        return failStart(provider, "failed to map BAR2");
    }
    _mmioBase = (volatile void *)_mmioMap->getVirtualAddress();
    IOLog("AirportRTW88: BAR2 mapped at %p, size 0x%llx\n",
          (void *)_mmioBase, (unsigned long long)_mmioMap->getLength());

    /* Instalar callbacks compartidos de compat (mismos que usa RTW88PCIDevice,
     * ahora apuntando a esta instancia via el puntero generico RTW88HwOps*) */
    g_pci_dev_instance = this;
    rtw88_pci_io_ops    = &_pci_io_ops;
    rtw88_dma_ops       = &_dma_ops;

    if (rtw88_compat_init() != 0)
        return failStart(provider, "compat initialization failed");
    _compatInitialized = true;

    _compatPciDev = (struct pci_dev *)IOMallocZero(sizeof(struct pci_dev));
    if (!_compatPciDev)
        return failStart(provider, "failed to allocate compat PCI device");

    _compatPciDev->vendor  = _pciDev->configRead16(0x00);
    _compatPciDev->device  = _pciDev->configRead16(0x02);
    _compatPciDev->kext_dev = this;
    _compatPciDev->resource[2]     = (resource_size_t)_mmioBase;
    _compatPciDev->resource_len[2] = (resource_size_t)_mmioMap->getLength();

    IOLog("AirportRTW88: PCI device %04x:%04x\n",
          _compatPciDev->vendor, _compatPciDev->device);

    /* super::start() has already called our createWorkLoop().  AirportItlwm
     * relies on that controller-owned IO80211WorkLoop; do not create a second
     * parallel loop here. */
    if (!_workLoop)
        return failStart(provider, "IO80211 workloop unavailable after super::start");

    if (!setupInterrupt()) {
        return failStart(provider, "failed to set up interrupt");
    }

    rtw88_find_fw_dir();

    /* Crear la maquina de estados 802.11 -- AHORA con el pci_dev real,
     * en vez de nullptr como antes */
    _ieee80211 = RTW88IEEE80211::createAirport(this, _compatPciDev);
    if (!_ieee80211) {
        return failStart(provider, "failed to create IEEE80211 state machine");
    }
    _ieee80211->setEventDelegate(this);
    _ieee80211->setRxDelegate(this);

    rtw88_force_wifi_only();

    IOReturn probeRet = _ieee80211->start();
    if (probeRet != kIOReturnSuccess) {
        IOLog("AirportRTW88: probe failed (0x%08x)\n", probeRet);
        return failStart(provider, "IEEE80211 start failed");
    }
    _ieeeStarted = true;

    _awdlManager = new RTW88AWDLManager;
    if (!_awdlManager || !_awdlManager->init(_ieee80211, _workLoop, this, &AirportRTW88::awdlTimerFired))
        return failStart(provider, "failed to initialize AWDL/P2P manager");
    IOLog("AirPort_RTW88: AWDL/P2P manager initialized (1.0.1 partial support)\n");

    /* AirportItlwm Ventura publishes and selects the medium before
     * attachInterface().  This is deliberately kept in the legacy Ventura
     * frontend so OCLP's IO80211FamilyLegacy sees the same controller
     * lifecycle on Sonoma/Sequoia. */
    const IONetworkMedium *primaryMedium = nullptr;
    if (!createMediumTables(&primaryMedium) || !primaryMedium ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium))
        return failStart(provider, "failed to publish/select Wi-Fi medium");
    IOLog("AirPort_RTW88: medium table ready before attachInterface\n");

    /* Let IO80211/IONetworkController prepare and configure the client.
     * Calling init/attach directly bypasses the controller's lifecycle. */
    IOLog("AirportRTW88: calling attachInterface (AirportItlwm lifecycle, attach=true)\n");
    /* AirportItlwm passes its member pointer directly.  This matters when
     * attach=true because matching/registration can be synchronous: make the
     * primary interface visible to our callbacks before attachInterface()
     * returns instead of assigning it afterwards from a temporary. */
    if (!attachInterface((IONetworkInterface **)&_netif, true) || !_netif)
        return failStart(provider, "controller attachInterface failed");
    if (!OSDynamicCast(AirportRTW88Interface, _netif)) {
        detachInterface(_netif, true);
        _netif->release();
        _netif = nullptr;
        return failStart(provider, "controller returned unexpected interface type");
    }
    _netifAttached = true;

    /* IO80211Controller owns an IOOutputQueue too. Backpressure it before the
     * Realtek BE ring reaches -ENOSPC; otherwise rtw_tx() frees the skb and
     * macOS incorrectly believes the packet was transmitted. */
    _txStalled = false;
    rtw88_set_tx_resume_cb(rtw88_airport_tx_resume_trampoline);
    if (_intrSrc) _intrSrc->enable();

    /* AirportItlwm publishes a valid-but-not-yet-associated link, then both
     * the controller service and the interface.  The controller publication
     * is important for IO80211 clients that own virtual-interface lifecycle. */
    IO80211Controller::setLinkStatus(kIONetworkLinkValid);
    registerService();
    _netif->registerService();
    IOLog("AirPort_RTW88: controller and network interface registered\n");

    // Legacy IO80211 on newer hosts may never request the AWDL interface.
    // Attach on a later workloop turn, after start/publication has completed.
    // Discovery failure must not fail the working STA startup.
    _awdlManager->scheduleDiscovery();

    IOLog("AirportRTW88: device started successfully\n");
    return true;
}

bool AirportRTW88::failStart(IOService *provider, const char *reason)
{
    IOLog("AirportRTW88: start failed: %s\n", reason ? reason : "unknown error");

    /* Match AirportItlwm/IONetworkController ownership ordering: IO80211 must
     * stop while the controller workloop, interface and backend still exist.
     * Releasing those first can leave super::stop() touching freed state. */
    if (_superStarted) {
        super::stop(provider);
        _superStarted = false;
    }
    teardown();
    return false;
}

bool AirportRTW88::setupInterrupt()
{
    _intrSrc = IOInterruptEventSource::interruptEventSource(
        this,
        OSMemberFunctionCast(IOInterruptEventSource::Action,
                             this, &AirportRTW88::handleInterrupt),
        _pciDev, 0);
    if (!_intrSrc) {
        IOLog("AirportRTW88: failed to create interrupt event source\n");
        return false;
    }
    if (_workLoop->addEventSource(_intrSrc) != kIOReturnSuccess) {
        _intrSrc->release();
        _intrSrc = nullptr;
        return false;
    }
    return true;
}

void AirportRTW88::handleInterrupt(IOInterruptEventSource *src, int count)
{
    (void)src; (void)count;
    /* Controller workloop is a safe thread context. Retire DMA descriptors
     * deferred by a previous IRQ/NAPI pass before handling more traffic. */
    if (preemption_enabled())
        drainPendingFree();
    if (!__atomic_load_n(&_dmaStopped, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&_pmTransition, __ATOMIC_ACQUIRE) && _ieee80211)
        rtw88_trigger_interrupt();
}

UInt8 AirportRTW88::pciReadByte(int offset)   { return _pciDev->configRead8((UInt8)offset); }
UInt16 AirportRTW88::pciReadWord(int offset)  { return _pciDev->configRead16((UInt8)offset); }
UInt32 AirportRTW88::pciReadDword(int offset) { return _pciDev->configRead32((UInt8)offset); }
void AirportRTW88::pciWriteByte(int offset, UInt8 val)   { _pciDev->configWrite8((UInt8)offset, val); }
void AirportRTW88::pciWriteWord(int offset, UInt16 val)  { _pciDev->configWrite16((UInt8)offset, val); }
void AirportRTW88::pciWriteDword(int offset, UInt32 val) { _pciDev->configWrite32((UInt8)offset, val); }

int AirportRTW88::pciFindCapability(int cap)
{
    if (!_pciDev || cap < 0 || cap > 0xff)
        return 0;

    UInt8 offset = 0;
    UInt32 value = _pciDev->findPCICapability((UInt8)cap, &offset);
    return value ? (int)offset : 0;
}

void *AirportRTW88::allocCoherent(size_t size, IOPhysicalAddress *phys)
{
    /* DMA unmaps can be deferred by IRQ/NAPI. Reclaim them whenever allocation
     * returns to a sleepable thread so sustained traffic cannot grow the list. */
    if (preemption_enabled())
        drainPendingFree();

    IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOMemoryKernelUserShared,
        size,
        0x00000000FFFFFFF0ULL);
    if (!desc) { IOLog("AirportRTW88: dma alloc failed, size=%zu\n", size); return nullptr; }
    if (desc->prepare() != kIOReturnSuccess) { desc->release(); return nullptr; }

    IOPhysicalAddress pa = desc->getPhysicalAddress();
    void *va = desc->getBytesNoCopy();
    if (!va || !pa) { desc->complete(); desc->release(); return nullptr; }
    memset(va, 0, size);
    if (phys) *phys = pa;

    RTW88DMAEntry *entry = (RTW88DMAEntry *)IOMallocZero(sizeof(RTW88DMAEntry));
    if (!entry) { desc->complete(); desc->release(); return nullptr; }
    entry->desc = desc; entry->virt = va; entry->phys = pa; entry->size = size;

    IOSimpleLockLock(_dmaLock);
    entry->next = _dmaList;
    _dmaList = entry;
    IOSimpleLockUnlock(_dmaLock);
    return va;
}

void AirportRTW88::freeCoherent(size_t size, void *virt, IOPhysicalAddress phys)
{
    IOSimpleLockLock(_dmaLock);
    RTW88DMAEntry **prev = &_dmaList;
    for (RTW88DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->virt == virt) {
            *prev = e->next;
            IOSimpleLockUnlock(_dmaLock);
            if (preemption_enabled()) {
                e->desc->complete(); e->desc->release(); IOFree(e, sizeof(*e));
            } else {
                IOSimpleLockLock(_pendingFreeLock);
                e->next = _dmaPendingFree; _dmaPendingFree = e;
                IOSimpleLockUnlock(_pendingFreeLock);
            }
            return;
        }
        prev = &e->next;
    }
    IOSimpleLockUnlock(_dmaLock);
    IOLog("AirportRTW88: freeCoherent: virt %p not found\n", virt);
}

void AirportRTW88::freeCoherentByPhys(IOPhysicalAddress phys)
{
    IOSimpleLockLock(_dmaLock);
    RTW88DMAEntry **prev = &_dmaList;
    for (RTW88DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->phys == phys) {
            *prev = e->next;
            IOSimpleLockUnlock(_dmaLock);
            if (preemption_enabled()) {
                e->desc->complete(); e->desc->release(); IOFree(e, sizeof(*e));
            } else {
                IOSimpleLockLock(_pendingFreeLock);
                e->next = _dmaPendingFree; _dmaPendingFree = e;
                IOSimpleLockUnlock(_pendingFreeLock);
            }
            return;
        }
        prev = &e->next;
    }
    IOSimpleLockUnlock(_dmaLock);
}

void AirportRTW88::setBounceOrigVA(IOPhysicalAddress phys, void *orig_va)
{
    IOSimpleLockLock(_dmaLock);
    for (RTW88DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->phys == phys) { e->orig_va = orig_va; break; }
    }
    IOSimpleLockUnlock(_dmaLock);
}

void AirportRTW88::syncBounceForCpu(IOPhysicalAddress dma, size_t size)
{
    IOSimpleLockLock(_dmaLock);
    for (RTW88DMAEntry *e = _dmaList; e; e = e->next) {
        if (e->phys == dma && e->orig_va && e->virt) {
            size_t copy_len = (size <= e->size) ? size : e->size;
            IOSimpleLockUnlock(_dmaLock);
            memcpy(e->orig_va, e->virt, copy_len);
            return;
        }
    }
    IOSimpleLockUnlock(_dmaLock);
}

void AirportRTW88::resumeTxIfStalled()
{
    /* IRQ bottom-half/NAPI run on the ordered compat datapath worker. This is a
     * safe place to retire deferred DMA mappings and kick a deliberately
     * stalled IO80211 output queue after TX descriptors have been reclaimed. */
    if (preemption_enabled())
        drainPendingFree();

    if (!_txStalled || rtw88_be_tx_avail() < kRTW88TxResumeAvail)
        return;

    _txStalled = false;
    IOOutputQueue *q = getOutputQueue();
    if (q)
        q->service(IOBasicOutputQueue::kServiceAsync);
}

void AirportRTW88::drainPendingFree()
{
    if (!_pendingFreeLock) return;
    IOSimpleLockLock(_pendingFreeLock);
    RTW88DMAEntry *list = _dmaPendingFree;
    _dmaPendingFree = nullptr;
    IOSimpleLockUnlock(_pendingFreeLock);
    for (RTW88DMAEntry *e = list; e; ) {
        RTW88DMAEntry *next = e->next;
        e->desc->complete(); e->desc->release(); IOFree(e, sizeof(*e));
        e = next;
    }
}

void AirportRTW88::releaseDMAEntries()
{
    if (!_dmaLock) return;

    IOSimpleLockLock(_dmaLock);
    RTW88DMAEntry *list = _dmaList;
    _dmaList = nullptr;
    IOSimpleLockUnlock(_dmaLock);

    for (RTW88DMAEntry *entry = list; entry; ) {
        RTW88DMAEntry *next = entry->next;
        if (entry->desc) {
            entry->desc->complete();
            entry->desc->release();
        }
        IOFree(entry, sizeof(*entry));
        entry = next;
    }
}

void AirportRTW88::teardown()
{
    __atomic_store_n(&_shutdown, true, __ATOMIC_RELEASE);
    /* Stop asynchronous output-queue wakeups before interface teardown. */
    rtw88_set_tx_resume_cb(nullptr);
    _txStalled = false;
    (void)stopDMA();
    if (_intrSrc)
        _intrSrc->disable();

    if (_awdlManager)
        _awdlManager->reset();

    if (_ieee80211) {
        if (_ieeeStarted)
            _ieee80211->stop();
        _ieeeStarted = false;
        _ieee80211->release();
        _ieee80211 = nullptr;
    }

    if (_awdlManager) {
        delete _awdlManager;
        _awdlManager = nullptr;
    }

    if (_netif) {
        if (_netifAttached)
            detachInterface(_netif, true);
        _netifAttached = false;
        _netif->release();
        _netif = nullptr;
    }

    drainPendingFree();
    releaseDMAEntries();

    if (_compatInitialized) {
        rtw88_compat_exit();
        _compatInitialized = false;
    }

    if (_intrSrc) {
        if (_workLoop)
            _workLoop->removeEventSource(_intrSrc);
        _intrSrc->release();
        _intrSrc = nullptr;
    }
    if (_workLoop) {
        _workLoop->release();
        _workLoop = nullptr;
    }
    if (_compatPciDev) {
        IOFree(_compatPciDev, sizeof(*_compatPciDev));
        _compatPciDev = nullptr;
    }
    if (_mmioMap) {
        _mmioMap->release();
        _mmioMap = nullptr;
        _mmioBase = nullptr;
    }

    if (g_pci_dev_instance == this) {
        g_pci_dev_instance = nullptr;
        rtw88_pci_io_ops = nullptr;
        rtw88_dma_ops = nullptr;
    }

    if (_pciDev) {
        _pciDev->setBusMasterEnable(false);
        _pciDev->release();
        _pciDev = nullptr;
    }

    if (_pendingFreeLock) {
        IOSimpleLockFree(_pendingFreeLock);
        _pendingFreeLock = nullptr;
    }
    if (_dmaLock) {
        IOSimpleLockFree(_dmaLock);
        _dmaLock = nullptr;
    }
}

IOReturn AirportRTW88::registerWithPolicyMaker(IOService *policyMaker)
{
    if (!policyMaker)
        return kIOReturnBadArgument;

    _pmPowerState = kRTW88PowerStateOn;
    setProperty("PM_REGISTERED", kOSBooleanTrue);
    setProperty("PM_STATE", "on");
    setProperty("PM_HIBERNATION_SAFE_REINIT", kOSBooleanTrue);
    return policyMaker->registerPowerDriver(this, gRTW88PowerStates,
                                            kRTW88PowerStateCount);
}

IOReturn AirportRTW88::quiesceForSystemSleep()
{
    if (__atomic_exchange_n(&_pmTransition, true, __ATOMIC_ACQ_REL))
        return kIOReturnBusy;

    ++_pmSleepCount;
    setProperty("PM_STATE", "sleeping");
    setProperty("PM_SLEEP_COUNT", (uint64_t)_pmSleepCount, 32);

    _pmAWDLSyncWasEnabled = _awdlManager && _awdlManager->syncEnabled();
    if (_awdlManager)
        _awdlManager->suspendForPowerTransition();

    _pmRadioWasPowered = _ieee80211 && _ieee80211->isPowered();
    _txStalled = true;

    IOReturn radioRet = kIOReturnSuccess;
    if (_ieee80211 && _pmRadioWasPowered)
        radioRet = _ieee80211->suspendForSystemSleep();

    /* The Realtek core is now stopped and no longer owns descriptors.  Fence
     * PCI DMA only after rtw_core_stop()/rtw_pci_stop() synchronized the IRQ
     * bottom half.  This ordering is important for hibernate, where the PCI
     * function may lose all device state while kernel memory is restored. */
    IOReturn dmaRet = stopDMA();
    if (_pciDev)
        _pciDev->setMemoryEnable(false);

    if (preemption_enabled())
        drainPendingFree();

    _pmLastSleepResult = (radioRet != kIOReturnSuccess) ? radioRet : dmaRet;
    setProperty("PM_LAST_SLEEP_RESULT", (uint64_t)(uint32_t)_pmLastSleepResult, 32);
    setProperty("PM_RADIO_WAS_POWERED", _pmRadioWasPowered ? kOSBooleanTrue : kOSBooleanFalse);
    setProperty("PM_STATE", "off");
    __atomic_store_n(&_pmTransition, false, __ATOMIC_RELEASE);
    return _pmLastSleepResult;
}

IOReturn AirportRTW88::restoreAfterSystemWake()
{
    if (__atomic_exchange_n(&_pmTransition, true, __ATOMIC_ACQ_REL))
        return kIOReturnBusy;

    ++_pmWakeCount;
    setProperty("PM_STATE", "waking");
    setProperty("PM_WAKE_COUNT", (uint64_t)_pmWakeCount, 32);

    IOReturn ret = kIOReturnSuccess;
    if (!_pciDev || !_ieee80211 || !_mmioMap || !_mmioBase) {
        ret = kIOReturnNotReady;
        goto out;
    }

    /* Hibernation resumes a memory image, not the NIC.  Re-enable the PCI
     * command bits unconditionally and keep _dmaStopped asserted while the
     * firmware/rings are reconstructed.  This prevents a restored OS event
     * source from dispatching an interrupt into a half-initialized rtw88. */
    _pciDev->setMemoryEnable(true);
    _pciDev->setBusMasterEnable(true);
    {
        const UInt16 cmd = _pciDev->configRead16(kIOPCIConfigCommand);
        if ((cmd & 0x0006U) != 0x0006U) {
            IOLog("AirportRTW88: wake failed to restore PCI command bits (0x%04x)\n", cmd);
            ret = kIOReturnNotReady;
            goto out;
        }
    }

    /* Refresh the compat-visible BAR in case the platform firmware rewrote the
     * physical PCI BAR while preserving the IOMemoryMap virtual mapping. */
    if (_compatPciDev) {
        _compatPciDev->resource[2] = (resource_size_t)_mmioBase;
        _compatPciDev->resource_len[2] = (resource_size_t)_mmioMap->getLength();
    }

    if (_pmRadioWasPowered) {
        ret = _ieee80211->cmdPowerOn();
        if (ret != kIOReturnSuccess)
            goto out;
    }

    __atomic_store_n(&_dmaStopped, false, __ATOMIC_RELEASE);
    if (_intrSrc)
        _intrSrc->enable();

    _scanCacheBootstrapAttempted = false;
    _txStalled = false;
    /* Preserve the user-visible radio state across sleep.  If Wi-Fi was off
     * before suspend, do not restart AWDL discovery merely because the PCI
     * function returned to D0.  A later IO80211 enable() owns that restart. */
    if (_awdlManager && _pmRadioWasPowered) {
        if (_pmAWDLSyncWasEnabled)
            _awdlManager->resumeAfterPowerTransition();
        _awdlManager->scheduleDiscovery();
    }

    /* The infrastructure association is intentionally not resurrected from
     * hibernated RAM.  Firmware keys, RX PN state and AP sequence state are no
     * longer trustworthy after a cold PCI resume.  Report a valid-but-down
     * link so CoreWiFi performs a clean reassociation. */
    (void)setLinkStatus(kIONetworkLinkValid | kIONetworkLinkNoNetworkChange);
    if (_netif) {
        _netif->postMessage(APPLE80211_M_POWER_CHANGED);
        IOOutputQueue *q = getOutputQueue();
        if (q) q->service(IOBasicOutputQueue::kServiceAsync);
    }

out:
    if (ret != kIOReturnSuccess) {
        /* Never leave bus mastering enabled after a failed firmware restore. */
        __atomic_store_n(&_dmaStopped, true, __ATOMIC_RELEASE);
        if (_intrSrc) _intrSrc->disable();
        if (_pciDev) _pciDev->setBusMasterEnable(false);
    }
    _pmLastWakeResult = ret;
    setProperty("PM_LAST_WAKE_RESULT", (uint64_t)(uint32_t)ret, 32);
    setProperty("PM_STATE", ret == kIOReturnSuccess ? "on" : "wake-failed");
    __atomic_store_n(&_pmTransition, false, __ATOMIC_RELEASE);
    return ret;
}

IOReturn AirportRTW88::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    (void)whatDevice;
    if (powerStateOrdinal >= kRTW88PowerStateCount)
        return IOPMNoSuchState;
    if (powerStateOrdinal == _pmPowerState)
        return IOPMAckImplied;

    IOReturn ret = kIOReturnSuccess;
    if (powerStateOrdinal == kRTW88PowerStateOff)
        ret = quiesceForSystemSleep();
    else
        ret = restoreAfterSystemWake();

    /* Power management must always receive a synchronous acknowledgement; a
     * driver-level failure is exposed through IORegistry instead of hanging the
     * machine's sleep/wake transition. */
    _pmPowerState = powerStateOrdinal;
    if (ret != kIOReturnSuccess)
        IOLog("AirportRTW88: PM transition to state %lu failed 0x%x\n",
              powerStateOrdinal, ret);
    return IOPMAckImplied;
}

void AirportRTW88::stop(IOService *provider)
{
    /* AirportItlwm calls IO80211Controller::stop() before releasing its
     * workloop/HAL/interface state. Keep the same ordering here so superclass
     * teardown cannot observe a freed IO80211WorkLoop or Realtek backend. */
    if (_superStarted) {
        super::stop(provider);
        _superStarted = false;
    }
    teardown();
}

void AirportRTW88::free()
{
    teardown();
    super::free();
}

const OSString *AirportRTW88::newVendorString() const { return OSString::withCString("Apple"); }
const OSString *AirportRTW88::newModelString() const  { return OSString::withCString("802.11ac"); }

IOReturn AirportRTW88::enable(IONetworkInterface *iface)
{
    if (!_ieee80211 || __atomic_load_n(&_shutdown, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&_pmTransition, __ATOMIC_ACQUIRE) ||
        _pmPowerState == kRTW88PowerStateOff)
        return kIOReturnNotReady;
    if (__atomic_load_n(&_dmaStopped, __ATOMIC_ACQUIRE)) {
        if (!_pciDev) return kIOReturnNotReady;
        _pciDev->setBusMasterEnable(true);
        if (!(_pciDev->configRead16(kIOPCIConfigCommand) & 0x0004))
            return kIOReturnNotReady;
        __atomic_store_n(&_dmaStopped, false, __ATOMIC_RELEASE);
        if (_intrSrc) _intrSrc->enable();
        if (_awdlManager) _awdlManager->scheduleDiscovery();
    }
    _scanCacheBootstrapAttempted = false;
    /* Ventura can enable its queues without issuing POWER ON. Ensure the
     * hardware is running before handing interface state to IO80211. */
    IOReturn ret = _ieee80211->powerOn();
    if (ret != kIOReturnSuccess) return ret;
    ret = super::enable(iface);
    if (ret != kIOReturnSuccess) {
        _ieee80211->powerOff();
    } else {
        _txStalled = false;
        IOOutputQueue *q = getOutputQueue();
        if (q) q->service(IOBasicOutputQueue::kServiceAsync);
    }
    IOLog("AirportRTW88: IO80211 enable result=0x%x\n", ret);
    return ret;
}

IOReturn AirportRTW88::disable(IONetworkInterface *iface)
{
    /* Keep the IO80211 service published, but actually quiesce the Realtek
     * radio.  v51 left firmware/DMA association state alive across an IO80211
     * Wi-Fi power cycle; the next enable then reused stale state.  A complete
     * cmdPowerOff()/powerOn() pair is also the safe primitive for sleep/wake
     * on hardware that cannot reliably survive PCI D3hot with rtw88 state
     * retained. */
    IOReturn ret = super::disable(iface);
    _txStalled = false;
    IOReturn powerRet = kIOReturnSuccess;
    if (_ieee80211) powerRet = _ieee80211->cmdPowerOff();
    (void)setLinkStatus(kIONetworkLinkValid | kIONetworkLinkNoNetworkChange);
    IOLog("AirportRTW88: IO80211 disable result=0x%x radio-off=0x%x\n", ret, powerRet);
    if (ret == kIOReturnSuccess && powerRet != kIOReturnSuccess)
        return powerRet;
    return ret;
}

IOReturn AirportRTW88::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    if (!group || !filters) return kIOReturnBadArgument;
    if (group == gIONetworkFilterGroup) {
        /* AirportItlwm v2.2.0 publishes these two filters to IO80211. */
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
        return kIOReturnSuccess;
    }
    return IOEthernetController::getPacketFilters(group, filters);
}

IOReturn AirportRTW88::setMulticastMode(bool active)
{
    IOReturn ret = _ieee80211 ? _ieee80211->setReceiveMulticast(active) : kIOReturnNotReady;
    IOLog("AirportRTW88: multicast active=%d result=0x%x\n", active, ret);
    return ret;
}

IOReturn AirportRTW88::setMulticastList(IOEthernetAddress *addrs, UInt32 count)
{
    if (count && !addrs) return kIOReturnBadArgument;
    /* rtw88 configure_filter supports all-multicast, not an address table.
     * Use that documented fallback; the network stack handles membership. */
    return setMulticastMode(count != 0);
}

IOReturn AirportRTW88::setPromiscuousMode(bool active)
{
    /* Match AirportItlwm's IO80211 contract.  rtw88's normal STA receive
     * filter remains authoritative; this callback must not abort IO80211 setup. */
    (void)active;
    return kIOReturnSuccess;
}

bool AirportRTW88::createMediumTables(const IONetworkMedium **primary)
{
    /* AirportItlwm Ventura publishes the medium dictionary before
     * attachInterface().  Keep the same IO80211 lifecycle so the interface
     * is born with a selected/current medium already available. */
    OSDictionary *mediumDict = OSDictionary::withCapacity(1);
    if (!mediumDict)
        return false;

    IONetworkMedium *medium = IONetworkMedium::medium(0x80, 11000000);
    if (!medium) {
        mediumDict->release();
        return false;
    }

    bool ok = IONetworkMedium::addMedium(mediumDict, medium);
    if (ok && primary)
        *primary = medium;
    if (ok)
        ok = publishMediumDictionary(mediumDict);

    medium->release();
    mediumDict->release();
    return ok;
}

IONetworkInterface *AirportRTW88::createInterface()
{
    IOLog("AirportRTW88: createInterface entered (controller workloop=%p)\n",
          getWorkLoop());
    AirportRTW88Interface *interface = OSTypeAlloc(AirportRTW88Interface);
    if (!interface) {
        IOLog("AirportRTW88: createInterface allocation failed\n");
        return nullptr;
    }
    if (!interface->init(this)) {
        IOLog("AirportRTW88: createInterface IO80211Interface::init failed\n");
        interface->release();
        return nullptr;
    }
    IOLog("AirportRTW88: createInterface initialized\n");
    return interface;
}

bool AirportRTW88::configureInterface(IONetworkInterface *iface)
{
    /* Match AirportItlwm Ventura: IO80211/IONetworkController performs the
     * interface configuration.  The medium table is published in start()
     * before attachInterface(), not created during the attach callback. */
    bool ok = super::configureInterface(iface);
    IOLog("AirportRTW88: configureInterface super=%d\n", ok);
    return ok;
}

IOReturn AirportRTW88::selectMedium(const IONetworkMedium *medium)
{
    if (!medium)
        return kIOReturnBadArgument;
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

UInt32 AirportRTW88::getFeatures() const
{
    /* Preserve IONetworkController feature flags. IO80211-specific 802.11n
     * negotiation is handled by enableFeature(), as in AirportItlwm. */
    return super::getFeatures();
}

UInt32 AirportRTW88::outputPacket(mbuf_t m, void *param)
{
    (void)param;
    if (__atomic_load_n(&_pmTransition, __ATOMIC_ACQUIRE) ||
        _pmPowerState == kRTW88PowerStateOff) {
        _txStalled = true;
        return kIOReturnOutputStall;
    }
    if (__atomic_load_n(&_dmaStopped, __ATOMIC_ACQUIRE) || !_ieee80211) {
        if (m) mbuf_freem(m);
        return kIOReturnOutputDropped;
    }

    if (preemption_enabled())
        drainPendingFree();

    /* During a connected off-channel AWDL window the single PHY is not on
     * the AP channel. Backpressure en0 instead of transmitting AP traffic on
     * the peer-to-peer channel. */
    if (_ieee80211->staTxBlockedByAWDL()) {
        _txStalled = true;
        return kIOReturnOutputStall;
    }

    const unsigned int avail = rtw88_be_tx_avail();
    if (_txStalled || avail < kRTW88TxStallAvail) {
        _txStalled = true;
        /* Do not consume m. IOBasicOutputQueue retries this exact packet when
         * resumeTxIfStalled() services the queue after descriptor reclamation. */
        return kIOReturnOutputStall;
    }

    return _ieee80211->outputPacket(m);
}

IOReturn AirportRTW88::getHardwareAddress(IOEthernetAddress *addr)
{
    if (!addr || !_ieee80211) return kIOReturnNotReady;
    _ieee80211->getMACAddress(addr->bytes);
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::setHardwareAddress(const IOEthernetAddress *addr)
{
    if (!addr || !_ieee80211)
        return kIOReturnBadArgument;

    /* Newer CoreWiFi asks the controller to adopt its per-network private
     * station address before it submits APPLE80211_IOC_ASSOCIATE.  The default
     * IOEthernetController implementation returns Unsupported; that aborts the
     * join before the Realtek association path is reached. */
    IOReturn ret = _ieee80211->setMACAddress(addr->bytes);
    if (ret == kIOReturnSuccess) {
        memcpy(_macAddr.bytes, addr->bytes, sizeof(_macAddr.bytes));
        IOLog("AirPort_RTW88: station MAC updated to %02x:%02x:%02x:%02x:%02x:%02x\n",
              addr->bytes[0], addr->bytes[1], addr->bytes[2],
              addr->bytes[3], addr->bytes[4], addr->bytes[5]);
    } else {
        IOLog("AirPort_RTW88: station MAC update failed ret=0x%x\n", ret);
    }
    return ret;
}

IOReturn AirportRTW88::getHardwareAddressForInterface(IO80211Interface *iface,
                                                       IOEthernetAddress *addr)
{
    (void)iface;
    /* AirportItlwm maps the infrastructure-interface query to the physical
     * controller address.  Virtual interfaces get their own address through
     * IO80211's attachVirtualInterface lifecycle. */
    return getHardwareAddress(addr);
}

/*
 * Ventura IO80211FamilyLegacy transport compatibility.
 *
 * AirportItlwm Ventura implements apple80211Request() and leaves association/
 * security translation to IO80211FamilyLegacy. Preserve that path except for
 * scan and the validated 900/908-byte association prefix on newer hosts using
 * the restored Legacy/Skywalk stack.
 */
namespace {
static bool rtw88TryHostAssociation(AirportRTW88 *self, IO80211Interface *interface,
                                    apple80211req *req, SInt32 *result)
{
    if (!req->req_data || !RTW88AssocWire::supportedRequestSize(req->req_len)) return false;
    uint8_t prefix[RTW88AssocWire::PrefixSize] = {};
    RTW88AssocWire::Personal fields = {};
    const int error = copyin((user_addr_t)req->req_data, prefix, sizeof(prefix));
    const bool valid = !error && RTW88AssocWire::decode(prefix, sizeof(prefix), fields);
    RTW88AssocWire::wipe(prefix, sizeof(prefix));
    if (!valid) { RTW88AssocWire::wipe(&fields, sizeof(fields)); return false; }
    apple80211_assoc_data assoc = {};
    assoc.version = APPLE80211_VERSION;
    assoc.ad_mode = fields.mode; assoc.ad_auth_lower = fields.lower; assoc.ad_auth_upper = fields.upper;
    assoc.ad_ssid_len = fields.ssidLength;
    memcpy(assoc.ad_ssid, fields.ssid, fields.ssidLength);
    memcpy(assoc.ad_bssid.octet, fields.bssid, sizeof(fields.bssid));
    assoc.ad_key.version = APPLE80211_VERSION;
    assoc.ad_key.key_len = fields.keyLength; assoc.ad_key.key_cipher_type = fields.cipher;
    memcpy(assoc.ad_key.key, fields.key, fields.keyLength);
    RTW88AssocWire::wipe(&fields, sizeof(fields));
    self->setProperty("STA_ASSOC_HOST32_TRANSLATED", kOSBooleanTrue);
    *result = self->apple80211Request(SIOCSA80211, APPLE80211_IOC_ASSOCIATE, interface, &assoc);
    RTW88AssocWire::wipe(&assoc, sizeof(assoc));
    if (*result < 0 || *result >= 256) {
        const int e = self->errnoFromReturn(*result); *result = e ? e : EIO;
    }
    return true;
}
}

namespace {
static bool rtw88TryAssocFromSplitWrapper(AirportRTW88 *, IO80211Interface *, void *, SInt32 *);
}
SInt32 AirportRTW88::apple80211_ioctl(IO80211Interface *interface,
                                      IO80211VirtualInterface *virtualInterface,
                                      ifnet_t net, unsigned long cmd,
                                      void *data)
{
    /* Ventura is the native ABI for this kext. */
    if (version_major < 23) {
        SInt32 sr = super::apple80211_ioctl(interface, virtualInterface, net, cmd, data);
        return sr;
    }

    if (!data || (cmd != SIOCGA80211 && cmd != SIOCSA80211)) {
        SInt32 sr = super::apple80211_ioctl(interface, virtualInterface, net, cmd, data);
        return sr;
    }

    auto *req = static_cast<struct apple80211req *>(data);
    if (!req) {
        return EINVAL;
    }

    const bool isGet = (cmd == SIOCGA80211);
    const bool isSet = (cmd == SIOCSA80211);
    /* v52: snapshot the BSD wrapper before handing the same pointer to
     * IO80211FamilyLegacy.  Do not re-read wrapper fields after super returns;
     * issue #4 showed this path executing around a power-cycle corruption. */
    const uint32_t reqType = req->req_type;
    const uint32_t reqVal = req->req_val;
    const uint32_t len = req->req_len;
    const user_addr_t reqData = (user_addr_t)req->req_data;

    /* BSD buffers are NOT generally apple80211Request() structures. The
     * Ventura family strips version from CARD_CAPABILITIES (16 -> 12 bytes),
     * strips version/length from SSID (40 -> <=32 bytes), and uses req_val
     * for scalar requests. Passing caller-sized buffers directly to typed
     * handlers both corrupts the wire format and can overrun an allocation.
     * Keep native marshalling for all but the proven STA scan/powersave
     * exceptions. In particular virtual-interface/AWDL requests must retain
     * their original interface routing through the family.
     */
    if (isSet && reqType == APPLE80211_IOC_ASSOCIATE)
        setProperty("STA_ASSOC_BSD_LEN", (uint64_t)len, 32);
    // Normalize the host format before the legacy family truncates/casts it.
    if (isSet && reqType == APPLE80211_IOC_ASSOCIATE && !virtualInterface &&
        interface && interface == _netif && RTW88AssocWire::supportedRequestSize(len)) {
        SInt32 ret = 0;
        if (rtw88TryHostAssociation(this, interface, req, &ret)) return ret;
    }
    const bool scanBridge = !virtualInterface &&
        ((isGet && reqType == APPLE80211_IOC_SCAN_RESULT) ||
         (isSet && (reqType == APPLE80211_IOC_SCAN_REQ ||
                    reqType == APPLE80211_IOC_SCAN_REQ_MULTIPLE)));
    const bool scalarPowerSave = !virtualInterface && isSet && len == 0 &&
        reqType == APPLE80211_IOC_POWERSAVE;
    const bool scalarDisassociate = !virtualInterface && isSet && len == 0 &&
        reqType == APPLE80211_IOC_DISASSOCIATE;
    const bool channelsBridge = !virtualInterface && isGet &&
        reqType == APPLE80211_IOC_CHANNELS_INFO;
    if (!scanBridge && !scalarPowerSave && !scalarDisassociate && !channelsBridge) {
        const UInt32 before = __atomic_load_n(&_nativeAssocDispatchCount, __ATOMIC_RELAXED);
        SInt32 sr = super::apple80211_ioctl(interface, virtualInterface, net, cmd, data);
        if (virtualInterface && sr != 0) {
            setProperty("AWDL_BSD_ERROR_SELECTOR", (uint64_t)(uint32_t)reqType, 32);
            setProperty("AWDL_BSD_ERROR_LENGTH", (uint64_t)len, 32);
            setProperty("AWDL_BSD_ERROR_SET", isSet ? kOSBooleanTrue : kOSBooleanFalse);
            setProperty("AWDL_BSD_ERROR", (uint64_t)(uint32_t)sr, 32);
        }
        // A family-level rejection may occur before the split SET override.
        // Retry only a validated STA ASSOCIATE that never reached our handler.
        // Darwin ENOTSUP=45 and EOPNOTSUPP=102; Linux macros differ here.
        if (isSet && reqType == APPLE80211_IOC_ASSOCIATE && !virtualInterface &&
            interface && interface == _netif && (sr == 45 || sr == 102) &&
            __atomic_load_n(&_nativeAssocDispatchCount, __ATOMIC_RELAXED) == before) {
            SInt32 ret = 0;
            if (rtw88TryAssocFromSplitWrapper(this, interface, data, &ret)) {
                setProperty("STA_ASSOC_OUTER_FALLBACK", kOSBooleanTrue);
                if (ret == kIOReturnSuccess || (ret > 0 && ret < 256)) sr = ret;
                else { sr = errnoFromReturn(ret); if (!sr) sr = EIO; }
            }
        }
        setProperty("STA_V23_NATIVE_SELECTOR", (uint64_t)reqType, 32);
        setProperty("STA_V23_NATIVE_RET", (uint64_t)(uint32_t)sr, 32);
        if (sr != 0) {
            setProperty("STA_V23_NATIVE_ERROR_SELECTOR", (uint64_t)reqType, 32);
            setProperty("STA_V23_NATIVE_ERROR_RET", (uint64_t)(uint32_t)sr, 32);
            setProperty("STA_V23_NATIVE_ERROR_IS_SET", isSet ? kOSBooleanTrue : kOSBooleanFalse);
            if (isSet) {
                setProperty("STA_V23_NATIVE_SET_ERROR_SELECTOR", (uint64_t)reqType, 32);
                setProperty("STA_V23_NATIVE_SET_ERROR_RET", (uint64_t)(uint32_t)sr, 32);
            }
        }
        return sr;
    }

    /* A malformed userspace length must never become a giant kernel allocation. */
    if (len > 64U * 1024U) {
        return EINVAL;
    }

    /* Some Apple80211 SETs carry their value in req_val and intentionally
     * have no payload.  Modern CoreWiFi uses this for POWERSAVE=
     * MAX_THROUGHPUT (7) before association.  The restored Ventura family
     * otherwise returns Darwin ENOTSUP and aborts the join transaction. */
    if (len == 0) {
        if (isSet && reqType == APPLE80211_IOC_POWERSAVE) {
            if (reqVal < APPLE80211_POWERSAVE_MODE_DISABLED ||
                reqVal > APPLE80211_POWERSAVE_MODE_MAX_POWERSAVE)
                return EINVAL;
            _powerSaveLevel = (UInt32)reqVal;
            setProperty("STA_V20_POWERSAVE_SET", kOSBooleanTrue);
            setProperty("STA_V20_POWERSAVE_VALUE", (uint64_t)_powerSaveLevel, 32);
            IOLog("AirPort_RTW88: v20 zero-len POWERSAVE SET value=%u accepted\n",
                  (unsigned)_powerSaveLevel);
            return 0;
        }
        if (reqType != APPLE80211_IOC_DISASSOCIATE) {
            SInt32 sr = super::apple80211_ioctl(interface, virtualInterface, net, cmd, data);
            return sr;
        }
        SInt32 r = apple80211Request(cmd, reqType, interface, nullptr);
        if (r == kIOReturnSuccess)
            return 0;
        if (r > 0 && r < 256)
            return r;
        int e = errnoFromReturn(r);
        return e ? e : EIO;
    }

    if (!(void *)reqData) {
        return EFAULT;
    }

    /* The restored family rejects the host CHANNELS_INFO layout with EINVAL
     * before reaching our handler. Retain this v21 scan-support exception,
     * but never give a caller-sized allocation to the typed handler.
     */
    if (channelsBridge) {
        setProperty("STA_V24_CHANNELS_CALLER_LEN", (uint64_t)len, 32);
        if (len < sizeof(apple80211_channels_info)) return EINVAL;
        void *out = IOMallocZero(len);
        if (!out) return ENOMEM;
        const SInt32 r = apple80211Request(cmd, reqType, interface, out);
        int e = 0;
        if (r == kIOReturnSuccess)
            e = copyout(out, reqData, len);
        else if (r > 0 && r < 256) e = r;
        else { e = errnoFromReturn(r); if (!e) e = EIO; }
        IOFree(out, len);
        setProperty("STA_V24_CHANNELS_BRIDGE_RET", (uint64_t)(uint32_t)e, 32);
        return e;
    }

    /* SCAN_RESULT is the only legacy Apple80211 request whose controller
     * callback uses a pointer-to-pointer result.  Handle it explicitly rather
     * than feeding a userspace-sized buffer to a Ventura structure. */
    if (isGet && reqType == APPLE80211_IOC_SCAN_RESULT) {
        struct apple80211_scan_result *result = nullptr;
        SInt32 r = apple80211Request(cmd, reqType, interface, &result);
        if (r != kIOReturnSuccess) {
            if (r > 0 && r < 256)
                return r;
            int e = errnoFromReturn(r);
            return e ? e : EIO;
        }
        if (!result)
            return ENOENT;

        void *out = IOMallocZero(len);
        if (!out)
            return ENOMEM;
        const size_t known = sizeof(struct apple80211_scan_result);
        const size_t n = len < known ? len : known;
        memcpy(out, result, n);
        const int ce = copyout(out, reqData, len);
        IOFree(out, len);
        return ce ? ce : 0;
    }

    /* Only scan SETs remain here. Require the entire structure before its
     * fields are accessed by the typed handler; never dispatch a short buffer.
     */
    const size_t scanSize = reqType == APPLE80211_IOC_SCAN_REQ ?
        sizeof(apple80211_scan_data) : sizeof(apple80211_scan_multiple_data);
    if (!isSet || !scanBridge || len < scanSize)
        return EINVAL;

    void *payload = IOMallocZero(len);
    if (!payload)
        return ENOMEM;

    if (isSet) {
        const int ce = copyin(reqData, payload, len);
        if (ce) {
            IOFree(payload, len);
            return ce;
        }
    }

    SInt32 r = apple80211Request(cmd, reqType, interface, payload);

    if (r == kIOReturnSuccess && isGet) {
        const int ce = copyout(payload, reqData, len);
        IOFree(payload, len);
        return ce ? ce : 0;
    }

    IOFree(payload, len);
    if (r == kIOReturnSuccess)
        return 0;
    if (r > 0 && r < 256)
        return r;

    const int e = errnoFromReturn(r);
    return e ? e : EIO;
}


#if __IO80211_TARGET >= __MAC_10_15
namespace {
/* Do not compare Apple IO80211 BSD errno returns against the Linux compat
 * ENOTSUP macro.  src/compat/linux/kernel.h intentionally remaps ENOTSUP to
 * Linux EOPNOTSUPP (95), while Darwin ENOTSUP is 45.  The split IO80211
 * methods return BSD/Darwin errno values, as confirmed on hardware. */
static constexpr SInt32 kRTW88DarwinENOTSUP = 45;

struct RTW88SplitTraceSnapshot {
    uint32_t words[10]; /* first 40 bytes: enough to identify apple80211req on x86_64 */
};

static void rtw88CaptureSplitTrace(const void *data, RTW88SplitTraceSnapshot *out)
{
    /* v52 safety hardening: the split IO80211 ingress does not provide a
     * payload length.  v51 copied 40 bytes unconditionally from an opaque
     * pointer purely for diagnostics.  That read can cross the actual object
     * boundary and is never required for request handling.  Keep the trace
     * structure zeroed instead of speculatively touching unknown memory. */
    (void)data;
    bzero(out, sizeof(*out));
}

static void rtw88PublishSplitTrace(AirportRTW88 *self,
                                   const RTW88SplitTraceSnapshot &snap,
                                   uint32_t path, SInt32 superRet)
{
    if (!self)
        return;

    /* Keep four unsupported calls rather than only the last one.  v16 proved
     * that this entry point is real, but not that every call here is ASSOCIATE.
     * These diagnostics are deliberately observation-only: no dispatch and no
     * mutation of the request payload. */
    static volatile UInt32 traceSequence = 0;
    const UInt32 seq = __atomic_add_fetch(&traceSequence, 1U, __ATOMIC_RELAXED);
    const UInt32 slot = (seq - 1U) & 3U;

    char key[64];
#define RTW88_TRACE_U32(suffix, value) do { \
        snprintf(key, sizeof(key), "STA_V17_TRACE%u_%s", slot, suffix); \
        self->setProperty(key, (uint64_t)(uint32_t)(value), 32); \
    } while (0)

    RTW88_TRACE_U32("SEQ", seq);
    RTW88_TRACE_U32("PATH", path); /* 1=iface+vif+skywalk, 2=skywalk-only */
    RTW88_TRACE_U32("SUPER_RET", superRet);

    /* apple80211req candidate fields if this really is the BSD wrapper. */
    RTW88_TRACE_U32("REQ_TYPE", snap.words[4]); /* offset 16 */
    RTW88_TRACE_U32("REQ_VAL",  snap.words[5]); /* offset 20 */
    RTW88_TRACE_U32("REQ_LEN",  snap.words[6]); /* offset 24 */

    /* Direct apple80211_assoc_data candidate fields.  RAW0..3 map to:
     * version; mode/auth-lower; auth-upper(+pad); ssid_len when direct. */
    RTW88_TRACE_U32("RAW0", snap.words[0]);
    RTW88_TRACE_U32("RAW1", snap.words[1]);
    RTW88_TRACE_U32("RAW2", snap.words[2]);
    RTW88_TRACE_U32("RAW3", snap.words[3]);
    RTW88_TRACE_U32("RAW7", snap.words[7]);
    RTW88_TRACE_U32("RAW8", snap.words[8]);
    RTW88_TRACE_U32("RAW9", snap.words[9]);
#undef RTW88_TRACE_U32
}

static bool rtw88AssocPayloadLooksValid(const apple80211_assoc_data *d)
{
    if (!d || d->version != APPLE80211_VERSION)
        return false;
    if (d->ad_mode != APPLE80211_AP_MODE_UNKNOWN &&
        d->ad_mode != APPLE80211_AP_MODE_INFRA &&
        d->ad_mode != APPLE80211_AP_MODE_ANY)
        return false;
    if (!d->ad_ssid_len || d->ad_ssid_len > APPLE80211_MAX_SSID_LEN)
        return false;

    bool nonzero = false;
    for (uint32_t i = 0; i < d->ad_ssid_len; ++i) {
        if (d->ad_ssid[i] != 0) nonzero = true;
        /* SSIDs are arbitrary bytes, but reject embedded NUL here because the
         * current backend join API is NUL-terminated and handleASSOCIATE does
         * the same validation. */
        if (d->ad_ssid[i] == 0) return false;
    }
    if (!nonzero) return false;

    const uint32_t personal = APPLE80211_AUTHTYPE_WPA_PSK |
                              APPLE80211_AUTHTYPE_WPA2_PSK |
                              APPLE80211_AUTHTYPE_SHA256_PSK;
    const bool secure = (d->ad_auth_upper & personal) != 0;
    if (d->ad_auth_lower != APPLE80211_AUTHTYPE_OPEN)
        return false;
    if (d->ad_auth_upper != APPLE80211_AUTHTYPE_NONE && !secure)
        return false;
    if (secure && d->ad_key.key_len != 32)
        return false;
    if (!secure && d->ad_key.key_len != 0)
        return false;

    if (secure && d->ad_rsn_ie[0] != 0) {
        if (d->ad_rsn_ie[0] != 48)
            return false;
        const uint16_t n = (uint16_t)d->ad_rsn_ie[1] + 2U;
        if (n < 2 || n > APPLE80211_MAX_RSN_IE_LEN)
            return false;
    }
    return true;
}

static bool rtw88TryAssocFromSplitWrapper(AirportRTW88 *self,
                                           IO80211Interface *interface,
                                           void *data,
                                           SInt32 *result)
{
    if (!self || !data || !result)
        return false;

    auto *req = static_cast<apple80211req *>(data);
    if (req->req_type != APPLE80211_IOC_ASSOCIATE ||
        req->req_len < sizeof(apple80211_assoc_data) ||
        req->req_len > 64U * 1024U || !req->req_data)
        return false;

    apple80211_assoc_data assoc = {};
    const int ce = copyin((user_addr_t)req->req_data, &assoc, sizeof(assoc));
    if (ce != 0 || !rtw88AssocPayloadLooksValid(&assoc)) {
        self->setProperty("STA_SPLIT_ASSOC_WRAPPER_COPYIN_ERR", (uint64_t)(uint32_t)ce, 32);
        return false;
    }

    self->setProperty("STA_SPLIT_ASSOC_WRAPPER", kOSBooleanTrue);
    self->setProperty("STA_SPLIT_ASSOC_LEN", (uint64_t)req->req_len, 32);
    *result = self->apple80211Request(SIOCSA80211, APPLE80211_IOC_ASSOCIATE,
                                      interface, &assoc);
    self->setProperty("STA_SPLIT_ASSOC_FALLBACK_RET", (uint64_t)(uint32_t)*result, 32);
    bzero(&assoc, sizeof(assoc));
    return true;
}
}

SInt32 AirportRTW88::apple80211_ioctl_set(IO80211Interface *interface,
                                           IO80211VirtualInterface *virtualInterface,
                                           IO80211SkywalkInterface *skywalkInterface,
                                           void *data)
{
    setProperty("STA_SPLIT_ASSOC_INGRESS", kOSBooleanTrue);
    RTW88SplitTraceSnapshot splitPre = {};
    rtw88CaptureSplitTrace(data, &splitPre);
    const UInt32 before = __atomic_load_n(&_nativeAssocDispatchCount, __ATOMIC_RELAXED);

    /* v19's hardware trace finally identified the opaque split SET exactly:
     * req_if_name="en0", req_type=APPLE80211_IOC_POWERSAVE (5),
     * req_val=APPLE80211_POWERSAVE_MODE_MAX_THROUGHPUT (7), req_len=0.
     * This is a real zero-payload Apple80211 request, not ASSOCIATE.  Handle
     * POWERSAVE by selector/value instead of blanket-ACKing an unknown SET. */
    if (data && interface && interface == _netif && !virtualInterface) {
        auto *req = static_cast<apple80211req *>(data);
        if (req->req_type == APPLE80211_IOC_POWERSAVE && req->req_len == 0) {
            if (req->req_val >= APPLE80211_POWERSAVE_MODE_DISABLED &&
                req->req_val <= APPLE80211_POWERSAVE_MODE_MAX_POWERSAVE) {
                _powerSaveLevel = (UInt32)req->req_val;
                static volatile UInt32 powerSaveSetCount = 0;
                const UInt32 n = __atomic_add_fetch(&powerSaveSetCount, 1U, __ATOMIC_RELAXED);
                setProperty("STA_V20_POWERSAVE_SET", kOSBooleanTrue);
                setProperty("STA_V20_POWERSAVE_VALUE", (uint64_t)_powerSaveLevel, 32);
                setProperty("STA_V20_POWERSAVE_SET_COUNT", (uint64_t)n, 32);
                IOLog("AirPort_RTW88: v20 split POWERSAVE SET value=%u accepted (count=%u)\n",
                      (unsigned)_powerSaveLevel, (unsigned)n);
                return 0;
            }
        }
    }

    /* Let the restored Ventura family do the normal translation first.  This
     * preserves every working scan/Instant-Hotspot/discovery behavior from v15. */
    SInt32 sr = super::apple80211_ioctl_set(interface, virtualInterface,
                                            skywalkInterface, data);
    setProperty("STA_SPLIT_ASSOC_SUPER_RET", (uint64_t)(uint32_t)sr, 32);
    if (sr == kRTW88DarwinENOTSUP)
        rtw88PublishSplitTrace(this, splitPre, 1U, sr);

    const UInt32 after = __atomic_load_n(&_nativeAssocDispatchCount, __ATOMIC_RELAXED);
    if (after != before) {
        setProperty("STA_SPLIT_ASSOC_NATIVE_DISPATCH", kOSBooleanTrue);
        return sr;
    }

    /* Some restored-family builds hand the already-decoded Ventura ASSOCIATE
     * payload to this split hook.  Recognize it narrowly; never guess scan or
     * unrelated SET payloads. */
    if (rtw88AssocPayloadLooksValid(static_cast<apple80211_assoc_data *>(data))) {
        setProperty("STA_SPLIT_ASSOC_DIRECT", kOSBooleanTrue);
        SInt32 r = apple80211Request(SIOCSA80211, APPLE80211_IOC_ASSOCIATE,
                                     interface ? interface : _netif, data);
        setProperty("STA_SPLIT_ASSOC_FALLBACK_RET", (uint64_t)(uint32_t)r, 32);
        return r;
    }

    /* Other Ventura Legacy builds keep an apple80211req wrapper here. */
    SInt32 fallback = kIOReturnUnsupported;
    if (rtw88TryAssocFromSplitWrapper(this, interface ? interface : _netif,
                                      data, &fallback))
        return fallback;

    setProperty("STA_SPLIT_ASSOC_UNDECODED", kOSBooleanTrue);

    /* v20 no longer blanket-ACKs arbitrary ENOTSUP SETs.  The one observed on
     * hardware was decoded as POWERSAVE and is handled above.  If another
     * unsupported request appears, preserve the trace and return it unchanged
     * so the next blocker is visible instead of silently masking it. */
    if (sr == kRTW88DarwinENOTSUP && interface && interface == _netif && !virtualInterface) {
        setProperty("STA_V20_NEXT_ENOTSUP_VISIBLE", kOSBooleanTrue);
    }
    return sr;
}

SInt32 AirportRTW88::apple80211_ioctl_set(IO80211SkywalkInterface *skywalkInterface,
                                           void *data)
{
    setProperty("STA_SPLIT_ASSOC_SKYWALK_INGRESS", kOSBooleanTrue);
    RTW88SplitTraceSnapshot splitPre = {};
    rtw88CaptureSplitTrace(data, &splitPre);
    const UInt32 before = __atomic_load_n(&_nativeAssocDispatchCount, __ATOMIC_RELAXED);
    SInt32 sr = super::apple80211_ioctl_set(skywalkInterface, data);
    setProperty("STA_SPLIT_ASSOC_SKYWALK_SUPER_RET", (uint64_t)(uint32_t)sr, 32);
    if (sr == kRTW88DarwinENOTSUP)
        rtw88PublishSplitTrace(this, splitPre, 2U, sr);
    const UInt32 after = __atomic_load_n(&_nativeAssocDispatchCount, __ATOMIC_RELAXED);
    if (after != before) {
        setProperty("STA_SPLIT_ASSOC_NATIVE_DISPATCH", kOSBooleanTrue);
        return sr;
    }

    if (rtw88AssocPayloadLooksValid(static_cast<apple80211_assoc_data *>(data))) {
        setProperty("STA_SPLIT_ASSOC_DIRECT", kOSBooleanTrue);
        SInt32 r = apple80211Request(SIOCSA80211, APPLE80211_IOC_ASSOCIATE,
                                     _netif, data);
        setProperty("STA_SPLIT_ASSOC_FALLBACK_RET", (uint64_t)(uint32_t)r, 32);
        return r;
    }

    SInt32 fallback = kIOReturnUnsupported;
    if (rtw88TryAssocFromSplitWrapper(this, _netif, data, &fallback))
        return fallback;

    setProperty("STA_SPLIT_ASSOC_UNDECODED", kOSBooleanTrue);
    return sr;
}
#endif

/* Native IO80211 request payloads; compared against AirportItlwm v2.2.0
 * Ventura's apple80211Request dispatcher.  The BSD wrapper above is only a
 * transport fallback when the restored family does not dispatch a request. */
SInt32 AirportRTW88::apple80211Request(unsigned int request_type,
                                        int request_number,
                                        IO80211Interface *interface,
                                        void *data)
{
    setProperty("NATIVE_REQUEST_SEEN", kOSBooleanTrue);
    setProperty("NATIVE_REQUEST_TYPE", (uint64_t)request_type, 64);
    setProperty("NATIVE_REQUEST_SELECTOR", (uint64_t)(uint32_t)request_number, 32);
    if (request_number == APPLE80211_IOC_ASSOCIATE) {
        setProperty("NATIVE_ASSOCIATE_SEEN", kOSBooleanTrue);
        __atomic_add_fetch(&_nativeAssocDispatchCount, 1U, __ATOMIC_RELAXED);
    }
    kprintf("AirPort_RTW88: apple80211Request type=0x%x selector=%d data=%p\n",
            request_type, request_number, data);

    if (!_ieee80211) return kIOReturnNotReady;
    if (request_type != SIOCGA80211 && request_type != SIOCSA80211)
        return kIOReturnBadArgument;
    bool isSet = (request_type == SIOCSA80211);
    if (!data && request_number != APPLE80211_IOC_DISASSOCIATE)
        return kIOReturnBadArgument;
    IOLog("AirportRTW88: IOCTL %s selector=%d\n", isSet ? "SET" : "GET", request_number);

    // COUNTRY_CODE_CHANGED synchronously requests COUNTRY_CODE again.
    // Keep both invocations outside the large general-purpose switch frame.
    if (request_number == APPLE80211_IOC_COUNTRY_CODE)
        return handleRequestCOUNTRY_CODE(isSet, data);
    const SInt32 ret = handleNativeRequest(request_type, request_number, interface, data);
    if (request_number == APPLE80211_IOC_ASSOCIATE)
        setProperty("STA_ASSOC_NATIVE_RET", (uint64_t)(uint32_t)ret, 32);
    return ret;
}

__attribute__((__noinline__)) SInt32 AirportRTW88::handleNativeRequest(
    unsigned int request_type, int request_number, IO80211Interface *interface, void *data)
{
    const bool isSet = request_type == SIOCSA80211;
    switch (request_number) {
    case APPLE80211_IOC_SSID:
        return handleSSID(isSet, (struct apple80211_ssid_data *)data);

    case APPLE80211_IOC_AUTH_TYPE:
        return handleAUTH_TYPE(isSet, (struct apple80211_authtype_data *)data);

    case APPLE80211_IOC_ASSOCIATE:
        return isSet ? handleASSOCIATE((struct apple80211_assoc_data *)data) : kIOReturnUnsupported;

    case APPLE80211_IOC_RSN_IE:
        return handleRSN_IE(isSet, (struct apple80211_rsn_ie_data *)data);

    case APPLE80211_IOC_AP_IE_LIST:
        return isSet ? kIOReturnUnsupported : handleAP_IE_LIST((struct apple80211_ap_ie_data *)data);

    case APPLE80211_IOC_CIPHER_KEY:
        return isSet ? handleCIPHER_KEY((struct apple80211_key *)data) : kIOReturnUnsupported;

    case APPLE80211_IOC_SCAN_REQ:
        if (!isSet) return kIOReturnUnsupported;
        return handleSCAN_REQ(data);

    case APPLE80211_IOC_SCAN_REQ_MULTIPLE:
        /* Ventura may use the multi-SSID scan request even when one network is
         * requested.  The Realtek backend scans the full channel set, so the
         * request filters are advisory for now. */
        if (!isSet) return kIOReturnUnsupported;
        /* CoreWiFi can submit another scan while the hardware scan is still
         * running.  Do not surface EBUSY: keep the active scan and let its
         * SCAN_DONE satisfy the coalesced request. */
        if (_scanInProgress) {
            IOLog("AirPort_RTW88: SCAN_REQ_MULTIPLE coalesced with active scan\n");
            return kIOReturnSuccess;
        }
        {
            RTW88StateResult st = {};
            if (_ieee80211->cmdGetState(&st) == kIOReturnSuccess &&
                st.state == RTW88_STATE_CONNECTED) {
                /* Do not channel-hop a live STA.  The manual backend scan
                 * currently scans the complete channel table rather than the
                 * CoreWiFi-requested subset, which can keep us off-channel
                 * long enough for the AP to drop the association.  Satisfy
                 * connected background scans from the already-maintained BSS
                 * cache; disconnected scans still perform real RF scanning. */
                _scanCursor = 0;
                UInt32 result = 0;
                IOLog("AirPort_RTW88: SCAN_REQ_MULTIPLE connected cache-only completion\n");
                _netif->postMessage(APPLE80211_M_SCAN_DONE, &result, sizeof(result));
                return kIOReturnSuccess;
            }
        }
        _scanInProgress = true;
        _scanCursor = 0;
        {
            IOReturn ret = _ieee80211->cmdScan();
            if (ret) _scanInProgress = false;
            return ret;
        }

    case APPLE80211_IOC_SCANCACHE_CLEAR:
        /* Do not destroy the backend BSS tree while a scan may still be using
         * it.  Reset only the Apple-side enumeration cursor; the next hardware
         * scan refreshes/deduplicates entries by BSSID.  Sonoma's CoreWiFi may
         * use this as the only indication that it wants a fresh family cache,
         * so allow one new demand-driven bootstrap scan after a clear. */
        if (!isSet) return kIOReturnUnsupported;
        _scanCursor = 0;
        _scanCacheBootstrapAttempted = false;
        return kIOReturnSuccess;

    case APPLE80211_IOC_SCAN_RESULT:
        return isSet ? kIOReturnUnsupported : handleSCAN_RESULT((struct apple80211_scan_result **)data);

    case APPLE80211_IOC_RSSI: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret) return ret;
        if (st.state != RTW88_STATE_CONNECTED) return 6;
        auto *d = static_cast<apple80211_rssi_data *>(data);
        bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION;
        d->num_radios = 1; d->rssi_unit = APPLE80211_UNIT_DBM;
        d->rssi[0] = d->aggregate_rssi = st.rssi;
        d->rssi_ext[0] = d->aggregate_rssi_ext = st.rssi;
        // Signed textual RSSI avoids decoding an unsigned IORegistry number.
        // These are observations, not an invented PHY speed or MCS value.
        char signal[16];
        snprintf(signal, sizeof(signal), "%d dBm", st.rssi);
        setProperty("RADIO_RSSI", signal);
        setProperty("RADIO_CHANNEL", (uint64_t)st.channel, 32);
        setProperty("RADIO_WIDTH_MHZ", (uint64_t)_ieee80211->channelWidthMHz(), 32);
        setProperty("RADIO_HW_NSS", (uint64_t)_ieee80211->txNSS(), 32);
        setProperty("RADIO_TX_AGGREGATION", _ieee80211->txAggregationActive() ? kOSBooleanTrue : kOSBooleanFalse);
        RTW88BSS currentBSS = {};
        if (_ieee80211->copyCurrentBSS(&currentBSS) == kIOReturnSuccess) {
            setProperty("RADIO_PAIRWISE_CIPHER", (uint64_t)currentBSS.cipher, 32);
            setProperty("RADIO_GROUP_CIPHER", (uint64_t)currentBSS.group_cipher, 32);
        }
        setProperty("RADIO_RX_FRAMES", (uint64_t)_ieee80211->receivedFrameCount(), 32);
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_DISASSOCIATE:
        return isSet ? handleDISASSOCIATE() : kIOReturnUnsupported;
    case APPLE80211_IOC_OP_MODE: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_opmode_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->op_mode = APPLE80211_M_STA;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_PHY_MODE:
        return handleRequestPHY_MODE(isSet, data);
    case APPLE80211_IOC_CARD_CAPABILITIES: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_capability_data *>(data);
        bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION;

        /* Follow AirportItlwm v2.2.0's Ventura capability layout.
         * Keep the low feature bits truthful for rtw88, then use the exact
         * opaque high-byte pattern AirportItlwm currently exposes. Do not
         * invent extra AWDL bits: IO80211 probes the virtual-interface path
         * separately. */
        const unsigned caps[] = {
            APPLE80211_CAP_TKIP, APPLE80211_CAP_AES_CCM,
            APPLE80211_CAP_WPA2, APPLE80211_CAP_TKIPMIC,
            APPLE80211_CAP_SHSLOT, APPLE80211_CAP_SHPREAMBLE
        };
        for (unsigned cap : caps)
            d->capabilities[cap / 8] |= 1U << (cap % 8);

        /* AirportItlwm v2.2.0 (Ventura legacy IO80211 path) publishes these
         * high bytes. Keep them byte-for-byte aligned with the reference; the
         * previous 1.0.1 draft had drifted to a different experimental mask. */
        d->capabilities[2] = 0xFF;
        d->capabilities[3] = 0x2B;
        d->capabilities[4] = 0xAD;
        d->capabilities[5] = 0x8C;
        d->capabilities[6] = 0x8C;
        d->capabilities[7] = 0x84;
        IOLog("AirPort_RTW88: CARD_CAPABILITIES AirportItlwm v2.2.0 profile advertised\n");
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_RATE:
        return handleRequestRATE(isSet, data);

    case APPLE80211_IOC_RATE_SET:
        return handleRequestRATE_SET(isSet, data);

    case APPLE80211_IOC_MCS_INDEX_SET: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        if (_ieee80211->cmdGetState(&st) != kIOReturnSuccess || st.state != RTW88_STATE_CONNECTED)
            return 6;
        auto *d = static_cast<apple80211_mcs_index_set_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        const uint8_t nss = _ieee80211->txNSS();
        /* Advertise the standard HT MCS 0-7 block for each active spatial
         * stream; this mirrors the hardware NSS instead of inventing rates. */
        for (uint8_t mcs = 0; mcs < (uint8_t)(8U * nss) && mcs <= APPLE80211_MAX_MCS_INDEX; ++mcs)
            d->mcs_set_map[mcs / 8U] |= (uint8_t)(1U << (mcs % 8U));
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_VHT_MCS_INDEX_SET: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        if (_ieee80211->cmdGetState(&st) != kIOReturnSuccess || st.state != RTW88_STATE_CONNECTED)
            return kIOReturnError;
        auto *d = static_cast<RTW88VHTMCSIndexSetData *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        const uint8_t nss = _ieee80211->txNSS();
        /* IEEE VHT MCS map: 2 bits/stream, value 2 = MCS 0-9, 3 = not
         * supported.  rtw88 chips handled by this port are VHT capable. */
        uint16_t map = 0xffffU;
        for (uint8_t i = 0; i < nss && i < 8; ++i) {
            map &= (uint16_t)~(0x3U << (i * 2U));
            map |= (uint16_t)(0x2U << (i * 2U));
        }
        d->mcs_map = map;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_MCS_VHT: {
        if (!data) return kIOReturnBadArgument;
        if (isSet) return kIOReturnError;
        RTW88StateResult st = {};
        if (_ieee80211->cmdGetState(&st) != kIOReturnSuccess || st.state != RTW88_STATE_CONNECTED)
            return kIOReturnError;
        auto *d = static_cast<RTW88MCSVHTData *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->index = 0; /* rtw88 port does not yet expose live TX MCS */
        d->nss = _ieee80211->txNSS();
        d->bw = _ieee80211->channelWidthMHz();
        d->guard_interval = APPLE80211_GI_LONG;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_TX_ANTENNA:
    case APPLE80211_IOC_ANTENNA_DIVERSITY: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_antenna_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        d->antenna_index[0] = 1;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_MCS: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        if (_ieee80211->cmdGetState(&st) != kIOReturnSuccess || st.state != RTW88_STATE_CONNECTED)
            return 6;
        auto *d = static_cast<apple80211_mcs_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->index = 0; /* live TX-MCS export is not wired yet */
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_ROAM_THRESH: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_roam_threshold_data *>(data);
        bzero(d, sizeof(*d));
        d->threshold = 100;
        d->count = 0;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_LINK_CHANGED_EVENT_DATA: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_link_changed_event_data *>(data);
        bzero(d, sizeof(*d));
        RTW88StateResult st = {};
        const IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret != kIOReturnSuccess) return ret;
        d->isLinkDown = st.state != RTW88_STATE_CONNECTED;
        if (d->isLinkDown) {
            d->voluntary = false;
            d->reason = APPLE80211_LINK_DOWN_REASON_DEAUTH;
        } else {
            d->rssi = (uint32_t)st.rssi;
            d->nf = (uint16_t)(int16_t)-95;
            d->snr = (uint16_t)((st.rssi > -95) ? (st.rssi + 95) : 0);
        }
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_TX_NSS: {
        auto *d = static_cast<apple80211_tx_nss_data *>(data);
        if (isSet) return kIOReturnError;
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->nss = _ieee80211->txNSS();
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_NSS: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_nss_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->nss = _ieee80211->txNSS();
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_ROAM:
        /* AirportItlwm v2.2.0 recognizes the selector but declines the roam
         * command.  Returning an explicit error is better than an unknown
         * selector because CoreWiFi now knows the controller understood it. */
        return isSet ? kIOReturnError : kIOReturnUnsupported;

    case APPLE80211_IOC_WOW_PARAMETERS:
        return kIOReturnError;

    case APPLE80211_IOC_IE:
        /* Reserved for frame-IE injection.  Keep selector parity with the
         * Ventura reference without pretending that rtw88 has implemented it. */
        return kIOReturnError;

    case APPLE80211_IOC_BSSID: {
        if (isSet) return kIOReturnSuccess;
        RTW88StateResult st = {}; IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret) return ret;
        // AirportItlwm returns BSD ENXIO (6) until associated. Success with
        // an empty BSSID can make airportd treat an idle interface as joined.
        if (st.state != RTW88_STATE_CONNECTED) return 6;
        auto *d = static_cast<apple80211_bssid_data *>(data);
        bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION;
        memcpy(d->bssid.octet, st.bssid, 6);
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_DEAUTH: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_deauth_data *>(data);
        bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION;
        d->deauth_reason = _ieee80211->deauthReason();
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ASSOCIATION_STATUS: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {}; IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret) return ret;
        auto *d = static_cast<apple80211_assoc_status_data *>(data);
        bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION;
        // Query success is distinct from successful association (AirportItlwm).
        d->status = st.state == RTW88_STATE_CONNECTED ?
            APPLE80211_STATUS_SUCCESS : APPLE80211_STATUS_UNAVAILABLE;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_ASSOCIATE_RESULT: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {}; IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret) return ret;
        if (st.state != RTW88_STATE_CONNECTED) return kIOReturnNotReady;
        auto *d = static_cast<apple80211_assoc_result_data *>(data);
        d->version = APPLE80211_VERSION; d->result = APPLE80211_RESULT_SUCCESS;
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_CHANNELS_INFO:
        return handleRequestCHANNELS_INFO(isSet, data);
    case APPLE80211_IOC_ROAM_PROFILE: {
        if (!data) return kIOReturnBadArgument;
        auto *d = static_cast<RTW88RoamProfileBandData *>(data);
        if (isSet) {
            memcpy(&_roamProfile, d, sizeof(_roamProfile));
            _roamProfileValid = true;
            setProperty("STA_ROAM_PROFILE_SET", kOSBooleanTrue);
            return kIOReturnSuccess;
        }
        if (!_roamProfileValid)
            return kIOReturnError;
        memcpy(d, &_roamProfile, sizeof(_roamProfile));
        setProperty("STA_ROAM_PROFILE_GET", kOSBooleanTrue);
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_SUPPORTED_CHANNELS:
    case APPLE80211_IOC_HW_SUPPORTED_CHANNELS:
        return handleRequestHW_SUPPORTED_CHANNELS(isSet, data);
    case APPLE80211_IOC_DRIVER_VERSION:
    case APPLE80211_IOC_HARDWARE_VERSION: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_version_data *>(data);
        bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION;
        const char *chip = rtw88AirportChipName(_compatPciDev);
        if (request_number == APPLE80211_IOC_DRIVER_VERSION) {
            char label[sizeof(d->string)] = {};
            strlcpy(label, chip, sizeof(label));
            strlcat(label, " (AirPort_RTW88 2.0.0)", sizeof(label));
            d->string_len = (uint16_t)strlcpy(d->string, label, sizeof(d->string));
        } else {
            d->string_len = (uint16_t)strlcpy(d->string, chip, sizeof(d->string));
        }
        return kIOReturnSuccess;
    }

    /* System Information queries AirDrop capability/channel from the primary
     * controller, not only through awdl0's virtual IOCTL path.  Returning
     * unsupported (or channel 0) makes the UI omit the "AirDrop Channel"
     * field even though awdl0 exists.  Mirror the manager's effective social
     * channel here, while keeping SET ownership with IO80211's virtual path. */
    case APPLE80211_IOC_AWDL_MASTER_CHANNEL: {
        if (isSet || !data || !_awdlManager) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_awdl_master_channel *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->master_channel = _awdlManager->reportedChannel();
        return d->master_channel ? kIOReturnSuccess : kIOReturnNotReady;
    }
    case APPLE80211_IOC_AWDL_SECONDARY_MASTER_CHANNEL: {
        if (isSet || !data || !_awdlManager) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_awdl_secondary_master_channel *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->secondary_master_channel = _awdlManager->secondaryMasterChannel();
        return kIOReturnSuccess;
    }
    case APPLE80211_IOC_AWDL_SYNC_ENABLED: {
        if (isSet || !data || !_awdlManager) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_awdl_sync_enabled *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->enabled = _awdlManager->syncEnabled() ? 1 : 0;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_VIRTUAL_IF_CREATE:
        return isSet ? handleVIRTUAL_IF_CREATE((struct apple80211_virt_if_create_data *)data)
                     : kIOReturnUnsupported;

    case APPLE80211_IOC_VIRTUAL_IF_DELETE:
        return isSet ? handleVIRTUAL_IF_DELETE((struct apple80211_virt_if_delete_data *)data)
                     : kIOReturnUnsupported;

    case APPLE80211_IOC_CURRENT_NETWORK:
        if (isSet) return kIOReturnUnsupported;
        IOLog("AirPort_RTW88: CURRENT_NETWORK dispatcher entry\n");
        return handleCURRENT_NETWORK(static_cast<apple80211_scan_result *>(data));

    case APPLE80211_IOC_STATE:
        return isSet ? kIOReturnUnsupported : handleSTATE((struct apple80211_state_data *)data);

    case APPLE80211_IOC_CHANNEL:
        return isSet ? kIOReturnUnsupported : handleCHANNEL((struct apple80211_channel_data *)data);

    case APPLE80211_IOC_RADIO_INFO: {
        if (isSet) return kIOReturnUnsupported;
        if (!data) return kIOReturnBadArgument;
        auto *radio = static_cast<apple80211_radio_info_data *>(data);
        bzero(radio, sizeof(*radio));
        radio->version = APPLE80211_VERSION;
        radio->count = 1;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_POWER:
        return handlePOWER(isSet, (struct apple80211_power_data *)data);

    case APPLE80211_IOC_NOISE: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret) return ret;
        if (st.state != RTW88_STATE_CONNECTED) return 6;
        auto *d = static_cast<apple80211_noise_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        d->noise_unit = APPLE80211_UNIT_DBM;
        d->noise[0] = d->aggregate_noise = -95;
        d->noise_ext[0] = d->aggregate_noise_ext = -95;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_INT_MIT: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_intmit_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->int_mit = APPLE80211_INT_MIT_AUTO;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_PROTMODE: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret) return ret;
        if (st.state != RTW88_STATE_CONNECTED) return 6;
        auto *d = static_cast<apple80211_protmode_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->protmode = APPLE80211_PROTMODE_OFF;
        d->threshold = 0;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_TXPOWER: {
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        IOReturn ret = _ieee80211->cmdGetState(&st);
        if (ret) return ret;
        if (st.state != RTW88_STATE_CONNECTED) return 6;
        auto *d = static_cast<apple80211_txpower_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->txpower_unit = APPLE80211_UNIT_PERCENT;
        d->txpower = 100;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_POWERSAVE: {
        auto *d = static_cast<apple80211_powersave_data *>(data);
        if (!d) return kIOReturnBadArgument;
        if (isSet) {
            if (d->version != APPLE80211_VERSION ||
                d->powersave_level > APPLE80211_POWERSAVE_MODE_MAX_POWERSAVE)
                return kIOReturnBadArgument;
            _powerSaveLevel = d->powersave_level;
            setProperty("STA_V20_POWERSAVE_SET", kOSBooleanTrue);
            setProperty("STA_V20_POWERSAVE_VALUE", (uint64_t)_powerSaveLevel, 32);
            return kIOReturnSuccess;
        }
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->powersave_level = _powerSaveLevel;
        return kIOReturnSuccess;
    }

    case APPLE80211_IOC_LOCALE: {
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_locale_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->locale = APPLE80211_LOCALE_ROW;
        return kIOReturnSuccess;
    }



    default:
        return kIOReturnUnsupported;
    }
}

/* IO80211FamilyLegacy re-enters the dispatcher while posting COUNTRY_CODE
 * changes. v24 reserved 0x1210 bytes on EVERY call, exhausting the kernel
 * stack on that nested path. Keep large scratch buffers in non-inlined
 * handlers so unrelated requests, including country GET/SET, stay small.
 */
__attribute__((__noinline__)) IOReturn AirportRTW88::handleRequestPHY_MODE(bool isSet, void *data)
{
        if (isSet) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_phymode_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->phy_mode = APPLE80211_MODE_11A | APPLE80211_MODE_11B |
            APPLE80211_MODE_11G | APPLE80211_MODE_11N | APPLE80211_MODE_11AC;

        /* AirportItlwm reports the current negotiated PHY instead of AUTO
         * once a BSS is selected.  rtw88 already keeps the target BSS IEs, so
         * infer the same legacy/HT/VHT distinction without depending on a
         * host-version-specific Skywalk API. */
        d->active_phy_mode = APPLE80211_MODE_AUTO;
        RTW88StateResult st = {};
        RTW88BSS b = {};
        if (_ieee80211->cmdGetState(&st) == kIOReturnSuccess &&
            (st.state == RTW88_STATE_CONNECTED ||
             st.state == RTW88_STATE_HANDSHAKING ||
             st.state == RTW88_STATE_ASSOCIATING ||
             st.state == RTW88_STATE_AUTHENTICATING) &&
            _ieee80211->copyCurrentBSS(&b) == kIOReturnSuccess) {
            bool hasHT = false, hasVHT = false;
            for (uint16_t off = 0; off + 2 <= b.ies_len; ) {
                const uint8_t id = b.ies[off];
                const uint8_t ieLen = b.ies[off + 1];
                if ((uint32_t)off + 2U + ieLen > b.ies_len) break;
                if (id == WLAN_EID_VHT_CAPABILITY || id == WLAN_EID_VHT_OPERATION)
                    hasVHT = true;
                else if (id == WLAN_EID_HT_CAPABILITY || id == WLAN_EID_HT_OPERATION)
                    hasHT = true;
                off = (uint16_t)(off + 2U + ieLen);
            }
            if (hasVHT)
                d->active_phy_mode = APPLE80211_MODE_11AC;
            else if (hasHT)
                d->active_phy_mode = APPLE80211_MODE_11N;
            else if (b.channel > 14)
                d->active_phy_mode = APPLE80211_MODE_11A;
            else
                d->active_phy_mode = APPLE80211_MODE_11G;
        }
        return kIOReturnSuccess;
    }

__attribute__((__noinline__)) IOReturn AirportRTW88::handleRequestRATE(bool isSet, void *data)
{
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        if (_ieee80211->cmdGetState(&st) != kIOReturnSuccess || st.state != RTW88_STATE_CONNECTED)
            return 6;
        RTW88BSS b = {};
        if (_ieee80211->copyCurrentBSS(&b) != kIOReturnSuccess) return 6;
        auto *d = static_cast<apple80211_rate_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->num_radios = 1;
        uint32_t best = 0;
        for (uint16_t pos = 0; pos + 2 <= b.ies_len;) {
            const uint8_t id = b.ies[pos];
            const uint8_t n = b.ies[pos + 1];
            if ((uint32_t)pos + 2U + n > b.ies_len) break;
            if (id == 1 || id == 50) {
                for (uint8_t i = 0; i < n; ++i) {
                    const uint32_t r = b.ies[pos + 2 + i] & 0x7fU;
                    if (r > best) best = r;
                }
            }
            pos = (uint16_t)(pos + 2U + n);
        }
        /* Legacy rates use 500-kbit/s units; AirportItlwm exposes its
         * net80211 rate value directly through this ABI. */
        d->rate[0] = best;
        return kIOReturnSuccess;
    }

__attribute__((__noinline__)) IOReturn AirportRTW88::handleRequestRATE_SET(bool isSet, void *data)
{
        if (isSet) return kIOReturnUnsupported;
        RTW88StateResult st = {};
        if (_ieee80211->cmdGetState(&st) != kIOReturnSuccess || st.state != RTW88_STATE_CONNECTED)
            return 6;
        RTW88BSS b = {};
        if (_ieee80211->copyCurrentBSS(&b) != kIOReturnSuccess) return 6;
        auto *d = static_cast<apple80211_rate_set_data *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        for (uint16_t pos = 0; pos + 2 <= b.ies_len && d->num_rates < APPLE80211_MAX_RATES;) {
            const uint8_t id = b.ies[pos];
            const uint8_t n = b.ies[pos + 1];
            if ((uint32_t)pos + 2U + n > b.ies_len) break;
            if (id == 1 || id == 50) {
                for (uint8_t i = 0; i < n && d->num_rates < APPLE80211_MAX_RATES; ++i) {
                    apple80211_rate &r = d->rates[d->num_rates++];
                    r.version = APPLE80211_VERSION;
                    r.rate = b.ies[pos + 2 + i] & 0x7fU;
                    r.flags = (b.ies[pos + 2 + i] & 0x80U) ? APPLE80211_RATE_FLAG_BASIC : APPLE80211_RATE_FLAG_NONE;
                }
            }
            pos = (uint16_t)(pos + 2U + n);
        }
        return kIOReturnSuccess;
    }

__attribute__((__noinline__)) IOReturn AirportRTW88::handleRequestCHANNELS_INFO(bool isSet, void *data)
{
        if (isSet || !data) return kIOReturnUnsupported;
        auto *d = static_cast<apple80211_channels_info *>(data);
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        RTW88Channel channels[APPLE80211_MAX_CHANNELS] = {};
        uint32_t count = 0;
        IOReturn ret = _ieee80211->copyChannels(channels, APPLE80211_MAX_CHANNELS, &count);
        if (ret != kIOReturnSuccess && ret != kIOReturnNoSpace) return ret;
        if (count > APPLE80211_MAX_CHANNELS) count = APPLE80211_MAX_CHANNELS;
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t ch = channels[i].number;
            d->chan_num[i] = (uint8_t)ch;
            d->passive[i] = channels[i].passive ? 1 : 0;
            d->radar_dfs[i] = channels[i].radar ? 1 : 0;
            d->support_40Mhz[i] = (ch != 14) ? 1 : 0;
            d->support_80Mhz[i] = (ch > 14) ? 1 : 0;
            d->chan_spec[i] = ch;
        }
        d->num_chan_specs = (uint16_t)count;
        setProperty("STA_CHANNELS_INFO_OK", kOSBooleanTrue);
        setProperty("STA_CHANNELS_INFO_COUNT", (uint64_t)count, 32);
        return kIOReturnSuccess;
    }

__attribute__((__noinline__)) IOReturn AirportRTW88::handleRequestHW_SUPPORTED_CHANNELS(bool isSet, void *data)
{
        if (isSet) return kIOReturnUnsupported;
        RTW88Channel channels[APPLE80211_MAX_CHANNELS] = {};
        uint32_t count = 0;
        IOReturn ret = _ieee80211->copyChannels(channels, APPLE80211_MAX_CHANNELS, &count);
        if (ret) return ret;
        auto *d = static_cast<apple80211_sup_channel_data *>(data);
        bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION; d->num_channels = count;
        for (uint32_t i = 0; i < count; i++) {
            d->supported_channels[i].version = APPLE80211_VERSION;
            d->supported_channels[i].channel = channels[i].number;
            d->supported_channels[i].flags = APPLE80211_C_FLAG_ACTIVE |
                APPLE80211_C_FLAG_20MHZ |
                (channels[i].number <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
        }
        return kIOReturnSuccess;
    }

__attribute__((__noinline__)) IOReturn AirportRTW88::handleRequestCOUNTRY_CODE(bool isSet, void *data)
{
        auto *d = static_cast<apple80211_country_code_data *>(data);
        if (isSet) {
            if (d->version != APPLE80211_VERSION) return kIOReturnBadArgument;
            /* Keep Apple's special x/X requests from replacing the usable code. */
            if (d->cc[0] != 'x' && d->cc[0] != 'X') {
                memcpy(_countryCode, d->cc, sizeof(_countryCode));
                _countryCode[APPLE80211_MAX_CC_LEN - 1] = 0;
                if (_netif) _netif->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED);
            }
            return kIOReturnSuccess;
        }
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        memcpy(d->cc, _countryCode, sizeof(d->cc));
        return kIOReturnSuccess;
    }

bool AirportRTW88::ensureAWDLVirtualInterface()
{
    if (!_awdlManager || !_netif)
        return false;
    if (_awdlManager->awdlInterface())
        return true;

    ether_addr addr = {};
    // The controller cache is only populated by SET hardware address;
    // normal startup can leave it zero. Read the initialized backend instead.
    IOEthernetAddress physical = {};
    if (getHardwareAddress(&physical) != kIOReturnSuccess ||
        !RTW88AWDL::unicast(physical.bytes)) return false;
    memcpy(addr.octet, physical.bytes, sizeof(addr.octet));
    /* AWDL uses a distinct locally administered unicast address. Keep it
     * deterministic for this boot and distinct from en0. */
    addr.octet[0] = (uint8_t)((addr.octet[0] | 0x02u) & 0xFEu);
    addr.octet[5] ^= 0x80u;

    IO80211VirtualInterface *created = nullptr;
    IOLog("AirPort_RTW88: proactively attaching AWDL VIF mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
          addr.octet[0], addr.octet[1], addr.octet[2], addr.octet[3], addr.octet[4], addr.octet[5]);
    bool ok = attachVirtualInterface(&created, &addr, APPLE80211_VIF_AWDL, true);
    if (!ok || !created) {
        IOLog("AirPort_RTW88: proactive attachVirtualInterface(AWDL) failed\n");
        return false;
    }
    IOLog("AirPort_RTW88: proactive AWDL VIF attached bsd=%s role=%u\n",
          created->getBSDName() ? created->getBSDName() : "?",
          (unsigned)created->getInterfaceRole());

    /* Ventura can publish the BSD awdl0 object without driving the legacy
     * enableVirtualInterface callback.  In that state ifconfig shows awdl0
     * with mtu 0 / inactive and CoreWLAN still reports no usable VIF.  Drive
     * the controller's normal enable path once, explicitly, after attach.
     * This is intentionally non-fatal: STA must remain usable even if the
     * private IO80211 VIF lifecycle rejects the transition. */
    SInt32 enableRet = enableVirtualInterface(created);
    IOLog("AirPort_RTW88: proactive AWDL enable result=0x%x bsd=%s\n",
          (unsigned)enableRet, created->getBSDName() ? created->getBSDName() : "?");
    if (enableRet != kIOReturnSuccess) {
        // An attached BSD object is not an enabled AWDL transport.
        _awdlManager->clearVirtualInterface(created);
        return false;
    }

    return true;
}

IOReturn AirportRTW88::handleVIRTUAL_IF_CREATE(struct apple80211_virt_if_create_data *d)
{
    if (!d || d->version != APPLE80211_VERSION)
        return kIOReturnBadArgument;
    if (d->role < APPLE80211_VIF_P2P_DEVICE || d->role > APPLE80211_VIF_AWDL)
        return kIOReturnUnsupported;

    ether_addr addr = {};
    memcpy(addr.octet, d->mac, APPLE80211_ADDR_LEN);

    if (!_awdlManager)
        return kIOReturnNotReady;

    IO80211VirtualInterface *existing =
        d->role == APPLE80211_VIF_AWDL ? _awdlManager->awdlInterface()
                                        : _awdlManager->p2pInterface();

    if (existing) {
        const char *name = existing->getBSDName();
        bzero(d->bsd_name, sizeof(d->bsd_name));
        if (name) strlcpy((char *)d->bsd_name, name, sizeof(d->bsd_name));
        IOLog("AirPort_RTW88: VIRTUAL_IF_CREATE role=%u already exists bsd=%s\n",
              d->role, name ? name : "?");
        return kIOReturnSuccess;
    }

    IOLog("AirPort_RTW88: VIRTUAL_IF_CREATE role=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
          d->role, d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);

    /* Match AirportItlwm's lifecycle: let IO80211 perform the full
     * attach/configure/name sequence. This calls createVirtualInterface(),
     * then enableVirtualInterface(), and eventually yields p2p0/awdl0. */
    IO80211VirtualInterface *created = nullptr;
    if (!attachVirtualInterface(&created, &addr, d->role, true) || !created) {
        IOLog("AirPort_RTW88: attachVirtualInterface failed role=%u\n", d->role);
        return kIOReturnError;
    }
    _awdlManager->setVirtualInterface(d->role, created);

    const char *name = created->getBSDName();
    bzero(d->bsd_name, sizeof(d->bsd_name));
    if (name) strlcpy((char *)d->bsd_name, name, sizeof(d->bsd_name));

    IOLog("AirPort_RTW88: VIRTUAL_IF_CREATE success role=%u bsd=%s\n",
          d->role, name ? name : "?");
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleVIRTUAL_IF_DELETE(struct apple80211_virt_if_delete_data *d)
{
    if (!d || d->version != APPLE80211_VERSION)
        return kIOReturnBadArgument;

    char requested[sizeof(d->bsd_name) + 1] = {};
    memcpy(requested, d->bsd_name, sizeof(d->bsd_name));

    IO80211VirtualInterface *target = nullptr;
    IO80211VirtualInterface *awdl = _awdlManager ? _awdlManager->awdlInterface() : nullptr;
    IO80211VirtualInterface *p2p  = _awdlManager ? _awdlManager->p2pInterface() : nullptr;
    if (awdl && awdl->getBSDName() &&
        strncmp(awdl->getBSDName(), requested, sizeof(d->bsd_name)) == 0)
        target = awdl;
    else if (p2p && p2p->getBSDName() &&
             strncmp(p2p->getBSDName(), requested, sizeof(d->bsd_name)) == 0)
        target = p2p;

    if (!target) {
        IOLog("AirPort_RTW88: VIRTUAL_IF_DELETE bsd=%s not found\n", requested);
        return kIOReturnNotFound;
    }

    UInt role = (UInt)target->getInterfaceRole();
    IOLog("AirPort_RTW88: VIRTUAL_IF_DELETE role=%u bsd=%s\n", role, requested);
    bool ok = detachVirtualInterface(target, true);
    if (ok) {
        if (_awdlManager) _awdlManager->clearVirtualInterface(target);
        return kIOReturnSuccess;
    }
    return kIOReturnError;
}

IOReturn AirportRTW88::handleSSID(bool set, struct apple80211_ssid_data *d)
{
    if (!d) return kIOReturnBadArgument;
    if (set) return kIOReturnSuccess; // AirportItlwm accepts this; ASSOCIATE carries the target.
    RTW88StateResult st = {}; IOReturn r = _ieee80211->cmdGetState(&st);
    setProperty("STA_LAST_RAW_STATE", (uint64_t)_ieee80211->rawState(), 32);
    setProperty("STA_LAST_REPORTED_STATE", (uint64_t)st.state, 32);
    setProperty("STA_ASSOC_VISIBLE", _ieee80211->associatedVisible() ? kOSBooleanTrue : kOSBooleanFalse);
    if (r) return r;
    // Match the classic IO80211 contract: no current SSID before RUN.
    if (st.state != RTW88_STATE_CONNECTED) return 6;
    bzero(d, sizeof(*d)); d->version = APPLE80211_VERSION;
    d->ssid_len = (uint32_t)strnlen(st.ssid, sizeof(d->ssid_bytes));
    memcpy(d->ssid_bytes, st.ssid, d->ssid_len);
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleAUTH_TYPE(bool set, struct apple80211_authtype_data *d)
{
    if (!d) return kIOReturnBadArgument;
    if (set) {
        if (d->version != APPLE80211_VERSION) return kIOReturnBadArgument;
        /* AirportItlwm stores the Apple auth request verbatim.  Do not reject
         * mixed WPA/WPA2 masks before the actual ASSOCIATE request arrives. */
        _authLower = d->authtype_lower;
        _authUpper = d->authtype_upper;
    } else {
        bzero(d, sizeof(*d));
        d->version = APPLE80211_VERSION;
        d->authtype_lower = _authLower;
        d->authtype_upper = _authUpper;
    }
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleASSOCIATE(struct apple80211_assoc_data *d)
{
    setProperty("STA_INTERNAL_RSN_MODE", kOSBooleanTrue);
    kprintf("AirPort_RTW88: handleASSOCIATE ENTER data=%p (internal RSN)\n", d);
    // Metadata only: never publish SSID, BSSID, PMK or password bytes.
    if (d) {
        setProperty("STA_ASSOC_INPUT_VERSION", (uint64_t)d->version, 32);
        setProperty("STA_ASSOC_INPUT_MODE", (uint64_t)d->ad_mode, 32);
        setProperty("STA_ASSOC_INPUT_AUTH_LOWER", (uint64_t)d->ad_auth_lower, 32);
        setProperty("STA_ASSOC_INPUT_AUTH_UPPER", (uint64_t)d->ad_auth_upper, 32);
        setProperty("STA_ASSOC_INPUT_SSID_LEN", (uint64_t)d->ad_ssid_len, 32);
        setProperty("STA_ASSOC_INPUT_KEY_LEN", (uint64_t)d->ad_key.key_len, 32);
    }

    if (!d || d->version != APPLE80211_VERSION ||
        !d->ad_ssid_len || d->ad_ssid_len > APPLE80211_MAX_SSID_LEN)
        return kIOReturnBadArgument;

    char ssid[APPLE80211_MAX_SSID_LEN + 1] = {};
    memcpy(ssid, d->ad_ssid, d->ad_ssid_len);
    if (strnlen(ssid, sizeof(ssid)) != d->ad_ssid_len)
        return kIOReturnUnsupported;

    const uint8_t emptyBssid[APPLE80211_ADDR_LEN] = {};
    const uint8_t *bssid = memcmp(d->ad_bssid.octet, emptyBssid,
                                  APPLE80211_ADDR_LEN) ? d->ad_bssid.octet : nullptr;

    const uint32_t personalMask =
        APPLE80211_AUTHTYPE_WPA_PSK |
        APPLE80211_AUTHTYPE_WPA2_PSK |
        APPLE80211_AUTHTYPE_SHA256_PSK;
    const bool secure = (d->ad_auth_upper & personalMask) != 0;

    if (d->ad_auth_lower != APPLE80211_AUTHTYPE_OPEN ||
        (d->ad_auth_upper != APPLE80211_AUTHTYPE_NONE && !secure))
        return kIOReturnUnsupported;

    /* Proven Ventura path (0.4.0 -> 1.0.0): IO80211 supplies the 32-byte
     * association PMK in ad_key.  Keep the 4-way handshake inside rtw88.
     * Do not enable Apple RSN here: that experimental path changed the join
     * transport and was never the known-good RTL8822BE path. */
    const uint8_t *pmk = nullptr;
    if (secure) {
        if (d->ad_key.key_len != 32) {
            setProperty("STA_ASSOCIATE_BAD_PMK_LEN", (uint64_t)d->ad_key.key_len, 32);
            return kIOReturnUnsupported;
        }
        pmk = d->ad_key.key;

    }

    // Validate the open-network key before tearing down an existing link.
    if (!secure && d->ad_key.key_len != 0)
        return kIOReturnUnsupported;

    RTW88StateResult current = {};
    if (_ieee80211->cmdGetState(&current) == kIOReturnSuccess &&
        (current.state == RTW88_STATE_AUTHENTICATING ||
         current.state == RTW88_STATE_ASSOCIATING ||
         current.state == RTW88_STATE_HANDSHAKING ||
         current.state == RTW88_STATE_CONNECTED)) {
        bool same = _ieee80211->matchesPendingJoin(ssid, bssid, pmk);
        setProperty("ASSOCIATE_COALESCED", same ? kOSBooleanTrue : kOSBooleanFalse);
        if (same) return kIOReturnSuccess;
        // A valid new ASSOCIATE supersedes the old attempt/link. Drain its
        // authentication worker and remove old keys/STA before setting the
        // new request's RSN IE (doDisconnect clears the previous IE).
        IOReturn disconnectRet = _ieee80211->cmdDisconnect();
        if (disconnectRet != kIOReturnSuccess) return disconnectRet;
    }

    _authLower = d->ad_auth_lower;
    _authUpper = d->ad_auth_upper;
    _ieee80211->clearAssocRsnIE();
    if (secure) {
        /* Ventura's legacy struct names the 16-bit RSN length slot `pad`.
         * Prefer the TLV's own length because that is what the proven audit
         * path used and it is stable across the older/newer header naming. */
        uint16_t rsnLen = 0;
        if (d->ad_rsn_ie[0] == 48)
            rsnLen = (uint16_t)d->ad_rsn_ie[1] + 2;
        if (rsnLen >= 2 && rsnLen <= APPLE80211_MAX_RSN_IE_LEN) {
            IOReturn rsnRet = _ieee80211->cmdSetAssocRsnIE(d->ad_rsn_ie, rsnLen);
            if (rsnRet != kIOReturnSuccess) return rsnRet;
        }
    } else {
        if (d->ad_key.key_len != 0)
            return kIOReturnUnsupported;
        _ieee80211->clearAssocRsnIE();
    }

    _assocDoneReported = false;
    _rsnHandshakeReported = false;
    setProperty("STA_ASSOCIATE_BEFORE_RAW_STATE", (uint64_t)_ieee80211->rawState(), 32);
    IOReturn ret = _ieee80211->cmdConnect(ssid, nullptr, pmk, bssid, false);
    setProperty("STA_ASSOCIATE_CMD_RET", (uint64_t)(uint32_t)ret, 32);
    setProperty("STA_ASSOCIATE_AFTER_RAW_STATE", (uint64_t)_ieee80211->rawState(), 32);
    if (ret == kIOReturnSuccess)
        setProperty("STA_ASSOCIATE_ACCEPTED", kOSBooleanTrue);

    IOLog("AirportRTW88: ASSOCIATE internal-RSN ssid=%s secure=%d keylen=%u ret=0x%x\n",
          ssid, secure, d->ad_key.key_len, ret);
    return ret;
}

IOReturn AirportRTW88::handleRSN_IE(bool set, struct apple80211_rsn_ie_data *d)
{
    if (!d) return kIOReturnBadArgument;
    if (set) {
        if (d->version != APPLE80211_VERSION || d->len < 2 ||
            d->len > APPLE80211_MAX_RSN_IE_LEN)
            return kIOReturnBadArgument;
        return _ieee80211->cmdSetAssocRsnIE(d->ie, d->len);
    }
    uint16_t len = 0;
    IOReturn ret = _ieee80211->copyTargetRsnIE(d->ie, sizeof(d->ie), &len);
    if (ret) return ret;
    d->version = APPLE80211_VERSION;
    d->len = len;
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleAP_IE_LIST(struct apple80211_ap_ie_data *d)
{
    if (!d || !d->ie_data || d->len == 0) return kIOReturnBadArgument;
    uint16_t len = 0;
    uint8_t rsn[APPLE80211_MAX_RSN_IE_LEN] = {};
    IOReturn ret = _ieee80211->copyTargetRsnIE(rsn, sizeof(rsn), &len);
    if (ret) return ret;
    if (len > d->len) return kIOReturnNoSpace;
    memcpy(d->ie_data, rsn, len);
    d->version = APPLE80211_VERSION;
    d->len = len;
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleCIPHER_KEY(struct apple80211_key *key)
{
    if (!key || key->version != APPLE80211_VERSION || key->key_len > APPLE80211_KEY_BUFF_LEN)
        return kIOReturnBadArgument;

    if (key->key_cipher_type == APPLE80211_CIPHER_NONE)
        return kIOReturnSuccess;

    uint32_t cipher = 0;
    switch (key->key_cipher_type) {
    case APPLE80211_CIPHER_AES_CCM:
        cipher = WLAN_CIPHER_SUITE_CCMP;
        break;
    case APPLE80211_CIPHER_TKIP:
        cipher = WLAN_CIPHER_SUITE_TKIP;
        break;
    default:
        return kIOReturnUnsupported;
    }

    /* Match AirportItlwm semantics strictly:
     * key_flags == 4 -> PTK
     * key_flags == 0 -> GTK
     */
    bool pairwise;
    if (key->key_flags == 4) {
        pairwise = true;
    } else if (key->key_flags == 0) {
        pairwise = false;
    } else {
        IOLog("AirportRTW88: unexpected CIPHER_KEY flags=%u\n",
              key->key_flags);
        return kIOReturnUnsupported;
    }
    IOReturn ret = _ieee80211->cmdInstallExternalKey(pairwise,
                    (uint8_t)key->key_index, cipher, key->key, (uint8_t)key->key_len);
    IOLog("AirportRTW88: CIPHER_KEY %s cipher=%u idx=%u len=%u ret=0x%x\n",
          pairwise ? "PTK" : "GTK", key->key_cipher_type, key->key_index,
          key->key_len, ret);
    setProperty("STA_KEY_RAW_STATE", (uint64_t)_ieee80211->rawState(), 32);
    setProperty("STA_KEY_ASSOC_VISIBLE", _ieee80211->associatedVisible() ? kOSBooleanTrue : kOSBooleanFalse);
    /* AirportItlwm v2.3 posts APPLE80211_M_RSN_HANDSHAKE_DONE after each
     * successful PTK/GTK callback.  Do the same.  Waiting for *both* keys
     * before posting this event can deadlock Apple's supplicant if it expects
     * the PTK notification before it submits the GTK.  The Realtek backend
     * still gates ordinary data until both keys are installed. */
    if (!ret && _netif) {
        _netif->postMessage(APPLE80211_M_RSN_HANDSHAKE_DONE);
        _rsnHandshakeReported = true;
        setProperty(pairwise ? "APPLE_RSN_PTK_EVENT" : "APPLE_RSN_GTK_EVENT",
                    kOSBooleanTrue);
        IOLog("AirportRTW88: Apple RSN key event posted type=%s ready=%d\n",
              pairwise ? "PTK" : "GTK", _ieee80211->externalKeysReady());
    }
    return ret;
}

IOReturn AirportRTW88::handleDISASSOCIATE()
{
    /* AirportItlwm acknowledges DISASSOCIATE without tearing down AUTH/ASSOC.
     * Its SCAN state also remains a scan rather than becoming a fake link-down
     * event.  Our raw SCANNING state is an implementation detail, so when no
     * infrastructure RUN latch exists, acknowledge the cleanup request without
     * destroying the scan transaction. */
    const RTW88State raw = _ieee80211->rawState();
    const bool visible = _ieee80211->associatedVisible();
    setProperty("STA_DISASSOC_RAW_STATE", (uint64_t)raw, 32);
    setProperty("STA_DISASSOC_ASSOC_VISIBLE", visible ? kOSBooleanTrue : kOSBooleanFalse);

    if (raw == RTW88_STATE_AUTHENTICATING || raw == RTW88_STATE_ASSOCIATING) {
        setProperty("STA_DISASSOC_IGNORED_DURING_JOIN", kOSBooleanTrue);
        IOLog("AirportRTW88: DISASSOCIATE ignored during join raw_state=%u\n", raw);
        return kIOReturnSuccess;
    }

    if (raw == RTW88_STATE_SCANNING && !visible) {
        setProperty("STA_DISASSOC_ACK_SCAN_ONLY", kOSBooleanTrue);
        setProperty("STA_DISASSOC_EXECUTED", kOSBooleanFalse);
        IOLog("AirportRTW88: DISASSOCIATE acknowledged during disconnected scan (no teardown)\n");
        /* v9 accidentally called cmdDisconnect() here, which generated a real
         * disconnected event and collapsed SCANNING to IDLE.  AirportItlwm's
         * join path treats this cleanup as non-destructive while association
         * is being prepared. */
        return kIOReturnSuccess;
    }

    _rsnHandshakeReported = false;
    _ieee80211->clearAssocRsnIE();
    setProperty("STA_DISASSOC_EXECUTED", kOSBooleanTrue);
    return _ieee80211->cmdDisconnect();
}

IOReturn AirportRTW88::handleSCAN_REQ(void *data)
{
    if (!data) return kIOReturnBadArgument;
    auto *d = static_cast<apple80211_scan_data *>(data);
    if (d->version != APPLE80211_VERSION || d->ssid_len > APPLE80211_MAX_SSID_LEN ||
        d->num_channels > APPLE80211_MAX_CHANNELS) return kIOReturnBadArgument;
    if (_scanInProgress) {
        IOLog("AirPort_RTW88: SCAN_REQ coalesced with active scan\n");
        return kIOReturnSuccess;
    }
    RTW88StateResult st = {};
    if (_ieee80211->cmdGetState(&st) == kIOReturnSuccess &&
        st.state == RTW88_STATE_CONNECTED) {
        _scanCursor = 0;
        UInt32 result = 0;
        IOLog("AirPort_RTW88: SCAN_REQ connected cache-only completion\n");
        _netif->postMessage(APPLE80211_M_SCAN_DONE, &result, sizeof(result));
        return kIOReturnSuccess;
    }
    _scanInProgress = true; _scanCursor = 0;
    IOReturn ret = _ieee80211->cmdScan();
    if (ret) _scanInProgress = false;
    return ret;
}

void AirportRTW88::fillScanResultFromBSS(const RTW88BSS &b, struct apple80211_scan_result *d, bool fullIEs)
{
    if (!d) return;
    bzero(d, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->asr_channel.version = APPLE80211_VERSION;
    d->asr_channel.channel = b.channel;
    /* Match AirportItlwm's Ventura behavior: keep scan/current-network
     * channel flags deliberately conservative so CoreWiFi accepts them. */
    d->asr_channel.flags = APPLE80211_C_FLAG_ACTIVE |
                           APPLE80211_C_FLAG_20MHZ |
        (b.channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);

    d->asr_noise = -95;
    d->asr_rssi = b.rssi;
    d->asr_snr = (int16_t)(b.rssi - d->asr_noise);
    d->asr_beacon_int = (int16_t)b.beacon_interval;
    d->asr_cap = (int16_t)b.capabilities;
    d->asr_age = 0;
    memcpy(d->asr_bssid, b.bssid, APPLE80211_ADDR_LEN);
    d->asr_ssid_len = b.ssid_len > APPLE80211_MAX_SSID_LEN ?
                      APPLE80211_MAX_SSID_LEN : b.ssid_len;
    if (d->asr_ssid_len)
        memcpy(d->asr_ssid, b.ssid, d->asr_ssid_len);

    const size_t ieLength = b.ies_len > sizeof(b.ies) ? sizeof(b.ies) : b.ies_len;
    d->asr_ie_len = 0;
    if (fullIEs) {
        /* AirportItlwm's CURRENT_NETWORK is a scan-result-style object. Give
         * CoreWiFi/locationd the complete BSS IE blob for the associated AP. */
        const size_t ieCopy = ieLength > sizeof(d->asr_ie_data) ? sizeof(d->asr_ie_data) : ieLength;
        if (ieCopy) {
            memcpy(d->asr_ie_data, b.ies, ieCopy);
            d->asr_ie_len = (int16_t)ieCopy;
        }
    } else {
        /* Preserve the already-working Ventura SCAN_RESULT contract: expose
         * only the first RSN TLV, matching the audited AirportItlwm behavior. */
        for (size_t pos = 0; pos + 2 <= ieLength;) {
            const size_t n = (size_t)b.ies[pos + 1] + 2;
            if (n > ieLength - pos) break;
            if (b.ies[pos] == 48) { /* RSN TLV */
                const size_t copyLen = n > sizeof(d->asr_ie_data) ? sizeof(d->asr_ie_data) : n;
                memcpy(d->asr_ie_data, b.ies + pos, copyLen);
                d->asr_ie_len = (int16_t)copyLen;
                break;
            }
            pos += n;
        }
    }

    /* Populate real supported rates from beacon/probe IEs. */
    for (size_t pos = 0; pos + 2 <= ieLength;) {
        const size_t n = (size_t)b.ies[pos + 1] + 2;
        if (n > ieLength - pos) break;
        if (b.ies[pos] == 1 || b.ies[pos] == 50) {
            for (size_t j = 0; j < n - 2 && d->asr_nrates < APPLE80211_MAX_RATES; j++)
                d->asr_rates[d->asr_nrates++] = b.ies[pos + 2 + j];
        }
        pos += n;
    }
}

IOReturn AirportRTW88::handleSCAN_RESULT(struct apple80211_scan_result **out)
{
    if (!out) return kIOReturnBadArgument;
    *out = nullptr;

    /* Keep the Ventura/AirportItlwm pointer-to-pointer SCAN_RESULT ABI.
     * On Sonoma with the OCLP legacy wireless stack, CoreWiFi can skip
     * SCAN_REQ/SCAN_REQ_MULTIPLE entirely and query only the family cache.
     * If that cache is empty, perform exactly one demand-driven physical scan
     * while unassociated.  The normal kRTW88EventScanDone path posts
     * APPLE80211_M_SCAN_DONE; a later GET then enumerates the populated cache.
     *
     * Do not do this on Ventura: its existing explicit scan request path is
     * known-good and must remain untouched. */
    if (_scanInProgress && version_major < 23)
        return kIOReturnBusy;

    RTW88BSS b = {};
    IOReturn ret = _ieee80211->copyScanBSS(_scanCursor, &b);
    setProperty("STA_V24_SCAN_CACHE_RET", (uint64_t)(uint32_t)ret, 32);
    if (ret == kIOReturnNotFound) {
        /* Newer hosts poll the cache while a physical scan is running. A
         * blanket EBUSY hides even existing BSS entries during repeated
         * scans. copyScanBSS returns a locked copy, safe to publish here.
         * An empty cache still reports busy; a partial list ends normally.
         */
        if (_scanInProgress && _scanCursor == 0)
            return kIOReturnBusy;
        if (_scanCursor == 0 && version_major >= 23 &&
            !_scanCacheBootstrapAttempted) {
            RTW88StateResult st = {};
            if (_ieee80211->cmdGetState(&st) == kIOReturnSuccess &&
                st.state == RTW88_STATE_IDLE) {
                _scanCacheBootstrapAttempted = true;
                _scanInProgress = true;
                IOReturn scanRet = _ieee80211->cmdScan();
                if (scanRet != kIOReturnSuccess) {
                    _scanInProgress = false;
                    IOLog("AirPort_RTW88: Sonoma cache bootstrap scan failed 0x%x\n", scanRet);
                    return scanRet;
                }
                IOLog("AirPort_RTW88: Sonoma cache-only SCAN_RESULT triggered bootstrap scan\n");
                return kIOReturnBusy;
            }
        }
        _scanCursor = 0;
        return 5; /* AirportItlwm end-of-list ABI */
    }
    if (ret) return ret;

    fillScanResultFromBSS(b, &_scanResult, false);
    _scanCursor++;
    setProperty("STA_V24_SCAN_RESULT_CURSOR", (uint64_t)_scanCursor, 32);
    setProperty("STA_V24_SCAN_RESULT_DURING_SCAN", _scanInProgress ? kOSBooleanTrue : kOSBooleanFalse);
    *out = &_scanResult;
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleCURRENT_NETWORK(struct apple80211_scan_result *out)
{
    static_assert(sizeof(apple80211_scan_result) == 1164, "Ventura CURRENT_NETWORK ABI changed");
    if (!out) return kIOReturnBadArgument;

    RTW88BSS current = {};
    IOReturn ret = _ieee80211->copyCurrentBSS(&current);
    if (ret != kIOReturnSuccess) {
        /* CoreWiFi/locationd asks selector 103 aggressively.  Do not turn a
         * valid RUN state into -3903 merely because the scan-cache copy was
         * evicted.  Reconstruct the minimum current BSS from the authoritative
         * connection state exposed by the Realtek backend. */
        RTW88StateResult st = {};
        IOReturn sr = _ieee80211->cmdGetState(&st);
        if (sr != kIOReturnSuccess || st.state != RTW88_STATE_CONNECTED ||
            st.channel == 0 || st.ssid[0] == '\0') {
            IOLog("AirPort_RTW88: CURRENT_NETWORK unavailable bss=0x%x state=0x%x run=%u ch=%u ssid0=%u\n",
                  ret, sr, st.state, st.channel, (unsigned)(uint8_t)st.ssid[0]);
            return ret != kIOReturnSuccess ? ret : kIOReturnNotReady;
        }

        current.ssid_len = (uint8_t)strnlen(st.ssid, 32);
        memcpy(current.ssid, st.ssid, current.ssid_len);
        current.ssid[current.ssid_len] = '\0';
        memcpy(current.bssid, st.bssid, sizeof(current.bssid));
        current.channel = (uint8_t)st.channel;
        current.rssi = (int16_t)st.rssi;
        current.beacon_interval = 100;
        IOLog("AirPort_RTW88: CURRENT_NETWORK reconstructed from connected state\n");
    }

    fillScanResultFromBSS(current, out, true);
    IOLog("AirPort_RTW88: CURRENT_NETWORK ssid_len=%u channel=%u rssi=%d ies=%d\n",
          out->asr_ssid_len, out->asr_channel.channel,
          out->asr_rssi, out->asr_ie_len);
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleSTATE(struct apple80211_state_data *out)
{
    if (!out) return kIOReturnBadArgument;
    struct RTW88StateResult st;
    IOReturn r = _ieee80211->cmdGetState(&st);
    if (r != kIOReturnSuccess) return r;
    bzero(out, sizeof(*out));
    out->version = APPLE80211_VERSION;
    switch (st.state) {
    case RTW88_STATE_SCANNING: out->state = APPLE80211_S_SCAN; break;
    case RTW88_STATE_AUTHENTICATING: out->state = APPLE80211_S_AUTH; break;
    case RTW88_STATE_ASSOCIATING:
    case RTW88_STATE_HANDSHAKING: out->state = APPLE80211_S_ASSOC; break;
    case RTW88_STATE_CONNECTED: out->state = APPLE80211_S_RUN; break;
    default: out->state = APPLE80211_S_INIT; break;
    }
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handleCHANNEL(struct apple80211_channel_data *out)
{
    if (!out) return kIOReturnBadArgument;
    RTW88StateResult st = {};
    IOReturn r = _ieee80211->cmdGetState(&st);
    if (r != kIOReturnSuccess) return r;
    if (st.state != RTW88_STATE_CONNECTED || st.channel == 0) return 6;
    bzero(out, sizeof(*out));
    out->version = APPLE80211_VERSION;
    out->channel.version = APPLE80211_VERSION;
    out->channel.channel = st.channel;
    const uint8_t width = _ieee80211->channelWidthMHz();
    out->channel.flags = APPLE80211_C_FLAG_ACTIVE |
        (width == 80 ? APPLE80211_C_FLAG_80MHZ :
         width == 40 ? APPLE80211_C_FLAG_40MHZ : APPLE80211_C_FLAG_20MHZ) |
        (st.channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
    return kIOReturnSuccess;
}

IOReturn AirportRTW88::handlePOWER(bool set, struct apple80211_power_data *d)
{
    if (!d) return kIOReturnBadArgument;
    if (set && __atomic_load_n(&_shutdown, __ATOMIC_ACQUIRE))
        return kIOReturnNotReady;
    if (set) {
        if (d->version != APPLE80211_VERSION || !d->num_radios ||
            d->num_radios > APPLE80211_MAX_RADIO)
            return kIOReturnBadArgument;
        UInt32 requested = d->power_state[0];
        if (requested != APPLE80211_POWER_ON && requested != APPLE80211_POWER_OFF)
            return kIOReturnUnsupported;
        // The device has one PHY. Never silently apply only the first of
        // contradictory radio states received from the family.
        for (UInt32 i = 1; i < d->num_radios; ++i)
            if (d->power_state[i] != requested) return kIOReturnUnsupported;
        IOReturn ret = requested == APPLE80211_POWER_ON ?
            _ieee80211->cmdPowerOn() : _ieee80211->cmdPowerOff();
        if (ret == kIOReturnSuccess && _netif)
            _netif->postMessage(APPLE80211_M_POWER_CHANGED);
        return ret;
    }
    RTW88StateResult st = {};
    IOReturn ret = _ieee80211->cmdGetState(&st);
    if (ret != kIOReturnSuccess) return ret;
    bzero(d, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->num_radios = 1;
    d->power_state[0] = st.powered ? APPLE80211_POWER_ON : APPLE80211_POWER_OFF;
    return kIOReturnSuccess;
}

SInt32 AirportRTW88::stopDMA()
{
    /* This callback can run with the IO80211 command gate held. Do not join
     * workers or recursively disable the interface here: they may need that
     * same gate. Fence device DMA before acknowledging the request. Rings stay
     * allocated; enable() restores bus mastering for a reversible stop. */
    __atomic_store_n(&_dmaStopped, true, __ATOMIC_RELEASE);
    if (_intrSrc) _intrSrc->disable();
    if (_pciDev) {
        _pciDev->setBusMasterEnable(false);
        if (_pciDev->configRead16(kIOPCIConfigCommand) & 0x0004)
            return kIOReturnError;
    }
    return kIOReturnSuccess;
}

void AirportRTW88::systemWillShutdown(IOOptionBits specifier)
{
    if (specifier == kIOMessageSystemWillPowerOff ||
        specifier == kIOMessageSystemWillRestart) {
        __atomic_store_n(&_shutdown, true, __ATOMIC_RELEASE);
        const SInt32 result = stopDMA();
        IOLog("AirportRTW88: shutdown DMA fence result=0x%x\n", result);
    }
    super::systemWillShutdown(specifier);
}

UInt32 AirportRTW88::hardwareOutputQueueDepth(IO80211Interface *interface)
{
    /* AirportItlwm reports 0 here; IO80211 owns the queueing policy. */
    return 0;
}

SInt32 AirportRTW88::performCountryCodeOperation(IO80211Interface *interface, IO80211CountryCodeOp op)
{
    /* Match AirportItlwm: acknowledge IO80211's country-code operation. */
    return kIOReturnSuccess;
}

SInt32 AirportRTW88::enableFeature(IO80211FeatureCode feature, void *data)
{
    /* Match AirportItlwm: AWDL service initialization does not depend on
     * acknowledging undocumented feature codes. Keep this truthful and
     * conservative now that the diagnostic feature-gate test is complete. */
    if (feature == kIO80211Feature80211n)
        return kIOReturnSuccess;
    return 102;
}

SInt32 AirportRTW88::monitorModeSetEnabled(IO80211Interface *interface,
                                            bool enabled,
                                            UInt32 mode)
{
    /* No monitor backend yet, but AirportItlwm returns success for this SPI. */
    return kIOReturnSuccess;
}

mbuf_t AirportRTW88::allocateInputPacket(uint32_t len)
{
    return allocatePacket(len);  /* helper heredado de IONetworkController */
}

void AirportRTW88::injectRxFrame(mbuf_t m)
{
    if (!m)
        return;

    if (!_netif) {
        mbuf_freem(m);
        return;
    }

    /* Submit each completed Ethernet packet directly. The former queue/flush
     * pair exposes packets to BPF before the later queue drain reaches IP.
     * Avoid retaining packets in the interface queue across link transitions.
     * A zero options mask means immediate input, not a zero-length packet. */
    size_t plen = mbuf_pkthdr_len(m);
    size_t mlen = mbuf_len(m);

    if (plen < 14 || plen > 4096 || mlen < 14 || mlen > 4096) {
        IOLog("AirportRTW88: dropping bogus RX mbuf pkthdr=%zu mlen=%zu\n",
              plen, mlen);
        mbuf_freem(m);
        return;
    }

    _netif->inputPacket(m, (UInt32)plen, 0);

    /* RX progress is an independent proof that the PCI datapath worker is
     * alive. If a TX-completion interrupt was coalesced after the BE ring
     * drained, re-check the watermark here so the macOS output queue cannot
     * remain parked indefinitely. */
    if (_txStalled)
        resumeTxIfStalled();
}

namespace {
/* AWDL management frames use an IEEE 802.11 Action header followed by the
 * Apple vendor header.  Do not feed arbitrary infrastructure action frames
 * (BlockAck, SA Query, etc.) to the AWDL peer manager. */
static bool rtw88IsAWDLActionFrame(const uint8_t *frame, uint32_t len,
                                   uint8_t *subtypeOut)
{
    static const uint8_t kAWDLBSSID[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};
    static const uint8_t kAppleOUI[3] = {0x00, 0x17, 0xf2};
    constexpr uint32_t kHdrLen = 24;
    constexpr uint32_t kFixedAWDLActionLen = 16; /* category+OUI+12-byte fixed body */

    if (!frame || len < kHdrLen + kFixedAWDLActionLen)
        return false;

    /* Management/Action subtype (little-endian frame-control low byte). */
    if ((frame[0] & 0xfcU) != 0xd0U)
        return false;

    /* addr3/BSSID must be Apple's well-known AWDL BSSID. */
    if (memcmp(frame + 16, kAWDLBSSID, sizeof(kAWDLBSSID)) != 0)
        return false;

    const uint8_t *a = frame + kHdrLen;
    if (a[0] != 0x7f || memcmp(a + 1, kAppleOUI, sizeof(kAppleOUI)) != 0 ||
        a[4] != 8)
        return false;

    /* byte 5 is the AWDL version; byte 6 is PSF(0) or MIF(3). */
    const uint8_t subtype = a[6];
    if (subtype != 0 && subtype != 3)
        return false;

    /* Reject impossible source addresses before creating peer state. */
    const uint8_t *sa = frame + 10;
    bool any = false;
    for (unsigned i = 0; i < 6; ++i) any |= sa[i] != 0;
    if (!any || (sa[0] & 0x01U))
        return false;

    if (subtypeOut)
        *subtypeOut = subtype;
    return true;
}

/* MacKernelSDK only carries an opaque/empty declaration for packet_info_tag.
 * Reserve a reasonably sized, zeroed backing store so IO80211 never reads
 * beyond a one-byte empty C++ placeholder if Ventura consults private fields. */
static packet_info_tag *rtw88ZeroPacketInfo(uint8_t (&storage)[64])
{
    bzero(storage, sizeof(storage));
    return reinterpret_cast<packet_info_tag *>(storage);
}
}

void AirportRTW88::injectRxActionFrame(const uint8_t *frame, uint32_t len,
                                       int8_t rssi, uint16_t channel)
{
    IO80211VirtualInterface *awdl =
        _awdlManager ? _awdlManager->awdlInterface() : nullptr;
    if (!awdl || !frame || len > 4096)
        return;

    uint8_t subtype = 0xff;
    if (!rtw88IsAWDLActionFrame(frame, len, &subtype))
        return;
    bool presenceDue = false;
    const bool validatedPeer = _awdlManager->observeAction(frame, len, &presenceDue);

    /* Only publish peers after the AWDL parser has seen enough state to mark
     * them usable (MIF + Version/device class). Earlier builds posted every
     * PSF/MIF candidate, which could leave IO80211 with transient/stale peers
     * that never had a working data plane. */
    IO80211P2PInterface *p2p = OSDynamicCast(IO80211P2PInterface, awdl);
    const uint8_t *sa = frame + 10;
    if (p2p && validatedPeer && presenceDue) {
        ether_addr peer = {};
        memcpy(peer.octet, sa, sizeof(peer.octet));
        (void)p2p->postPeerPresence(&peer, (int)rssi,
                                    (int)channel,
                                    (int)subtype, nullptr);
        ++_awdlPeerPresencePosts;
        setProperty("AWDL_PEER_PRESENCE_POSTS", (uint64_t)_awdlPeerPresencePosts, 32);
        /* A newly usable peer may unblock queued mDNS/IPv6 traffic on awdl0. */
        p2p->signalOutputThread();

#if __IO80211_TARGET >= __MAC_10_15
        /* The Ventura P2P interface exposes a peer IPv6 notification hook.
         * Keep the EUI-64 address as diagnostic data, but do not interpret the
         * method's return register as an IOReturn: the legacy private ABI has
         * no trustworthy parameter/return contract in the public SDK. */
        unsigned char ipv6[16] = { 0xfe, 0x80 };
        ipv6[8]  = sa[0] ^ 0x02;
        ipv6[9]  = sa[1];
        ipv6[10] = sa[2];
        ipv6[11] = 0xff;
        ipv6[12] = 0xfe;
        ipv6[13] = sa[3];
        ipv6[14] = sa[4];
        ipv6[15] = sa[5];
        (void)p2p->postPeerPresenceIPv6(&peer, (int)rssi,
                                        (int)channel,
                                        (int)subtype, nullptr,
                                        ipv6);
        ++_awdlPeerIPv6Posts;
        setProperty("AWDL_PEER_IPV6_POSTED", kOSBooleanTrue);
        setProperty("AWDL_PEER_IPV6_POSTS", (uint64_t)_awdlPeerIPv6Posts, 32);
        setProperty("AWDL_PEER_IPV6", ipv6, sizeof(ipv6));
#endif
    }

    mbuf_t m = allocatePacket(len);
    if (!m)
        return;
    if (mbuf_copyback(m, 0, len, frame, MBUF_DONTWAIT) != 0) {
        mbuf_freem(m);
        return;
    }
    mbuf_pkthdr_setlen(m, len);

    alignas(8) uint8_t tagStorage[64];
    UInt32 ret = awdl->inputPacket(m, rtw88ZeroPacketInfo(tagStorage));
    setProperty("AWDL_LAST_ACTION_INPUT_RESULT", (uint64_t)ret, 32);
    if (ret != kIOReturnSuccess && ret != kIOReturnOutputSuccess) {
        /* IO80211VirtualInterface owns the mbuf on accepted paths. On a
         * failure return it is safer not to double-free an ambiguously
         * consumed packet; the trace above records the Ventura result. */
    }
}

void AirportRTW88::injectRxAWDLFrame(mbuf_t m)
{
    if (!m) return;
    IO80211VirtualInterface *awdl = _awdlManager ? _awdlManager->awdlInterface() : nullptr;
    if (!awdl) {
        mbuf_freem(m);
        return;
    }
    const unsigned long packetLen = (unsigned long)mbuf_pkthdr_len(m);
    alignas(8) uint8_t tagStorage[64];
    UInt32 ret = awdl->inputPacket(m, rtw88ZeroPacketInfo(tagStorage));
    if (_awdlManager) _awdlManager->noteDataRX(ret);
    /* IO80211P2PInterface::inputPacket is a private ABI. Keep the raw value
     * as bounded telemetry only; do not flood the shutdown console. */
    setProperty("AWDL_LAST_DATA_INPUT_RAW", (uint64_t)ret, 32);
    (void)packetLen;
}

IOWorkLoop *AirportRTW88::getRxWorkLoop()
{
    return _workLoop;   /* miembro ya declarado en AirportRTW88.hpp */
}

bool AirportRTW88::setLinkStatus(UInt32 status, const IONetworkMedium *activeMedium,
                                  UInt64 speed, OSData *data)
{
    if (__atomic_load_n(&_shutdown, __ATOMIC_ACQUIRE)) return false;
    const bool ret = IO80211Controller::setLinkStatus(status, activeMedium, speed, data);
    const bool up = (status & kIONetworkLinkActive) != 0;

    if (_netif && (_linkUp != up || !(status & kIONetworkLinkNoNetworkChange))) {
        _linkUp = up;
        if (getCommandGate()) {
            getCommandGate()->runAction(
                [](OSObject *owner, void *value, void *reason, void *, void *) -> IOReturn {
                    auto *self = static_cast<AirportRTW88 *>(owner);
                    if (!self->_netif) return kIOReturnNotReady;
                    const bool linkUp = (uintptr_t)value != 0;
                    const unsigned int why = (unsigned int)(uintptr_t)reason;
                    IOReturn lr = self->_netif->setLinkState(
                        linkUp ? kIO80211NetworkLinkUp : kIO80211NetworkLinkDown,
                        linkUp ? 0U : why);
                    if (linkUp) {
                        self->_netif->setLinkQualityMetric(100);
                        /* Proven 1.0.0 STA sequencing: with the internal rtw88
                         * supplicant, ASSOCIATION is complete for CoreWiFi only
                         * when the controlled port is actually usable (open
                         * network immediately, WPA2 after M3/PTK+GTK). */
                        if (!self->_assocDoneReported) {
                            self->_assocDoneReported = true;
                            self->setProperty("STA_ASSOC_DONE_AT_LINK_UP", kOSBooleanTrue);
                            self->_netif->postMessage(APPLE80211_M_ASSOC_DONE);
                        }
                    } else {
                        self->_assocDoneReported = false;
                    }
                    return lr;
                },
                (void *)(uintptr_t)(up ? 1U : 0U),
                (void *)(uintptr_t)(up ? 0U : _ieee80211->deauthReason()));
        }
    }
    return ret;
}

void AirportRTW88::setLinkStatus(UInt32 status)
{
    /* RTW88RxDelegate entry point.  Route it through the full controller
     * override so registry link properties and IO80211 interface state change
     * together, as in AirportItlwm. */
    (void)setLinkStatus(status, nullptr, 0, nullptr);
}

void AirportRTW88::rtw88Event(RTW88Event ev, void *data)
{
    if (__atomic_load_n(&_shutdown, __ATOMIC_ACQUIRE) || !_netif) return;
    switch (ev) {
    case kRTW88EventScanDone: {
        setProperty("STA_V24_SCAN_COMPLETED", kOSBooleanTrue);
        _scanInProgress = false; _scanCursor = 0; UInt32 result = data ? *(UInt32 *)data : 0;
        if (_ieee80211) {
            setProperty("RADIO_SCAN_ENGINE", _ieee80211->scanEngine());
            setProperty("RADIO_SCAN_CHANNELS", (uint64_t)_ieee80211->scanChannelCount(), 32);
            setProperty("RADIO_SCAN_SW_VISITED_2GHZ", (uint64_t)_ieee80211->scanVisited2GHz(), 32);
            setProperty("RADIO_SCAN_SW_VISITED_5GHZ", (uint64_t)_ieee80211->scanVisited5GHz(), 32);
            setProperty("RADIO_SCAN_CACHE_COUNT", (uint64_t)_ieee80211->scanCacheCount(), 32);
            setProperty("RADIO_SCAN_COMPLETED_GENERATION", (uint64_t)_ieee80211->scanGeneration(), 32);
            setProperty("RADIO_SCAN_ABORTED", result != 0);
        }
        _netif->postMessage(APPLE80211_M_SCAN_DONE, &result, sizeof(result)); break;
    }
    case kRTW88EventAssocDone:
        setProperty("STA_ASSOC_DONE_EVENT", kOSBooleanTrue);
        setProperty("STA_ASSOC_DONE_RAW_STATE", (uint64_t)_ieee80211->rawState(), 32);
        setProperty("STA_ASSOC_DONE_VISIBLE", _ieee80211->associatedVisible() ? kOSBooleanTrue : kOSBooleanFalse);
        _netif->postMessage(APPLE80211_M_ASSOC_DONE);
        break;
    case kRTW88EventDeauth:
        setProperty("STA_DEAUTH_EVENT", kOSBooleanTrue);
        if (data) setProperty("STA_DEAUTH_REASON", (uint64_t)*(uint32_t *)data, 32);
        _netif->postMessage(APPLE80211_M_DEAUTH_RECEIVED);
        break;
    case kRTW88EventDisconnected:
        setProperty("STA_DISCONNECTED_EVENT", kOSBooleanTrue);
        _netif->postMessage(APPLE80211_M_LINK_CHANGED);
        break;
    case kRTW88EventRSSIChanged:   _netif->postMessage(APPLE80211_M_LINK_QUALITY); break;
    default: break;
    }
}
