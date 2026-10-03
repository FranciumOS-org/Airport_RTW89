// SPDX-License-Identifier: GPL-2.0
/*
 * Platform glue, Linux side: see rtw89_glue.h. Turns the platform's services
 * into the struct pci_dev and the PCI/DMA ops the compat layer expects, then
 * drives the driver's own pci_driver.
 */
#include "rtw89_net80211.h"
#include "rtw89_glue.h"
#include "rtw89_crypto.h"
#include "core.h"
#include "coex.h"
#include "phy.h"

void rtw88_trigger_interrupt(void);
void rtw89_compat_debug_init(void);
/* src/compat/rtw88_compat.c */
extern void (*rtw88_napi_post_poll)(int done);
bool rtw88_queue_datapath_delayed_work(struct delayed_work *dwork, unsigned long delay);
/* rtw89_core_wrap.c */
unsigned int rtw89_compat_flush_ppdu_rx(struct rtw89_dev *rtwdev);

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
    unsigned long last_seen;    /* jiffies of the last frame heard from it */
    u32 scan_gen;               /* the scan it was last heard in */
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
    bool have_mac;                      /* rtw89_glue_set_mac() */
    u8 mac[ETH_ALEN];

    /* scan */
    bool scanning;
    struct ieee80211_scan_request *scan_req;    /* kept until the next scan */
    spinlock_t bss_lock;
    struct rtw89_glue_bss_entry bss[RTW89_GLUE_MAX_BSS];
    unsigned int n_bss;
    u32 scan_gen;               /* counts the scans started */

    /* The network the user asked for, kept to join it again when the
     * connection is lost: once it has worked, so that a wrong password is
     * not tried for ever. */
    struct mutex cmd_lock;              /* up, down, scan, join, leave and the rejoin work */
    struct {
        bool valid;
        bool proven;                    /* has been connected with these */
        u8 ssid[IEEE80211_MAX_SSID_LEN];
        u8 ssid_len;
        bool have_pmk;
        u8 pmk[32];
    } want;
    bool rejoin_off;                    /* rtw89_glue_set_rejoin(false) */
    bool rejoining;                     /* trying to get back */
    bool rejoin_scanned;                /* this attempt has had its scan */
    unsigned int rejoin_tries;          /* attempts since the connection was lost */
    u32 rejoins;                        /* times it came back */
    unsigned long scan_started;
    bool scan_connected;                /* the scan started while associated */
    u32 scans_connected;
    u32 scan_ms_connected;
    u32 last_scan_ms;
    /* On a thread of its own: the driver's works run on the shared queue,
     * and the scan and join started from here wait for some of them. */
    struct workqueue_struct *rejoin_wq;
    struct delayed_work rejoin_work;

    /* the rate of the last frame the AP sent to us alone, see glue_rx_rate() */
    u32 rx_rate;

    /* frames the driver holds for a PPDU status report, see rtw89_core_wrap.c */
    bool ppdu_flush;                    /* pass them on after a short wait */
    struct delayed_work ppdu_flush_work;
    u32 ppdu_flushed;

    /* How long frames take from the chip's receive timestamp to here. The
     * chip's clock and ours are compared through the smallest difference seen
     * lately (two windows, so drift between the clocks cannot build up). */
    u32 rx_base, rx_base_prev;
    bool rx_base_valid, rx_base_prev_valid;
    u64 rx_base_since;
    u32 rx_late;                        /* frames more than 5 ms late */
    u32 rx_late_irq;                    /* ... of which an interrupt came when they arrived */
    u32 rx_late_max;                    /* ms */
    u64 irq_time[16];                   /* our clock, us, of the last interrupts */
    unsigned int irq_next;
} glue;

#define RTW89_GLUE_PPDU_WAIT    1       /* ms a frame may wait for its status report */
#define RTW89_GLUE_LATE_US      5000

static struct ieee80211_hw *glue_hw(void);
static void glue_deliver(const u8 *frame, size_t len);
static void glue_tx_wake(void);
struct rtw89_glue_bss_entry;
static void glue_bss_describe(struct rtw89_glue_bss_entry *e);
static void glue_rejoin_work(struct work_struct *work);
static void glue_rejoin_queue(unsigned long delay);

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

/* The PCI front ends built in (module_pci_driver() in rtw89_compat.h). */
#define GLUE_PCI_DRIVERS(x) \
    x(rtw89_8851be_driver) x(rtw89_8852ae_driver) x(rtw89_8852be_driver) \
    x(rtw89_8852bte_driver) x(rtw89_8852ce_driver) x(rtw89_8922ae_driver) \
    x(rtw89_8922de_driver)
#define GLUE_DECLARE(d) struct pci_driver *rtw89_compat_pci_##d(void);
#define GLUE_ENTRY(d) rtw89_compat_pci_##d,
GLUE_PCI_DRIVERS(GLUE_DECLARE)
static struct pci_driver *(*const glue_pci_drivers[])(void) = {
    GLUE_PCI_DRIVERS(GLUE_ENTRY)
};

/* The front end for this device and its id_table entry, or NULL. */
static const struct pci_device_id *glue_match(uint16_t vendor, uint16_t device,
                                              struct pci_driver **drv)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(glue_pci_drivers); i++) {
        struct pci_driver *d = glue_pci_drivers[i]();
        const struct pci_device_id *id = d->id_table;

        for (; id->vendor; id++) {
            if (id->vendor == vendor && id->device == device) {
                if (drv)
                    *drv = d;
                return id;
            }
        }
    }
    return NULL;
}

bool rtw89_glue_supports(uint16_t vendor, uint16_t device)
{
    return glue_match(vendor, device, NULL) != NULL;
}

