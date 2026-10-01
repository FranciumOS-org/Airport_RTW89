// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// Hardware shims shared by the Ethernet and Airport controller frontends.

#include "RTW88IEEE80211.hpp"
#include <IOKit/IOLib.h>

extern "C" {
#include "../compat/rtw88_compat.h"
}

RTW88HwOps *g_pci_dev_instance = nullptr;

static int compat_pci_read_config_byte(struct pci_dev *, int where, u8 *val)
{
    if (!g_pci_dev_instance) { *val = 0xff; return -1; }
    *val = g_pci_dev_instance->pciReadByte(where);
    return 0;
}

static int compat_pci_read_config_word(struct pci_dev *, int where, u16 *val)
{
    if (!g_pci_dev_instance) { *val = 0xffff; return -1; }
    *val = g_pci_dev_instance->pciReadWord(where);
    return 0;
}

static int compat_pci_read_config_dword(struct pci_dev *, int where, u32 *val)
{
    if (!g_pci_dev_instance) { *val = 0xffffffff; return -1; }
    *val = g_pci_dev_instance->pciReadDword(where);
    return 0;
}

static int compat_pci_write_config_byte(struct pci_dev *, int where, u8 val)
{
    if (!g_pci_dev_instance) return -1;
    g_pci_dev_instance->pciWriteByte(where, val);
    return 0;
}

static int compat_pci_write_config_word(struct pci_dev *, int where, u16 val)
{
    if (!g_pci_dev_instance) return -1;
    g_pci_dev_instance->pciWriteWord(where, val);
    return 0;
}

static int compat_pci_write_config_dword(struct pci_dev *, int where, u32 val)
{
    if (!g_pci_dev_instance) return -1;
    g_pci_dev_instance->pciWriteDword(where, val);
    return 0;
}

static void *compat_ioremap(struct pci_dev *, int, size_t)
{
    return g_pci_dev_instance ? (void *)g_pci_dev_instance->mmioBase() : nullptr;
}

static void compat_iounmap(struct pci_dev *, void *) {}
static int compat_enable_msi(struct pci_dev *) { return 0; }
static void compat_disable_msi(struct pci_dev *) {}

static int compat_pci_find_capability(struct pci_dev *, int cap)
{
    return g_pci_dev_instance ? g_pci_dev_instance->pciFindCapability(cap) : 0;
}

struct pci_ops_rtw88 _pci_io_ops = {
    .read_config_byte = compat_pci_read_config_byte,
    .read_config_word = compat_pci_read_config_word,
    .read_config_dword = compat_pci_read_config_dword,
    .write_config_byte = compat_pci_write_config_byte,
    .write_config_word = compat_pci_write_config_word,
    .write_config_dword = compat_pci_write_config_dword,
    .ioremap = compat_ioremap,
    .iounmap = compat_iounmap,
    .enable_msi = compat_enable_msi,
    .disable_msi = compat_disable_msi,
    .pci_find_capability = compat_pci_find_capability,
};

static void *compat_dma_alloc(struct device *, size_t size,
                              dma_addr_t *dma_handle, gfp_t)
{
    if (!g_pci_dev_instance) return nullptr;
    IOPhysicalAddress phys = 0;
    void *virt = g_pci_dev_instance->allocCoherent(size, &phys);
    if (dma_handle) *dma_handle = (dma_addr_t)phys;
    return virt;
}

static void compat_dma_free(struct device *, size_t size, void *cpu_addr,
                            dma_addr_t dma_handle)
{
    if (g_pci_dev_instance)
        g_pci_dev_instance->freeCoherent(size, cpu_addr,
                                         (IOPhysicalAddress)dma_handle);
}

static dma_addr_t compat_dma_map(struct device *, void *ptr, size_t size, int dir)
{
    if (!g_pci_dev_instance || !ptr) return 0;

    IOPhysicalAddress phys = 0;
    void *bounce = g_pci_dev_instance->allocCoherent(size, &phys);
    if (!bounce) return 0;

    g_pci_dev_instance->setBounceOrigVA(phys, ptr);
    if (dir == 1 || dir == 2)
        memcpy(bounce, ptr, size);
    return (dma_addr_t)phys;
}

static void compat_dma_unmap(struct device *, dma_addr_t addr, size_t, int)
{
    if (g_pci_dev_instance)
        g_pci_dev_instance->freeCoherentByPhys((IOPhysicalAddress)addr);
}

static void compat_dma_sync_cpu(struct device *, dma_addr_t addr,
                                size_t size, int dir)
{
    if (dir == 0 && g_pci_dev_instance)
        g_pci_dev_instance->syncBounceForCpu((IOPhysicalAddress)addr, size);
}

static void compat_dma_sync_dev(struct device *, dma_addr_t, size_t, int) {}

struct rtw88_dma_alloc_ops _dma_ops = {
    .alloc_coherent = compat_dma_alloc,
    .free_coherent = compat_dma_free,
    .map_single = compat_dma_map,
    .unmap_single = compat_dma_unmap,
    .sync_single_for_cpu = compat_dma_sync_cpu,
    .sync_single_for_device = compat_dma_sync_dev,
};
