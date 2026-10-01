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
#define RTW89_GLUE_MAX_BSS 128
#define RTW89_GLUE_MAX_IES 1024

/* A network heard in a scan: what callers see plus what a join needs. */
struct rtw89_glue_bss_entry {
    struct rtw89_glue_bss pub;
    u16 beacon_int;
    u16 ies_len;
    u8 ies[RTW89_GLUE_MAX_IES];
};

static struct {
    bool active;
    struct rtw89_glue_platform plat;
    struct pci_dev pdev;
    struct pci_driver *drv;
    bool probed;

    spinlock_t dma_lock;
    struct hlist_head dma_hash[RTW89_GLUE_DMA_BUCKETS];
    unsigned int dma_count;

    /* radio up: one station interface */
    bool up;
    struct ieee80211_vif *vif;

    /* scan */
    bool scanning;
    struct ieee80211_scan_request *scan_req;    /* kept until the next scan */
    spinlock_t bss_lock;
    struct rtw89_glue_bss_entry bss[RTW89_GLUE_MAX_BSS];
    unsigned int n_bss;
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

    kfree(glue.scan_req);
    spin_lock_destroy(&glue.bss_lock);

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
    spin_lock_init(&glue.bss_lock);
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

    rtw89_glue_down();

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

/* ------------------------------------------------------------------ */
/*  Upcalls from the mac80211 stand-in                                  */
/* ------------------------------------------------------------------ */

static struct ieee80211_hw *glue_hw(void)
{
    return pci_get_drvdata(&glue.pdev);
}

/*
 * Remember a network from a beacon or probe response. Non-static so the smoke
 * test can feed it frames; everything else reaches it through glue_rx().
 */
void rtw89_glue_note_bss(const u8 *frame, size_t len, u16 freq, s8 signal);
void rtw89_glue_note_bss(const u8 *frame, size_t len, u16 freq, s8 signal)
{
    const struct ieee80211_mgmt *mgmt = (const void *)frame;
    const size_t fixed = offsetof(struct ieee80211_mgmt, u.beacon.variable);
    struct rtw89_glue_bss_entry *e = NULL;
    const struct element *ssid;
    bool has_ssid;
    unsigned int i;

    if (len < fixed)
        return;
    if (!ieee80211_is_beacon(mgmt->frame_control) &&
        !ieee80211_is_probe_resp(mgmt->frame_control))
        return;

    ssid = cfg80211_find_elem(WLAN_EID_SSID, frame + fixed, len - fixed);
    /* A hidden network beacons an empty or all-zero SSID. */
    has_ssid = ssid && ssid->datalen && ssid->datalen <= IEEE80211_MAX_SSID_LEN &&
               ssid->data[0];

    spin_lock(&glue.bss_lock);
    for (i = 0; i < glue.n_bss; i++) {
        if (ether_addr_equal(glue.bss[i].pub.bssid, mgmt->bssid)) {
            e = &glue.bss[i];
            break;
        }
    }
    if (!e && glue.n_bss < RTW89_GLUE_MAX_BSS) {
        e = &glue.bss[glue.n_bss++];
        memset(e, 0, sizeof(*e));
        memcpy(e->pub.bssid, mgmt->bssid, ETH_ALEN);
        e->pub.signal = signal;
    }
    if (e) {
        if (has_ssid) {
            e->pub.ssid_len = ssid->datalen;
            memcpy(e->pub.ssid, ssid->data, ssid->datalen);
            e->pub.ssid[ssid->datalen] = 0;
        }
        /* Keep the elements of a frame that names the network, if any did. */
        if ((has_ssid || !e->pub.ssid_len) && len - fixed <= RTW89_GLUE_MAX_IES) {
            e->ies_len = (u16)(len - fixed);
            memcpy(e->ies, frame + fixed, len - fixed);
            e->beacon_int = le16_to_cpu(mgmt->u.beacon.beacon_int);
            e->pub.capability = le16_to_cpu(mgmt->u.beacon.capab_info);
        }
        e->pub.freq = freq;
        e->pub.channel = (u8)ieee80211_frequency_to_channel(freq);
        if (signal > e->pub.signal)
            e->pub.signal = signal;
        e->pub.seen++;
    }
    spin_unlock(&glue.bss_lock);
}

/* Every received frame lands here until there is a network stack to give it to. */
static void glue_rx(void *ctx, struct ieee80211_sta *sta, struct sk_buff *skb)
{
    struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
    const struct ieee80211_hdr *hdr = (const void *)skb->data;
    size_t len = skb->len;

    if ((status->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_NO_PSDU)) ||
        len < sizeof(struct ieee80211_hdr_3addr) + FCS_LEN) {
        kfree_skb(skb);
        return;
    }