static void glue_teardown(void)
{
    unsigned int leaked;

    /* while the work queue machinery is still there */
    if (glue.rejoin_wq)
        destroy_workqueue(glue.rejoin_wq);
    rtw88_compat_exit();

    kfree(glue.scan_req);
    spin_lock_destroy(&glue.bss_lock);
    mutex_destroy(&glue.cmd_lock);

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
    struct pci_driver *drv;
    int ret, i;

    if (glue.active)
        return -EBUSY;
    if (!platform->cfg_read || !platform->cfg_write || !platform->dma_alloc ||
        !platform->dma_free || !device->mmio_base || !device->mmio_len)
        return -EINVAL;

    id = glue_match(device->vendor, device->device, &drv);
    if (!id)
        return -ENODEV;

    ret = rtw88_compat_init();
    if (ret)
        return ret;
    rtw89_compat_debug_init();

    memset(&glue, 0, sizeof(glue));
    glue.active = true;
    glue.plat = *platform;
    glue.drv = drv;
    spin_lock_init(&glue.dma_lock);
    spin_lock_init(&glue.bss_lock);
    mutex_init(&glue.cmd_lock);
    INIT_DELAYED_WORK(&glue.rejoin_work, glue_rejoin_work);
    glue.rejoin_wq = alloc_ordered_workqueue("rtw89_rejoin", 0);
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

    rtw89_data_init(glue_hw(), glue_deliver, glue_tx_wake);
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
        rtw89_data_exit();
        glue.drv->remove(&glue.pdev);
    }
    glue_teardown();
}

static u64 glue_now_us(void)
{
    return ktime_get_boottime_ns() / 1000;
}

void rtw89_glue_interrupt(void)
{
    if (!glue.probed)
        return;
    glue.irq_time[glue.irq_next++ % ARRAY_SIZE(glue.irq_time)] = glue_now_us();
    rtw88_trigger_interrupt();
}

/* The datapath thread, RTW89_GLUE_PPDU_WAIT after a poll left frames parked. */
static void glue_ppdu_flush_work(struct work_struct *work)
{
    if (glue.up && glue.ppdu_flush)
        glue.ppdu_flushed += rtw89_compat_flush_ppdu_rx(glue_hw()->priv);
}

/* The datapath thread, after every NAPI poll. */
static void glue_napi_post_poll(int done)
{
    struct rtw89_dev *rtwdev;

    if (!glue.up || !glue.ppdu_flush)
        return;
    rtwdev = glue_hw()->priv;
    if (!skb_queue_empty(&rtwdev->ppdu_sts.rx_queue[RTW89_PHY_0]))
        rtw88_queue_datapath_delayed_work(&glue.ppdu_flush_work, RTW89_GLUE_PPDU_WAIT);
}

void rtw89_glue_set_ppdu_flush(bool on)
{
    glue.ppdu_flush = on;
}

void rtw89_glue_set_ax(bool on)
{
    rtw89_mlme_set_he(on);
}

/*
 * Remember how a frame was sent to us: mode, rate index, streams, width and
 * guard interval in one word, so that readers on other threads never see half
 * of an update. glue_rate_info() turns it back.
 */
static void glue_rx_rate(const struct ieee80211_rx_status *status)
{
    glue.rx_rate = BIT(31) | (u32)status->encoding | (u32)status->rate_idx << 4 |
                   (u32)status->nss << 12 | (u32)status->bw << 16 | (u32)status->he_gi << 20 |
                   (status->enc_flags & RX_ENC_FLAG_SHORT_GI ? BIT(22) : 0) |
                   (u32)status->band << 24;
}

/* sta_set_rate_info_rx() */
static bool glue_rate_info(u32 word, struct rate_info *ri)
{
    struct ieee80211_supported_band *sband;
    unsigned int idx = (word >> 4) & 0xff;

    memset(ri, 0, sizeof(*ri));
    if (!(word & BIT(31)))
        return false;
    ri->bw = (word >> 16) & 0xf;
    ri->nss = (word >> 12) & 0xf;
    switch (word & 0xf) {
    case RX_ENC_HE:
        ri->flags = RATE_INFO_FLAGS_HE_MCS;
        ri->mcs = (u8)idx;
        ri->he_gi = (word >> 20) & 3;
        break;
    case RX_ENC_VHT:
        ri->flags = RATE_INFO_FLAGS_VHT_MCS;
        ri->mcs = (u8)idx;
        break;
    case RX_ENC_HT:
        ri->flags = RATE_INFO_FLAGS_MCS;
        ri->mcs = (u8)idx;
        break;
    default:
        sband = glue_hw()->wiphy->bands[(word >> 24) & 0xf];
        if (!sband || idx >= (unsigned int)sband->n_bitrates)
            return false;
        ri->legacy = sband->bitrates[idx].bitrate;
        break;
    }
    if (word & BIT(22))
        ri->flags |= RATE_INFO_FLAGS_SHORT_GI;
    return true;
}

/* A rate for the outside: 802.11 mode, index, streams, width and the bit rate. */
static void glue_rate_describe(const struct rate_info *ri, struct rtw89_glue_rate *out)
{
    static const u8 mhz[] = {
        [RATE_INFO_BW_20] = 20, [RATE_INFO_BW_40] = 40, [RATE_INFO_BW_80] = 80,
        [RATE_INFO_BW_160] = 160,
    };

    memset(out, 0, sizeof(*out));
    out->kbps = cfg80211_calculate_bitrate((struct rate_info *)ri) * 100;
    if (!out->kbps)
        return;
    out->mode = ri->flags & RATE_INFO_FLAGS_HE_MCS ? 3 : ri->flags & RATE_INFO_FLAGS_VHT_MCS ? 2 :
                ri->flags & RATE_INFO_FLAGS_MCS ? 1 : 0;
    out->mcs = out->mode ? ri->mcs : 0;
    /* 802.11n numbers its rates through the streams: 8 to a stream */
    out->nss = out->mode == 1 ? (u8)(ri->mcs / 8 + 1) : out->mode ? ri->nss : 1;
    out->width = ri->bw < ARRAY_SIZE(mhz) && mhz[ri->bw] ? mhz[ri->bw] : 20;
}

