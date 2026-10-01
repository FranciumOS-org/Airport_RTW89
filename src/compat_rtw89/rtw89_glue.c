// SPDX-License-Identifier: GPL-2.0
/*
 * Platform glue, Linux side: see rtw89_glue.h. Turns the platform's services
 * into the struct pci_dev and the PCI/DMA ops the compat layer expects, then
 * drives the driver's own pci_driver.
 */
#include "rtw89_net80211.h"
#include "rtw89_glue.h"
#include "core.h"

void rtw88_trigger_interrupt(void);
void rtw89_compat_debug_init(void);

/* ------------------------------------------------------------------ */
/*  State                                                               */
/* ------------------------------------------------------------------ */

/*
 * A DMA mapping. dma_alloc_coherent() memory is one platform allocation.
 * dma_map_single() of arbitrary kernel memory is done by bouncing: the device
 * gets a platform allocation and the data is copied in or out at map, sync
 * and unmap time (what Linux's swiotlb does), because kernel heap memory here
 * is neither guaranteed contiguous nor below 4 GB.
 */
struct rtw89_glue_dma {
    struct hlist_node node;     /* in dma_hash, keyed by bus address */
    u64 bus;
    void *virt;                 /* platform allocation */
    void *orig;                 /* caller's buffer for bounce mappings, else NULL */
    size_t size;
    void *cookie;
};

#define RTW89_GLUE_DMA_BUCKETS 256

static struct {
    bool active;
    struct rtw89_glue_platform plat;
    struct pci_dev pdev;
    struct pci_driver *drv;
    bool probed;

    spinlock_t dma_lock;
    struct hlist_head dma_hash[RTW89_GLUE_DMA_BUCKETS];
    unsigned int dma_count;
} glue;

/* ------------------------------------------------------------------ */
/*  PCI ops                                                             */
/* ------------------------------------------------------------------ */

static int glue_read_config_byte(struct pci_dev *dev, int where, u8 *val)
{
    *val = (u8)glue.plat.cfg_read(glue.plat.ctx, where, 1);
    return 0;
}

static int glue_read_config_word(struct pci_dev *dev, int where, u16 *val)
{
    *val = (u16)glue.plat.cfg_read(glue.plat.ctx, where, 2);
    return 0;
}

static int glue_read_config_dword(struct pci_dev *dev, int where, u32 *val)
{
    *val = glue.plat.cfg_read(glue.plat.ctx, where, 4);
    return 0;
}

static int glue_write_config_byte(struct pci_dev *dev, int where, u8 val)
{
    glue.plat.cfg_write(glue.plat.ctx, where, 1, val);
    return 0;
}

static int glue_write_config_word(struct pci_dev *dev, int where, u16 val)
{
    glue.plat.cfg_write(glue.plat.ctx, where, 2, val);
    return 0;
}

static int glue_write_config_dword(struct pci_dev *dev, int where, u32 val)
{
    glue.plat.cfg_write(glue.plat.ctx, where, 4, val);
    return 0;
}

/* The platform maps the BAR once; the driver just gets that mapping. */
static void *glue_ioremap(struct pci_dev *dev, int bar, size_t len)
{
    return (void *)dev->resource[bar];
}

static void glue_iounmap(struct pci_dev *dev, void *addr)
{
}

/* The platform picks the interrupt (MSI where available) on its own. */
static int glue_enable_msi(struct pci_dev *dev)
{
    return 0;
}

static void glue_disable_msi(struct pci_dev *dev)
{
}

/* Walk the classic capability list in the first 256 bytes of config space. */
static int glue_find_capability(struct pci_dev *dev, int cap)
{
    int ttl = 48;
    u16 status;
    u8 pos, id;

    glue_read_config_word(dev, PCI_STATUS, &status);
    if (!(status & PCI_STATUS_CAP_LIST))
        return 0;

    glue_read_config_byte(dev, PCI_CAPABILITY_LIST, &pos);
    while (ttl-- && pos >= 0x40) {
        pos &= ~3;
        glue_read_config_byte(dev, pos + PCI_CAP_LIST_ID, &id);
        if (id == 0xff)
            break;
        if (id == cap)
            return pos;
        glue_read_config_byte(dev, pos + PCI_CAP_LIST_NEXT, &pos);
    }
    return 0;
}

static struct pci_ops_rtw88 glue_pci_ops = {
    .read_config_byte    = glue_read_config_byte,
    .read_config_word    = glue_read_config_word,
    .read_config_dword   = glue_read_config_dword,
    .write_config_byte   = glue_write_config_byte,
    .write_config_word   = glue_write_config_word,
    .write_config_dword  = glue_write_config_dword,
    .ioremap             = glue_ioremap,
    .iounmap             = glue_iounmap,
    .enable_msi          = glue_enable_msi,
    .disable_msi         = glue_disable_msi,
    .pci_find_capability = glue_find_capability,
};