    if (ieee80211_is_beacon(hdr->frame_control) ||
        ieee80211_is_probe_resp(hdr->frame_control)) {
        /* The driver leaves the FCS on (RX_INCLUDES_FCS). */
        rtw89_glue_note_bss(skb->data, len - FCS_LEN, status->freq, status->signal);
        kfree_skb(skb);
        return;
    }

    /* Everything else is for the station MLME, which takes the skb. */
    rtw89_mlme_rx(skb);
}

/*
 * Feed one received 802.11 frame (without FCS) through the same path the
 * driver uses. For the userspace smoke test, which plays the access point.
 */
void rtw89_glue_test_rx(const u8 *frame, size_t len, u16 freq, s8 signal);
void rtw89_glue_test_rx(const u8 *frame, size_t len, u16 freq, s8 signal)
{
    struct ieee80211_rx_status *status;
    struct sk_buff *skb;

    if (!glue.probed)
        return;
    skb = dev_alloc_skb(len + FCS_LEN);
    if (!skb)
        return;
    skb_put_data(skb, frame, len);
    skb_put_zero(skb, FCS_LEN);
    status = IEEE80211_SKB_RXCB(skb);
    memset(status, 0, sizeof(*status));
    status->freq = freq;
    status->signal = signal;
    ieee80211_rx_napi(glue_hw(), NULL, skb, NULL);
}

static void glue_tx_status(void *ctx, struct sk_buff *skb)
{
    kfree_skb(skb);
}

static void glue_scan_done(void *ctx, bool aborted)
{
    glue.scanning = false;
    IOLog("[rtw89] scan %s: %u network(s) heard\n", aborted ? "aborted" : "finished",
          glue.n_bss);
}

static const struct rtw89_m80211_glue_ops glue_m80211_ops = {
    .rx = glue_rx,
    .tx_status = glue_tx_status,
    .scan_done = glue_scan_done,
};

/* ------------------------------------------------------------------ */
/*  Radio up / down                                                     */
/* ------------------------------------------------------------------ */

bool rtw89_glue_is_up(void)
{
    return glue.up;
}

int rtw89_glue_up(void)
{
    struct rtw89_m80211_local *local;
    struct ieee80211_hw *hw;
    unsigned int filter = 0;
    int ret;

    if (!glue.probed)
        return -ENODEV;
    if (glue.up)
        return 0;

    hw = glue_hw();
    local = hw_to_local(hw);
    rtw89_m80211_set_glue(hw, &glue_m80211_ops, NULL);

    glue.vif = rtw89_m80211_vif_alloc(hw, NL80211_IFTYPE_STATION, hw->wiphy->perm_addr);
    if (!glue.vif)
        return -ENOMEM;

    /* The driver enables the chip's interrupt mask inside start(). */
    if (glue.plat.irq_enable)
        glue.plat.irq_enable(glue.plat.ctx, true);

    /* mac80211 calls every driver op with the wiphy mutex held. */
    wiphy_lock(hw->wiphy);
    ret = local->ops->start(hw);
    if (ret)
        goto err_unlock;

    ret = local->ops->add_interface(hw, glue.vif);
    if (ret) {
        local->ops->stop(hw, false);
        goto err_unlock;
    }
    rtw89_m80211_vif_set_in_driver(glue.vif, true);
    local->ops->configure_filter(hw, 0, &filter, 0);
    rtw89_mlme_start(hw, glue.vif);
    wiphy_unlock(hw->wiphy);

    glue.up = true;
    return 0;

err_unlock:
    wiphy_unlock(hw->wiphy);
    if (glue.plat.irq_enable)
        glue.plat.irq_enable(glue.plat.ctx, false);
    rtw89_m80211_vif_free(glue.vif);
    glue.vif = NULL;
    return ret;
}