/* Measure how late a frame is, by the chip's own receive timestamp. */
static void glue_rx_timing(const struct ieee80211_rx_status *status)
{
    u64 now = glue_now_us();
    u32 offset = (u32)now - (u32)status->mactime;
    u32 base;
    s32 late;

    /* a new window every five seconds */
    if (now - glue.rx_base_since > 5000000) {
        glue.rx_base_prev = glue.rx_base;
        glue.rx_base_prev_valid = glue.rx_base_valid;
        glue.rx_base_valid = false;
        glue.rx_base_since = now;
    }
    if (!glue.rx_base_valid || (s32)(offset - glue.rx_base) < 0) {
        glue.rx_base = offset;
        glue.rx_base_valid = true;
    }
    base = glue.rx_base;
    if (glue.rx_base_prev_valid && (s32)(glue.rx_base_prev - base) < 0)
        base = glue.rx_base_prev;

    late = (s32)(offset - base);
    if (late > RTW89_GLUE_LATE_US) {
        u64 arrived = now - (u32)late;
        unsigned int i;

        glue.rx_late++;
        if ((u32)late / 1000 > glue.rx_late_max)
            glue.rx_late_max = (u32)late / 1000;
        /* did the chip interrupt us when it arrived? then it was read from
         * the ring at once and waited inside the driver */
        for (i = 0; i < ARRAY_SIZE(glue.irq_time); i++) {
            if (glue.irq_time[i] + 1000 >= arrived && glue.irq_time[i] <= arrived + 3000) {
                glue.rx_late_irq++;
                break;
            }
        }
    }
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

size_t rtw89_glue_coex_info(char *buf, size_t max)
{
    struct ieee80211_hw *hw;
    ssize_t n;

    if (!glue.probed || !max)
        return 0;
    hw = pci_get_drvdata(&glue.pdev);
    wiphy_lock(hw->wiphy);
    n = rtw89_btc_dump_info(hw->priv, buf, max);
    wiphy_unlock(hw->wiphy);
    return n > 0 ? (size_t)n : 0;
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
    if (!e) {
        if (glue.n_bss < RTW89_GLUE_MAX_BSS) {
            e = &glue.bss[glue.n_bss++];
        } else {
            /* full: the one heard least recently makes room */
            e = &glue.bss[0];
            for (i = 1; i < glue.n_bss; i++)
                if (time_before(glue.bss[i].last_seen, e->last_seen))
                    e = &glue.bss[i];
        }
        memset(e, 0, sizeof(*e));
        memcpy(e->pub.bssid, mgmt->bssid, ETH_ALEN);
        e->pub.signal = signal;
        e->scan_gen = glue.scan_gen;
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
            glue_bss_describe(e);
        }
        e->pub.freq = freq;
        e->pub.channel = (u8)ieee80211_frequency_to_channel(freq);
        /* the newest signal at the first frame of each scan, the strongest
         * after that: the list outlives scans, a best-ever value would not
         * follow the machine moving away */
        if (e->scan_gen != glue.scan_gen || signal > e->pub.signal)
            e->pub.signal = signal;
        e->scan_gen = glue.scan_gen;
        e->last_seen = jiffies;
        e->pub.seen++;
    }
    spin_unlock(&glue.bss_lock);
}

/* Mode, width and security of a network, from its elements. */
static void glue_bss_describe(struct rtw89_glue_bss_entry *e)
{
    const struct element *elem;
    const u8 *p, *end;
    u16 count;

    e->pub.mode = 0;
    e->pub.width = 20;
    e->pub.security = e->pub.capability & WLAN_CAPABILITY_PRIVACY ? RTW89_GLUE_SEC_WEP_WPA1 : 0;

    elem = cfg80211_find_elem(WLAN_EID_HT_OPERATION, e->ies, e->ies_len);
    if (elem && elem->datalen >= sizeof(struct ieee80211_ht_operation) &&
        cfg80211_find_elem(WLAN_EID_HT_CAPABILITY, e->ies, e->ies_len)) {
        e->pub.mode = 1;
        if (elem->data[1] & IEEE80211_HT_PARAM_CHA_SEC_OFFSET)
            e->pub.width = 40;
    }
    elem = cfg80211_find_elem(WLAN_EID_VHT_OPERATION, e->ies, e->ies_len);
    if (e->pub.mode && elem && elem->datalen >= sizeof(struct ieee80211_vht_operation) &&
        cfg80211_find_elem(WLAN_EID_VHT_CAPABILITY, e->ies, e->ies_len)) {
        const struct ieee80211_vht_operation *oper = (const void *)elem->data;

        e->pub.mode = 2;
        if (oper->chan_width == IEEE80211_VHT_CHANWIDTH_80MHZ)
            /* a second centre eight channels away means 160 MHz */
            e->pub.width = oper->center_freq_seg1_idx &&
                           abs(oper->center_freq_seg1_idx - oper->center_freq_seg0_idx) == 8 ?
                           160 : 80;
        else if (oper->chan_width == IEEE80211_VHT_CHANWIDTH_160MHZ)
            e->pub.width = 160;
    }
    if (cfg80211_find_ext_elem(WLAN_EID_EXT_HE_CAPABILITY, e->ies, e->ies_len))
        e->pub.mode = 3;

    /* RSN: version, group cipher, pairwise list, AKM list, capabilities */
    elem = cfg80211_find_elem(WLAN_EID_RSN, e->ies, e->ies_len);
    if (!elem)
        return;
    e->pub.security = 0;
    p = elem->data;
    end = p + elem->datalen;
    if (end - p < 8)
        return;
    p += 6;
    count = get_unaligned_le16(p);
    p += 2;
    if (end - p < count * 4 + 2)
        return;
    p += count * 4;
    count = get_unaligned_le16(p);
    p += 2;
    if (end - p < count * 4)
        return;
    for (; count; count--, p += 4) {
        if (p[0] != 0x00 || p[1] != 0x0f || p[2] != 0xac)
            continue;
        switch (p[3]) {
        case 2: case 4: case 6:             /* PSK, FT-PSK, PSK-SHA256 */
            e->pub.security |= RTW89_GLUE_SEC_WPA2_PSK;
            break;
        case 8: case 9: case 24: case 25:   /* SAE and its variants */
            e->pub.security |= RTW89_GLUE_SEC_WPA3_SAE;
            break;
        default:
            e->pub.security |= RTW89_GLUE_SEC_ENTERPRISE;
            break;
        }
    }
    if (end - p >= 2 && (get_unaligned_le16(p) & BIT(6)))
        e->pub.security |= RTW89_GLUE_SEC_PMF_REQUIRED;
}

