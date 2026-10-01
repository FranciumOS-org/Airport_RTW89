/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_SDIO_H
#define _RTW88_COMPAT_SDIO_H

#include "types.h"
#include "device.h"
#include "slab.h"

typedef unsigned int mmc_pm_flag_t;
#define MMC_PM_KEEP_POWER    (1 << 0)
#define MMC_PM_WAKE_SDIO_IRQ (1 << 1)

/* MMC host — only max_req_size used by rtw88 */
struct mmc_host {
    unsigned int max_req_size;
    unsigned int caps;
};

/* MMC/UHS capability flags */
#define MMC_CAP_UHS_SDR12   (1 << 0)
#define MMC_CAP_UHS_SDR25   (1 << 1)
#define MMC_CAP_UHS_SDR50   (1 << 2)
#define MMC_CAP_UHS_SDR104  (1 << 3)
#define MMC_CAP_UHS_DDR50   (1 << 4)

struct mmc_card {
    struct mmc_host *host;
    unsigned int type;
};

#define MMC_TYPE_SDIO 3

static inline int mmc_card_uhs(struct mmc_card *card)
{
    return card && card->host &&
           (card->host->caps & (MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25 |
                                MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR104 |
                                MMC_CAP_UHS_DDR50));
}

struct sdio_func {
    struct device    dev;    /* &func->dev used by dev_err, SET_IEEE80211_DEV */
    struct mmc_card *card;   /* host->max_req_size via func->card->host */
    u8   num;
    u16  vendor;
    u16  device;
    unsigned int max_blksize;
    void *drv_priv;
};

struct sdio_device_id {
    u8  class;
    u16 vendor;
    u16 device;
    unsigned long driver_data;
};

#define SDIO_DEVICE(v, d)  .vendor = (v), .device = (d)
#define SDIO_DEVICE_CLASS(c) .class = (c)
#define SDIO_ANY_ID (~0)

/* dev_to_sdio_func: recover sdio_func from embedded struct device */
#define dev_to_sdio_func(d) container_of(d, struct sdio_func, dev)

static inline void *sdio_get_drvdata(struct sdio_func *f) { return f->drv_priv; }
static inline void  sdio_set_drvdata(struct sdio_func *f, void *d) { f->drv_priv = d; }
static inline int   sdio_enable_func(struct sdio_func *f) { return 0; }
static inline void  sdio_disable_func(struct sdio_func *f) {}
static inline int   sdio_set_block_size(struct sdio_func *f, unsigned int sz) { return 0; }
static inline void  sdio_claim_host(struct sdio_func *f) {}
static inline void  sdio_release_host(struct sdio_func *f) {}

static inline u8   sdio_readb(struct sdio_func *f, unsigned int a, int *e)
    { if (e) *e = -EIO; return 0; }
static inline u16  sdio_readw(struct sdio_func *f, unsigned int a, int *e)
    { if (e) *e = -EIO; return 0; }
static inline u32  sdio_readl(struct sdio_func *f, unsigned int a, int *e)
    { if (e) *e = -EIO; return 0; }
static inline void sdio_writeb(struct sdio_func *f, u8 v, unsigned int a, int *e)
    { if (e) *e = -EIO; }
static inline void sdio_writew(struct sdio_func *f, u16 v, unsigned int a, int *e)
    { if (e) *e = -EIO; }
static inline void sdio_writel(struct sdio_func *f, u32 v, unsigned int a, int *e)
    { if (e) *e = -EIO; }

static inline int sdio_memcpy_fromio(struct sdio_func *f, void *d, unsigned int a, int l)
    { return -EIO; }
static inline int sdio_memcpy_toio(struct sdio_func *f, unsigned int a, void *s, int l)
    { return -EIO; }
static inline int sdio_readsb(struct sdio_func *f, void *d, unsigned int a, int l)
    { return -EIO; }

static inline int  sdio_claim_irq(struct sdio_func *f, void (*h)(struct sdio_func *))
    { return 0; }
static inline void sdio_release_irq(struct sdio_func *f) {}

static inline int sdio_set_host_pm_flags(struct sdio_func *f, mmc_pm_flag_t flags)
    { return 0; }

struct sdio_driver {
    const char *name;
    const struct sdio_device_id *id_table;
    int  (*probe)(struct sdio_func *, const struct sdio_device_id *);
    void (*remove)(struct sdio_func *);
    int  (*suspend)(struct sdio_func *, int message);
    int  (*resume)(struct sdio_func *);
};

static inline int  sdio_register_driver(struct sdio_driver *d) { return 0; }
static inline void sdio_unregister_driver(struct sdio_driver *d) {}

#define module_sdio_driver(__sdio_driver) \
    static int __init __sdio_driver##_init(void) \
        { return sdio_register_driver(&(__sdio_driver)); } \
    static void __exit __sdio_driver##_exit(void) \
        { sdio_unregister_driver(&(__sdio_driver)); }

unsigned int sdio_align_size(struct sdio_func *, unsigned int);
#endif /* _RTW88_COMPAT_SDIO_H */