void rtw89_glue_down(void)
{
    struct rtw89_m80211_local *local;
    struct ieee80211_hw *hw;

    if (!glue.up)
        return;
    glue.up = false;

    hw = glue_hw();
    local = hw_to_local(hw);

    wiphy_lock(hw->wiphy);
    if (glue.scanning && local->ops->cancel_hw_scan)
        local->ops->cancel_hw_scan(hw, glue.vif);
    rtw89_mlme_stop();
    rtw89_m80211_vif_set_in_driver(glue.vif, false);
    local->ops->remove_interface(hw, glue.vif);
    local->ops->stop(hw, false);
    wiphy_unlock(hw->wiphy);
    glue.scanning = false;

    if (glue.plat.irq_enable)
        glue.plat.irq_enable(glue.plat.ctx, false);

    rtw89_m80211_vif_free(glue.vif);
    glue.vif = NULL;
}

/* ------------------------------------------------------------------ */
/*  Scan                                                                */
/* ------------------------------------------------------------------ */

/* Probe request IEs per band: supported rates only for now. */
static const u8 glue_ies_2ghz[] = {
    WLAN_EID_SUPP_RATES, 8, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,
    WLAN_EID_EXT_SUPP_RATES, 4, 0x30, 0x48, 0x60, 0x6c,
};
static const u8 glue_ies_5ghz[] = {
    WLAN_EID_SUPP_RATES, 8, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,
};

bool rtw89_glue_scanning(void)
{
    return glue.scanning;
}

int rtw89_glue_scan(void)
{
    struct rtw89_m80211_local *local;
    struct ieee80211_scan_request *sreq;
    struct ieee80211_supported_band *sband;
    struct cfg80211_scan_request *req;
    struct cfg80211_ssid *ssid;
    struct ieee80211_hw *hw;
    unsigned int n_channels = 0;
    u8 *ies;
    int band, i, ret;

    if (!glue.up)
        return -ENETDOWN;
    if (glue.scanning)
        return -EBUSY;

    hw = glue_hw();
    local = hw_to_local(hw);

    for (band = 0; band < NUM_NL80211_BANDS; band++)
        if (hw->wiphy->bands[band])
            n_channels += hw->wiphy->bands[band]->n_channels;

    /* One allocation: request, channel pointers, one wildcard SSID, the IEs. */
    sreq = kzalloc(sizeof(*sreq) + n_channels * sizeof(sreq->req.channels[0]) +
                   sizeof(*ssid) + sizeof(glue_ies_2ghz) + sizeof(glue_ies_5ghz),
                   GFP_KERNEL);
    if (!sreq)
        return -ENOMEM;
    req = &sreq->req;
    ssid = (void *)&req->channels[n_channels];
    ies = (u8 *)(ssid + 1);

    for (band = 0; band < NUM_NL80211_BANDS; band++) {
        sband = hw->wiphy->bands[band];
        if (!sband)
            continue;
        for (i = 0; i < sband->n_channels; i++)
            if (!(sband->channels[i].flags & IEEE80211_CHAN_DISABLED))
                req->channels[req->n_channels++] = &sband->channels[i];
    }

    req->ssids = ssid;          /* zero length: the wildcard SSID */
    req->n_ssids = 1;
    req->wiphy = hw->wiphy;
    req->scan_start = jiffies;
    ether_addr_copy(req->mac_addr, glue.vif->addr);
    eth_broadcast_addr(req->bssid);

    memcpy(ies, glue_ies_2ghz, sizeof(glue_ies_2ghz));
    memcpy(ies + sizeof(glue_ies_2ghz), glue_ies_5ghz, sizeof(glue_ies_5ghz));
    req->ie = ies;
    req->ie_len = sizeof(glue_ies_2ghz) + sizeof(glue_ies_5ghz);
    sreq->ies.ies[NL80211_BAND_2GHZ] = ies;
    sreq->ies.len[NL80211_BAND_2GHZ] = sizeof(glue_ies_2ghz);
    sreq->ies.ies[NL80211_BAND_5GHZ] = ies + sizeof(glue_ies_2ghz);
    sreq->ies.len[NL80211_BAND_5GHZ] = sizeof(glue_ies_5ghz);

    spin_lock(&glue.bss_lock);
    glue.n_bss = 0;
    spin_unlock(&glue.bss_lock);

    wiphy_lock(hw->wiphy);
    /* The previous request is no longer referenced once a new scan starts. */
    kfree(glue.scan_req);
    glue.scan_req = sreq;
    glue.scanning = true;
    ret = local->ops->hw_scan(hw, glue.vif, sreq);
    if (ret)
        glue.scanning = false;
    wiphy_unlock(hw->wiphy);

    if (ret > 0)    /* "do a software scan instead": the firmware lacks scan offload */
        ret = -EOPNOTSUPP;
    if (!ret)
        IOLog("[rtw89] scan started on %u channel(s)\n", req->n_channels);
    return ret;
}