/* Every received frame lands here, on the driver's receive thread. */
static void glue_rx(void *ctx, struct ieee80211_sta *sta, struct sk_buff *skb)
{
    struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
    const struct ieee80211_hdr *hdr = (const void *)skb->data;
    size_t len = skb->len;

    if ((status->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_NO_PSDU)) || len < 2 + FCS_LEN) {
        kfree_skb(skb);
        return;
    }
    /* the one control frame that matters: it moves a reorder window */
    if (ieee80211_is_back_req(hdr->frame_control)) {
        rtw89_data_rx_bar(skb);
        return;
    }
    if (ieee80211_is_ctl(hdr->frame_control) ||
        len < sizeof(struct ieee80211_hdr_3addr) + FCS_LEN) {
        kfree_skb(skb);
        return;
    }

    if (status->flag & RX_FLAG_MACTIME_START)
        glue_rx_timing(status);

    if (ieee80211_is_beacon(hdr->frame_control) ||
        ieee80211_is_probe_resp(hdr->frame_control)) {
        /* The driver leaves the FCS on (RX_INCLUDES_FCS). */
        rtw89_glue_note_bss(skb->data, len - FCS_LEN, status->freq, status->signal);
        /* our own AP's beacons are also the sign that it is still there */
        if (ieee80211_is_beacon(hdr->frame_control))
            rtw89_mlme_rx_beacon(skb);
        else
            kfree_skb(skb);
        return;
    }

    if (ieee80211_is_data(hdr->frame_control)) {
        if (!is_multicast_ether_addr(hdr->addr1))
            glue_rx_rate(status);
        rtw89_data_rx(skb);
        return;
    }

    /* Everything else is for the station MLME, which takes the skb. */
    rtw89_mlme_rx(skb);
}

/* A received Ethernet frame from the data path. */
static void glue_deliver(const u8 *frame, size_t len)
{
    if (glue.plat.rx_frame)
        glue.plat.rx_frame(glue.plat.ctx, frame, len);
}

static void glue_tx_wake(void)
{
    if (glue.plat.tx_wake)
        glue.plat.tx_wake(glue.plat.ctx);
}

static void glue_tx_dequeued(void *ctx, struct ieee80211_txq *txq)
{
    rtw89_data_tx_dequeued(txq);
}

static void glue_link_event(void *ctx, struct ieee80211_vif *vif,
                            enum rtw89_m80211_link_event event, s32 rssi)
{
    if (event == RTW89_M80211_CONNECTION_LOSS)
        rtw89_mlme_connection_lost();
    else if (event == RTW89_M80211_BEACON_LOSS)
        rtw89_mlme_beacon_loss();
}

/*
 * Feed one received 802.11 frame (without FCS) through the same path the
 * driver uses. For the userspace smoke test, which plays the access point.
 */
static struct sk_buff *glue_test_skb(const u8 *frame, size_t len, u16 freq, s8 signal,
                                     bool decrypted)
{
    struct ieee80211_rx_status *status;
    struct sk_buff *skb;

    if (!glue.probed)
        return NULL;
    skb = dev_alloc_skb(len + FCS_LEN);
    if (!skb)
        return NULL;
    skb_put_data(skb, frame, len);
    skb_put_zero(skb, FCS_LEN);
    status = IEEE80211_SKB_RXCB(skb);
    memset(status, 0, sizeof(*status));
    status->freq = freq;
    status->signal = signal;
    if (decrypted)
        status->flag |= RX_FLAG_DECRYPTED;
    return skb;
}

void rtw89_glue_test_rx(const u8 *frame, size_t len, u16 freq, s8 signal, bool decrypted);
void rtw89_glue_test_rx(const u8 *frame, size_t len, u16 freq, s8 signal, bool decrypted)
{
    struct sk_buff *skb = glue_test_skb(frame, len, freq, signal, decrypted);

    if (skb)
        ieee80211_rx_napi(glue_hw(), NULL, skb, NULL);
}

/*
 * The same, but the frame is left where rtw89_core_rx() leaves a data frame
 * whose PPDU status report has not arrived, followed by the end of a poll.
 */
void rtw89_glue_test_rx_parked(const u8 *frame, size_t len, u16 freq, s8 signal,
                               bool decrypted);
void rtw89_glue_test_rx_parked(const u8 *frame, size_t len, u16 freq, s8 signal,
                               bool decrypted)
{
    struct sk_buff *skb = glue_test_skb(frame, len, freq, signal, decrypted);
    struct rtw89_dev *rtwdev;

    if (!skb)
        return;
    rtwdev = glue_hw()->priv;
    skb_queue_tail(&rtwdev->ppdu_sts.rx_queue[RTW89_PHY_0], skb);
    glue_napi_post_poll(0);
}

static void glue_tx_status(void *ctx, struct sk_buff *skb)
{
    rtw89_mlme_tx_status(skb);
    kfree_skb(skb);
}

/* Networks not heard for GLUE_BSS_MAX_AGE leave the list (after a scan). */
#define GLUE_BSS_MAX_AGE (90 * HZ)

static void glue_bss_expire(void)
{
    unsigned int i = 0;

    spin_lock(&glue.bss_lock);
    while (i < glue.n_bss) {
        if (time_after(jiffies, glue.bss[i].last_seen + GLUE_BSS_MAX_AGE)) {
            glue.n_bss--;
            if (i != glue.n_bss)
                glue.bss[i] = glue.bss[glue.n_bss];
        } else {
            i++;
        }
    }
    spin_unlock(&glue.bss_lock);
}

static void glue_scan_done(void *ctx, bool aborted)
{
    glue.scanning = false;
    glue.last_scan_ms = jiffies_to_msecs(jiffies - glue.scan_started);
    if (glue.scan_connected)
        glue.scan_ms_connected += glue.last_scan_ms;
    if (!aborted)
        glue_bss_expire();
    IOLog("[rtw89] scan %s: %u network(s) heard\n", aborted ? "aborted" : "finished",
          glue.n_bss);
    /* the attempt to get the network back was waiting for this */
    if (glue.rejoining)
        glue_rejoin_queue(0);
    if (glue.plat.scan_done)
        glue.plat.scan_done(glue.plat.ctx, aborted);
}