/* ------------------------------------------------------------------ */
/*  DMA ops                                                             */
/* ------------------------------------------------------------------ */

static struct hlist_head *glue_dma_bucket(u64 bus)
{
    /* Platform allocations are page-aligned, so skip the low bits. */
    return &glue.dma_hash[(bus >> 12) % RTW89_GLUE_DMA_BUCKETS];
}

static struct rtw89_glue_dma *glue_dma_new(size_t size, void *orig)
{
    struct rtw89_glue_dma *d;

    d = kzalloc(sizeof(*d), GFP_KERNEL);
    if (!d)
        return NULL;

    d->virt = glue.plat.dma_alloc(glue.plat.ctx, size, &d->bus, &d->cookie);
    if (!d->virt) {
        kfree(d);
        return NULL;
    }
    d->size = size;
    d->orig = orig;

    spin_lock(&glue.dma_lock);
    hlist_add_head(&d->node, glue_dma_bucket(d->bus));
    glue.dma_count++;
    spin_unlock(&glue.dma_lock);

    return d;
}

/* Look a mapping up by bus address; unlink it if @remove. */
static struct rtw89_glue_dma *glue_dma_find(u64 bus, bool remove)
{
    struct rtw89_glue_dma *d, *found = NULL;

    spin_lock(&glue.dma_lock);
    hlist_for_each_entry(d, glue_dma_bucket(bus), node) {
        if (d->bus == bus) {
            found = d;
            break;
        }
    }
    if (found && remove) {
        hlist_del_init(&found->node);
        glue.dma_count--;
    }
    spin_unlock(&glue.dma_lock);

    return found;
}

static void glue_dma_release(struct rtw89_glue_dma *d)
{
    glue.plat.dma_free(glue.plat.ctx, d->cookie);
    kfree(d);
}

static void *glue_dma_alloc_coherent(struct device *dev, size_t size,
                                     dma_addr_t *dma_handle, gfp_t flag)
{
    struct rtw89_glue_dma *d = glue_dma_new(size, NULL);

    if (!d)
        return NULL;
    memset(d->virt, 0, size);
    *dma_handle = (dma_addr_t)d->bus;
    return d->virt;
}

static void glue_dma_free_coherent(struct device *dev, size_t size,
                                   void *cpu_addr, dma_addr_t dma_handle)
{
    struct rtw89_glue_dma *d;

    /* Like Linux: freeing nothing is fine (rtw89 does it for rings it never
     * allocated, e.g. the firmware-command channel has no WD ring). */
    if (!cpu_addr)
        return;

    d = glue_dma_find(dma_handle, true);
    if (WARN_ON(!d))
        return;
    glue_dma_release(d);
}

static dma_addr_t glue_dma_map_single(struct device *dev, void *ptr,
                                      size_t size, int dir)
{
    struct rtw89_glue_dma *d;

    if (!ptr || !size)
        return 0;   /* dma_mapping_error() treats 0 as failure */

    d = glue_dma_new(size, ptr);
    if (!d)
        return 0;
    if (dir != DMA_FROM_DEVICE)
        memcpy(d->virt, ptr, size);
    return (dma_addr_t)d->bus;
}

static void glue_dma_unmap_single(struct device *dev, dma_addr_t addr,
                                  size_t size, int dir)
{
    struct rtw89_glue_dma *d = glue_dma_find(addr, true);

    if (WARN_ON(!d))
        return;
    if (dir != DMA_TO_DEVICE && d->orig)
        memcpy(d->orig, d->virt, min(size, d->size));
    glue_dma_release(d);
}

static void glue_dma_sync_for_cpu(struct device *dev, dma_addr_t addr,
                                  size_t size, int dir)
{
    struct rtw89_glue_dma *d = glue_dma_find(addr, false);

    if (d && d->orig && dir != DMA_TO_DEVICE)
        memcpy(d->orig, d->virt, min(size, d->size));
}

static void glue_dma_sync_for_device(struct device *dev, dma_addr_t addr,
                                     size_t size, int dir)
{
    struct rtw89_glue_dma *d = glue_dma_find(addr, false);

    if (d && d->orig && dir != DMA_FROM_DEVICE)
        memcpy(d->virt, d->orig, min(size, d->size));
}

static struct rtw88_dma_alloc_ops glue_dma_ops = {
    .alloc_coherent         = glue_dma_alloc_coherent,
    .free_coherent          = glue_dma_free_coherent,
    .map_single             = glue_dma_map_single,
    .unmap_single           = glue_dma_unmap_single,
    .sync_single_for_cpu    = glue_dma_sync_for_cpu,
    .sync_single_for_device = glue_dma_sync_for_device,
};