unsigned int rtw89_glue_scan_results(struct rtw89_glue_bss *out, unsigned int max)
{
    unsigned int n;

    if (!glue.active)
        return 0;

    spin_lock(&glue.bss_lock);
    n = min(glue.n_bss, max);
    for (unsigned int i = 0; i < n; i++)
        out[i] = glue.bss[i].pub;
    spin_unlock(&glue.bss_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/*  Join / leave                                                        */
/* ------------------------------------------------------------------ */

int rtw89_glue_join(const uint8_t *ssid, size_t ssid_len)
{
    static u8 ies[RTW89_GLUE_MAX_IES];  /* calls are serialised by the caller */
    struct rtw89_glue_bss_entry *best = NULL;
    struct rtw89_mlme_bss bss = {};
    struct ieee80211_hw *hw;
    unsigned int i;
    int ret;

    if (!glue.up)
        return -ENETDOWN;
    if (!ssid_len || ssid_len > IEEE80211_MAX_SSID_LEN)
        return -EINVAL;

    spin_lock(&glue.bss_lock);
    for (i = 0; i < glue.n_bss; i++) {
        struct rtw89_glue_bss_entry *e = &glue.bss[i];

        if (e->pub.ssid_len != ssid_len || memcmp(e->pub.ssid, ssid, ssid_len) ||
            !e->ies_len)
            continue;
        if (!best || e->pub.signal > best->pub.signal)
            best = e;
    }
    if (best) {
        memcpy(bss.bssid, best->pub.bssid, ETH_ALEN);
        memcpy(bss.ssid, ssid, ssid_len);
        bss.ssid_len = (u8)ssid_len;
        bss.freq = best->pub.freq;
        bss.capability = best->pub.capability;
        bss.beacon_int = best->beacon_int;
        memcpy(ies, best->ies, best->ies_len);
        bss.ies = ies;
        bss.ies_len = best->ies_len;
    }
    spin_unlock(&glue.bss_lock);
    if (!best)
        return -ENOENT;

    hw = glue_hw();
    wiphy_lock(hw->wiphy);
    ret = rtw89_mlme_connect(&bss);
    wiphy_unlock(hw->wiphy);
    return ret;
}

void rtw89_glue_leave(void)
{
    struct ieee80211_hw *hw;

    if (!glue.up)
        return;
    hw = glue_hw();
    wiphy_lock(hw->wiphy);
    rtw89_mlme_disconnect(WLAN_REASON_DEAUTH_LEAVING);
    wiphy_unlock(hw->wiphy);
}

void rtw89_glue_link(struct rtw89_glue_link *link)
{
    struct rtw89_mlme_status st;

    memset(link, 0, sizeof(*link));
    if (!glue.up)
        return;

    rtw89_mlme_get_status(&st);
    switch (st.state) {
    case RTW89_MLME_AUTHENTICATING:
    case RTW89_MLME_ASSOCIATING:
        link->state = RTW89_GLUE_LINK_JOINING;
        break;
    case RTW89_MLME_ASSOCIATED:
        link->state = RTW89_GLUE_LINK_ASSOCIATED;
        break;
    case RTW89_MLME_CONNECTED:
        link->state = RTW89_GLUE_LINK_CONNECTED;
        break;
    default:
        link->state = RTW89_GLUE_LINK_DOWN;
        break;
    }
    memcpy(link->bssid, st.bssid, ETH_ALEN);
    memcpy(link->ssid, st.ssid, sizeof(link->ssid));
    link->freq = st.freq;
    link->aid = st.aid;
    link->last_error = st.last_error;
    link->eapol_rx = st.eapol_rx;
}