/* For the userspace smoke test, whose pretend chip never finishes a scan. */
void rtw89_glue_test_scan_done(void);
void rtw89_glue_test_scan_done(void)
{
    glue_scan_done(NULL, false);
}

/* ... and never misses a beacon. */
void rtw89_glue_test_beacon_loss(void);
void rtw89_glue_test_beacon_loss(void)
{
    if (glue.up)
        ieee80211_beacon_loss(glue.vif);
}

/* How long to wait before each attempt to get the network back, in ms. */
static const unsigned int glue_rejoin_delay[] = { 500, 1000, 2000, 4000, 8000, 15000, 30000, 60000 };

static void glue_rejoin_queue(unsigned long delay)
{
    if (glue.rejoin_wq)
        queue_delayed_work(glue.rejoin_wq, &glue.rejoin_work, delay);
}

static void glue_rejoin_schedule(void)
{
    unsigned int i = min_t(unsigned int, glue.rejoin_tries, ARRAY_SIZE(glue_rejoin_delay) - 1);

    glue_rejoin_queue(msecs_to_jiffies(glue_rejoin_delay[i]));
}

/* The MLME changed state. Called with the wiphy mutex held, from whichever
 * thread caused the change. */
static void glue_link_notify(void)
{
    struct rtw89_mlme_status st;

    rtw89_mlme_get_status(&st);
    if (st.state == RTW89_MLME_CONNECTED) {
        if (glue.want.valid) {
            if (glue.rejoining) {
                glue.rejoins++;
                IOLog("[rtw89] connected again after %u attempt(s)\n", glue.rejoin_tries);
            }
            glue.want.proven = true;
        }
        glue.rejoining = false;
        glue.rejoin_tries = 0;
    } else if (st.state == RTW89_MLME_IDLE && glue.up && glue.want.valid && glue.want.proven &&
               !glue.rejoin_off) {
        if (!glue.rejoining)
            IOLog("[rtw89] connection lost (%d): will join \"%.*s\" again\n", st.last_error,
                  glue.want.ssid_len, glue.want.ssid);
        glue.rejoining = true;
        glue.rejoin_scanned = false;
        glue_rejoin_schedule();
    }

    if (glue.plat.link_changed)
        glue.plat.link_changed(glue.plat.ctx);
}

static int glue_start_tx_ba(void *ctx, struct ieee80211_sta *sta, u16 tid, u16 timeout)
{
    return rtw89_mlme_tx_ba_request(sta, tid, true);
}

static int glue_stop_tx_ba(void *ctx, struct ieee80211_sta *sta, u16 tid)
{
    return rtw89_mlme_tx_ba_request(sta, tid, false);
}

static const struct rtw89_m80211_glue_ops glue_m80211_ops = {
    .rx = glue_rx,
    .tx_status = glue_tx_status,
    .tx_dequeued = glue_tx_dequeued,
    .scan_done = glue_scan_done,
    .link_event = glue_link_event,
    .start_tx_ba = glue_start_tx_ba,
    .stop_tx_ba = glue_stop_tx_ba,
};

/* ------------------------------------------------------------------ */
/*  Radio up / down                                                     */
/* ------------------------------------------------------------------ */

bool rtw89_glue_is_up(void)
{
    return glue.up;
}

static int glue_up_locked(void);

int rtw89_glue_up(void)
{
    int ret;

    if (!glue.probed)
        return -ENODEV;
    mutex_lock(&glue.cmd_lock);
    ret = glue_up_locked();
    mutex_unlock(&glue.cmd_lock);
    return ret;
}

static int glue_up_locked(void)
{
    struct rtw89_m80211_local *local;
    struct ieee80211_hw *hw;
    unsigned int filter = 0;
    int ret;

    if (glue.up)
        return 0;

    hw = glue_hw();
    local = hw_to_local(hw);
    rtw89_m80211_set_glue(hw, &glue_m80211_ops, NULL);

    glue.vif = rtw89_m80211_vif_alloc(hw, NL80211_IFTYPE_STATION,
                                      glue.have_mac ? glue.mac : hw->wiphy->perm_addr);
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
    rtw89_mlme_start(hw, glue.vif, glue_link_notify);
    wiphy_unlock(hw->wiphy);

    INIT_DELAYED_WORK(&glue.ppdu_flush_work, glue_ppdu_flush_work);
    glue.ppdu_flush = true;
    glue.up = true;
    rtw88_napi_post_poll = glue_napi_post_poll;
    return 0;

err_unlock:
    wiphy_unlock(hw->wiphy);
    if (glue.plat.irq_enable)
        glue.plat.irq_enable(glue.plat.ctx, false);
    rtw89_m80211_vif_free(glue.vif);
    glue.vif = NULL;
    return ret;
}

static void glue_forget_network(void)
{
    glue.want.valid = false;
    glue.rejoining = false;
    memset(glue.want.pmk, 0, sizeof(glue.want.pmk));
}

static void glue_down_locked(void);

void rtw89_glue_down(void)
{
    if (!glue.active)
        return;
    /* before the lock: the work takes it too */
    glue_forget_network();
    cancel_delayed_work_sync(&glue.rejoin_work);

    mutex_lock(&glue.cmd_lock);
    glue_down_locked();
    mutex_unlock(&glue.cmd_lock);
}

static void glue_down_locked(void)
{
    struct rtw89_m80211_local *local;
    struct ieee80211_hw *hw;

    if (!glue.up)
        return;
    glue.up = false;
    rtw88_napi_post_poll = NULL;
    cancel_delayed_work_sync(&glue.ppdu_flush_work);

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
    /* the driver is stopped: nothing can still be using what was freed */
    rtw89_m80211_reap(hw, true);
}