/* Free what the driver left mapped; returns how many there were. */
static unsigned int glue_dma_reap(void)
{
    struct rtw89_glue_dma *d;
    unsigned int n = 0;
    int i;

    for (i = 0; i < RTW89_GLUE_DMA_BUCKETS; i++) {
        for (;;) {
            spin_lock(&glue.dma_lock);
            d = hlist_empty(&glue.dma_hash[i]) ? NULL :
                container_of(glue.dma_hash[i].first, struct rtw89_glue_dma, node);
            if (d) {
                hlist_del_init(&d->node);
                glue.dma_count--;
            }
            spin_unlock(&glue.dma_lock);
            if (!d)
                break;
            glue_dma_release(d);
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Probe / remove                                                      */
/* ------------------------------------------------------------------ */

static const struct pci_device_id *glue_match(uint16_t vendor, uint16_t device)
{
    const struct pci_device_id *id = rtw89_compat_pci_driver()->id_table;

    for (; id->vendor; id++)
        if (id->vendor == vendor && id->device == device)
            return id;
    return NULL;
}

bool rtw89_glue_supports(uint16_t vendor, uint16_t device)
{
    return glue_match(vendor, device) != NULL;
}

static void glue_teardown(void)
{
    unsigned int leaked;

    rtw88_compat_exit();

    leaked = glue_dma_reap();
    if (leaked)
        IOLog("[rtw89] released %u DMA mapping(s) the driver left behind\n", leaked);
    spin_lock_destroy(&glue.dma_lock);

    rtw88_pci_io_ops = NULL;
    rtw88_dma_ops = NULL;
    memset(&glue, 0, sizeof(glue));
}

int rtw89_glue_probe(const struct rtw89_glue_platform *platform,
                     const struct rtw89_glue_device *device)
{
    const struct pci_device_id *id;
    int ret, i;

    if (glue.active)
        return -EBUSY;
    if (!platform->cfg_read || !platform->cfg_write || !platform->dma_alloc ||
        !platform->dma_free || !device->mmio_base || !device->mmio_len)
        return -EINVAL;

    id = glue_match(device->vendor, device->device);
    if (!id)
        return -ENODEV;

    ret = rtw88_compat_init();
    if (ret)
        return ret;
    rtw89_compat_debug_init();

    memset(&glue, 0, sizeof(glue));
    glue.active = true;
    glue.plat = *platform;
    glue.drv = rtw89_compat_pci_driver();
    spin_lock_init(&glue.dma_lock);
    for (i = 0; i < RTW89_GLUE_DMA_BUCKETS; i++)
        INIT_HLIST_HEAD(&glue.dma_hash[i]);

    glue.pdev.vendor = device->vendor;
    glue.pdev.device = device->device;
    glue.pdev.subsystem_vendor = device->subsystem_vendor;
    glue.pdev.subsystem_device = device->subsystem_device;
    glue.pdev.revision = device->revision;
    glue.pdev.dev.name = glue.drv->name;
    /* rtw89 uses BAR 2 on every PCIe chip (rtw89_pci_setup_mapping). */
    glue.pdev.resource[2] = (resource_size_t)(uintptr_t)device->mmio_base;
    glue.pdev.resource_len[2] = device->mmio_len;

    rtw88_pci_io_ops = &glue_pci_ops;
    rtw88_dma_ops = &glue_dma_ops;

    ret = glue.drv->probe(&glue.pdev, id);
    if (ret) {
        glue_teardown();
        return ret;
    }

    glue.probed = true;
    return 0;
}

void rtw89_glue_remove(void)
{
    if (!glue.active)
        return;

    if (glue.probed) {
        glue.probed = false;
        glue.drv->remove(&glue.pdev);
    }
    glue_teardown();
}

void rtw89_glue_interrupt(void)
{
    if (glue.probed)
        rtw88_trigger_interrupt();
}

bool rtw89_glue_get_info(struct rtw89_glue_info *info)
{
    struct ieee80211_hw *hw;
    struct rtw89_dev *rtwdev;
    struct rtw89_fw_suit *suit;

    if (!glue.probed)
        return false;

    hw = pci_get_drvdata(&glue.pdev);
    rtwdev = hw->priv;
    suit = rtw89_fw_suit_get(rtwdev, RTW89_FW_NORMAL);

    memset(info, 0, sizeof(*info));
    memcpy(info->mac, hw->wiphy->perm_addr, ETH_ALEN);
    snprintf(info->fw_version, sizeof(info->fw_version), "%u.%u.%u.%u",
             suit->major_ver, suit->minor_ver, suit->sub_ver, suit->sub_idex);
    info->fw_commit = suit->commitid;
    info->chip_cut = rtwdev->hal.cv;
    info->rfe_type = rtwdev->efuse.rfe_type;
    info->tx_streams = rtwdev->hal.tx_nss;
    info->rx_streams = rtwdev->hal.rx_nss;
    return true;
}