int rtw89_glue_set_mac(const uint8_t mac[6])
{
    int ret = 0;

    if (!glue.probed)
        return -ENODEV;
    if (is_multicast_ether_addr(mac) || is_zero_ether_addr(mac))
        return -EINVAL;

    glue_forget_network();
    cancel_delayed_work_sync(&glue.rejoin_work);
    mutex_lock(&glue.cmd_lock);
    if (!glue.have_mac || !ether_addr_equal(glue.mac, mac)) {
        bool was_up = glue.up;

        /* the interface gets its address when it is added */
        glue_down_locked();
        memcpy(glue.mac, mac, ETH_ALEN);
        glue.have_mac = true;
        if (was_up)
            ret = glue_up_locked();
    }
    mutex_unlock(&glue.cmd_lock);
    return ret;
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

static int glue_scan_locked(void);

int rtw89_glue_scan(void)
{
    int ret;

    if (!glue.active)
        return -ENETDOWN;
    mutex_lock(&glue.cmd_lock);
    ret = glue_scan_locked();
    mutex_unlock(&glue.cmd_lock);
    return ret;
}

static int glue_scan_locked(void)
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

    wiphy_lock(hw->wiphy);
    /* The previous request is no longer referenced once a new scan starts. */
    kfree(glue.scan_req);
    glue.scan_req = sreq;
    glue.scanning = true;
    glue.scan_started = jiffies;
    glue.scan_connected = glue.vif->cfg.assoc;
    ret = local->ops->hw_scan(hw, glue.vif, sreq);
    if (ret) {
        glue.scanning = false;
    } else {
        if (glue.scan_connected)
            glue.scans_connected++;
        /*
         * The list is kept through the scan, not started afresh: macOS asks
         * to join right after starting one, and an emptied list made a
         * network on a channel not yet scanned "not found". What was not
         * heard for a while goes when a scan ends (glue_bss_expire()).
         */
        spin_lock(&glue.bss_lock);
        glue.scan_gen++;
        spin_unlock(&glue.bss_lock);
    }
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

static void glue_copy_entry(const struct rtw89_glue_bss_entry *e, struct rtw89_glue_bss *out,
                            uint8_t *ies, size_t ies_max, size_t *ies_len, uint16_t *beacon_int)
{
    size_t n = min_t(size_t, e->ies_len, ies_max);

    if (out)
        *out = e->pub;
    if (ies && n)
        memcpy(ies, e->ies, n);
    if (ies_len)
        *ies_len = ies ? n : 0;
    if (beacon_int)
        *beacon_int = e->beacon_int;
}

bool rtw89_glue_scan_entry(unsigned int index, struct rtw89_glue_bss *out, uint8_t *ies,
                           size_t ies_max, size_t *ies_len, uint16_t *beacon_int)
{
    bool found = false;

    if (!glue.active)
        return false;
    spin_lock(&glue.bss_lock);
    if (index < glue.n_bss) {
        glue_copy_entry(&glue.bss[index], out, ies, ies_max, ies_len, beacon_int);
        found = true;
    }
    spin_unlock(&glue.bss_lock);
    return found;
}

bool rtw89_glue_find_bss(const uint8_t bssid[6], struct rtw89_glue_bss *out, uint8_t *ies,
                         size_t ies_max, size_t *ies_len, uint16_t *beacon_int)
{
    bool found = false;
    unsigned int i;

    if (!glue.active)
        return false;
    spin_lock(&glue.bss_lock);
    for (i = 0; i < glue.n_bss && !found; i++) {
        if (ether_addr_equal(glue.bss[i].pub.bssid, bssid)) {
            glue_copy_entry(&glue.bss[i], out, ies, ies_max, ies_len, beacon_int);
            found = true;
        }
    }
    spin_unlock(&glue.bss_lock);
    return found;
}

unsigned int rtw89_glue_channels(struct rtw89_glue_channel *out, unsigned int max)
{
    struct ieee80211_supported_band *sband;
    unsigned int n = 0;
    int band, i;

    if (!glue.probed)
        return 0;
    for (band = 0; band < NUM_NL80211_BANDS; band++) {
        sband = glue_hw()->wiphy->bands[band];
        for (i = 0; sband && i < sband->n_channels && n < max; i++) {
            const struct ieee80211_channel *chan = &sband->channels[i];

            if (chan->flags & IEEE80211_CHAN_DISABLED)
                continue;
            out[n].freq = (u16)chan->center_freq;
            out[n].channel = (u8)chan->hw_value;
            out[n].passive = !!(chan->flags & IEEE80211_CHAN_NO_IR);
            out[n].radar = !!(chan->flags & IEEE80211_CHAN_RADAR);
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Join / leave                                                        */
/* ------------------------------------------------------------------ */

/* WPA2-PSK: the pairwise master key from a passphrase (IEEE 802.11 J.4.1), or
 * given directly as 64 hex digits. */
static int glue_derive_pmk(const u8 *ssid, size_t ssid_len,
                           const char *passphrase, size_t len, u8 pmk[32])
{
    size_t i;

    if (len == 64) {
        for (i = 0; i < 64; i++) {
            char c = passphrase[i];
            u8 v;

            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                v = c - 'A' + 10;
            else
                return -EACCES;
            pmk[i / 2] = (i & 1) ? (pmk[i / 2] | v) : (u8)(v << 4);
        }
        return 0;
    }
    if (len < 8 || len > 63)
        return -EACCES;

    rtw89_pbkdf2_sha1((const u8 *)passphrase, len, ssid, ssid_len, 4096, pmk, 32);
    return 0;
}

int rtw89_glue_derive_pmk(const uint8_t *ssid, size_t ssid_len, const char *passphrase,
                          size_t passphrase_len, uint8_t pmk[32])
{
    if (!ssid_len || ssid_len > IEEE80211_MAX_SSID_LEN)
        return -EINVAL;
    return glue_derive_pmk(ssid, ssid_len, passphrase, passphrase_len, pmk);
}

/* Join the strongest access point of the last scan that has this name. */
/* When set, the next glue_join_locked() is for an outside supplicant. */
static const u8 *glue_ext_rsn_ie;
static size_t glue_ext_rsn_len;
static bool glue_ext;

static int glue_join_locked(const u8 *ssid, size_t ssid_len, const u8 *bssid, const u8 *pmk)
{
    static u8 ies[RTW89_GLUE_MAX_IES];  /* under cmd_lock */
    struct rtw89_glue_bss_entry *best = NULL;
    struct rtw89_mlme_bss bss = {};
    struct ieee80211_hw *hw;
    unsigned int i;
    int ret;

    spin_lock(&glue.bss_lock);
    for (i = 0; i < glue.n_bss; i++) {
        struct rtw89_glue_bss_entry *e = &glue.bss[i];

        if (e->pub.ssid_len != ssid_len || memcmp(e->pub.ssid, ssid, ssid_len) ||
            !e->ies_len)
            continue;
        if (bssid && !ether_addr_equal(e->pub.bssid, bssid))
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
    if (glue_ext)
        ret = rtw89_mlme_connect_ext(&bss, glue_ext_rsn_ie, glue_ext_rsn_len);
    else
        ret = rtw89_mlme_connect(&bss, pmk);
    wiphy_unlock(hw->wiphy);
    return ret;
}

static int glue_join_request(const u8 *ssid, size_t ssid_len, const u8 *bssid, const u8 *pmk)
{
    typeof(glue.want) old;
    int ret;

    mutex_lock(&glue.cmd_lock);
    if (!glue.up) {
        ret = -ENETDOWN;
        goto out;
    }
    /* a new request ends the attempts to get the previous network back */
    if (glue.rejoining) {
        struct ieee80211_hw *hw = glue_hw();

        glue_forget_network();
        cancel_delayed_work(&glue.rejoin_work);
        wiphy_lock(hw->wiphy);
        rtw89_mlme_disconnect(WLAN_REASON_DEAUTH_LEAVING);
        wiphy_unlock(hw->wiphy);
    }

    /* What to come back to, in place before the join can succeed. If the
     * join cannot even start, what was there before stays. */
    old = glue.want;
    memset(&glue.want, 0, sizeof(glue.want));
    memcpy(glue.want.ssid, ssid, ssid_len);
    glue.want.ssid_len = (u8)ssid_len;
    glue.want.have_pmk = pmk != NULL;
    if (pmk)
        memcpy(glue.want.pmk, pmk, sizeof(glue.want.pmk));
    glue.want.valid = true;

    ret = glue_join_locked(ssid, ssid_len, bssid, pmk);
    if (ret)
        glue.want = old;
    else
        glue.rejoin_tries = 0;
    memset(&old, 0, sizeof(old));
out:
    mutex_unlock(&glue.cmd_lock);
    return ret;
}

int rtw89_glue_join(const uint8_t *ssid, size_t ssid_len,
                    const char *passphrase, size_t passphrase_len)
{
    bool have_pmk = false;
    u8 pmk[32];
    int ret;

    if (!glue.up)
        return -ENETDOWN;
    if (!ssid_len || ssid_len > IEEE80211_MAX_SSID_LEN)
        return -EINVAL;
    if (passphrase && passphrase_len) {
        ret = glue_derive_pmk(ssid, ssid_len, passphrase, passphrase_len, pmk);
        if (ret)
            return ret;
        have_pmk = true;
    }
    ret = glue_join_request(ssid, ssid_len, NULL, have_pmk ? pmk : NULL);
    memset(pmk, 0, sizeof(pmk));
    return ret;
}

int rtw89_glue_join_ext(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid,
                        const uint8_t *rsn_ie, size_t rsn_len)
{
    int ret;

    if (!glue.up)
        return -ENETDOWN;
    if (!ssid_len || ssid_len > IEEE80211_MAX_SSID_LEN)
        return -EINVAL;
    /* not remembered for coming back: the sign-in is not ours to repeat */
    mutex_lock(&glue.cmd_lock);
    glue_forget_network();
    cancel_delayed_work(&glue.rejoin_work);
    glue_ext = true;
    glue_ext_rsn_ie = rsn_ie;
    glue_ext_rsn_len = rsn_len;
    ret = glue.up ? glue_join_locked(ssid, ssid_len, bssid, NULL) : -ENETDOWN;
    glue_ext = false;
    glue_ext_rsn_ie = NULL;
    mutex_unlock(&glue.cmd_lock);
    return ret;
}

size_t rtw89_glue_assoc_rsn_ie(uint8_t *buf, size_t max)
{
    return glue.up ? rtw89_mlme_assoc_rsn_ie(buf, max) : 0;
}

int rtw89_glue_set_key(bool pairwise, int index, const uint8_t *key, size_t len, uint64_t rsc)
{
    struct ieee80211_hw *hw;
    int ret;

    if (!glue.up)
        return -ENETDOWN;
    hw = glue_hw();
    wiphy_lock(hw->wiphy);
    ret = rtw89_mlme_set_key(pairwise, index, key, len, rsc);
    wiphy_unlock(hw->wiphy);
    return ret;
}

int rtw89_glue_set_pmk(const uint8_t *pmk, size_t len)
{
    struct ieee80211_hw *hw;
    int ret;

    if (!glue.up)
        return -ENETDOWN;
    hw = glue_hw();
    wiphy_lock(hw->wiphy);
    ret = rtw89_mlme_set_pmk(pmk, len);
    wiphy_unlock(hw->wiphy);
    return ret;
}

int rtw89_glue_join_pmk(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid,
                        const uint8_t *pmk)
{
    if (!glue.up)
        return -ENETDOWN;
    if (!ssid_len || ssid_len > IEEE80211_MAX_SSID_LEN)
        return -EINVAL;
    return glue_join_request(ssid, ssid_len, bssid, pmk);
}

void rtw89_glue_leave(void)
{
    struct ieee80211_hw *hw;

    if (!glue.up)
        return;
    mutex_lock(&glue.cmd_lock);
    if (glue.up) {
        /* leaving is for good */
        glue_forget_network();
        cancel_delayed_work(&glue.rejoin_work);
        hw = glue_hw();
        wiphy_lock(hw->wiphy);
        rtw89_mlme_disconnect(WLAN_REASON_DEAUTH_LEAVING);
        wiphy_unlock(hw->wiphy);
    }
    mutex_unlock(&glue.cmd_lock);
}

/*
 * One step of getting the network back after the connection was lost: a
 * scan (the AP may have moved), then a join; glue_link_notify() and
 * glue_scan_done() queue the next step.
 */
static void glue_rejoin_work(struct work_struct *work)
{
    struct rtw89_mlme_status st;
    int ret;

    mutex_lock(&glue.cmd_lock);
    if (!glue.up || !glue.want.valid || !glue.rejoining || glue.rejoin_off)
        goto out;
    rtw89_mlme_get_status(&st);
    if (st.state != RTW89_MLME_IDLE)
        goto out;       /* an attempt is under way; its end brings us back */

    if (glue.scanning) {
        /* its results will do; look again when it is over, or after a while
         * if it never is */
        glue.rejoin_scanned = true;
        if (time_before(jiffies, glue.scan_started + 15 * HZ)) {
            glue_rejoin_queue(HZ);
            goto out;
        }
    } else if (!glue.rejoin_scanned) {
        glue.rejoin_scanned = true;
        if (!glue_scan_locked())
            goto out;
        /* no scan to be had: try with what was heard before */
    }

    glue.rejoin_scanned = false;
    glue.rejoin_tries++;
    ret = glue_join_locked(glue.want.ssid, glue.want.ssid_len, NULL,
                           glue.want.have_pmk ? glue.want.pmk : NULL);
    if (ret) {
        IOLog("[rtw89] attempt %u to join \"%.*s\" again failed: %d\n", glue.rejoin_tries,
              glue.want.ssid_len, glue.want.ssid, ret);
        glue_rejoin_schedule();
    }
out:
    mutex_unlock(&glue.cmd_lock);
}

void rtw89_glue_probe_ap(void)
{
    if (glue.up)
        rtw89_mlme_beacon_loss();
}

void rtw89_glue_drop(void)
{
    if (glue.up)
        rtw89_mlme_connection_lost();
}

void rtw89_glue_set_rejoin(bool on)
{
    if (!glue.active)
        return;
    glue.rejoin_off = !on;
    if (!on) {
        glue.rejoining = false;
        cancel_delayed_work(&glue.rejoin_work);
    }
}

void rtw89_glue_link(struct rtw89_glue_link *link)
{
    struct rtw89_mlme_status st;

    struct rtw89_data_stats stats;

    memset(link, 0, sizeof(*link));
    if (!glue.probed)
        return;
    rtw89_data_get_stats(&stats);
    link->tx_frames = stats.tx_frames;
    link->tx_dropped = stats.tx_dropped;
    link->rx_frames = stats.rx_frames;
    link->rx_dropped = stats.rx_dropped;
    link->rx_undecrypted = stats.rx_undecrypted;
    link->rx_replay = stats.rx_replay;
    link->rx_dup = stats.rx_dup;
    link->rx_reorder_timeout = stats.rx_reorder_timeout;
    link->rx_late = glue.rx_late;
    link->rx_late_irq = glue.rx_late_irq;
    link->rx_late_max_ms = glue.rx_late_max;
    link->rx_ppdu_flushed = glue.ppdu_flushed;
    link->ppdu_flush = glue.ppdu_flush;
    link->ax = rtw89_mlme_get_he();
    link->rejoin = !glue.rejoin_off;
    link->rejoins = glue.rejoins;
    {
        struct rtw89_dev *rtwdev = glue_hw()->priv;

        if (rtwdev->regulatory.regd)
            memcpy(link->country, rtwdev->regulatory.regd->alpha2, 2);
        if (glue.up && glue.vif && glue.vif->cfg.assoc) {
            const struct rtw89_chan *chan = rtw89_chan_get(rtwdev, RTW89_CHANCTX_0);

            link->txpwr_limit[0] = rtw89_phy_read_txpwr_limit(rtwdev, chan->band_type,
                RTW89_CHANNEL_WIDTH_20, RTW89_1TX, RTW89_RS_MCS, RTW89_NONBF,
                chan->primary_channel);
            link->txpwr_limit[1] = rtw89_phy_read_txpwr_limit(rtwdev, chan->band_type,
                RTW89_CHANNEL_WIDTH_20, RTW89_2TX, RTW89_RS_MCS, RTW89_NONBF,
                chan->primary_channel);
        }
    }
    if (!glue.up)
        return;

    rtw89_mlme_get_status(&st);
    link->rejoining = glue.rejoining;
    link->rejoin_tries = glue.rejoin_tries;
    link->beacons = st.beacons;
    link->beacon_losses = st.beacon_losses;
    link->beacon_updates = st.beacon_updates;
    link->scans_connected = glue.scans_connected;
    link->scan_ms_connected = glue.scan_ms_connected;
    link->last_scan_ms = glue.last_scan_ms;
    link->probe_acks = st.probe_acks;
    if (st.state == RTW89_MLME_CONNECTED) {
        struct rate_info ri;

        if (st.tx_rate_valid)
            glue_rate_describe(&st.tx_rate, &link->tx_rate);
        if (glue_rate_info(glue.rx_rate, &ri))
            glue_rate_describe(&ri, &link->rx_rate);
    }
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
    link->authorized = st.state == RTW89_MLME_CONNECTED && rtw89_mlme_authorized();
    if (st.state != RTW89_MLME_IDLE) {
        struct rtw89_glue_bss bss;

        if (rtw89_glue_find_bss(st.bssid, &bss, NULL, 0, NULL, NULL))
            link->signal = bss.signal;
    }
    link->width = st.width;
    link->center_freq = st.center_freq;
    link->mode = st.mode;
    link->nss = st.nss;
    link->last_error = st.last_error;
    link->eapol_rx = st.eapol_rx;
    link->tx_ba = st.tx_ba;
    link->rx_ba = st.rx_ba;
}

/* ------------------------------------------------------------------ */
/*  Data                                                                */
/* ------------------------------------------------------------------ */

void *rtw89_glue_tx_alloc(size_t len, uint8_t **frame)
{
    struct sk_buff *skb;

    if (!glue.probed)
        return NULL;
    skb = rtw89_data_tx_alloc(len);
    if (skb)
        *frame = skb->data;
    return skb;
}

int rtw89_glue_tx(void *handle)
{
    return rtw89_data_tx(handle);
}

unsigned int rtw89_glue_tx_room(void)
{
    return rtw89_data_tx_room();
}

void rtw89_glue_tx_cancel(void *handle)
{
    kfree_skb(handle);
}
