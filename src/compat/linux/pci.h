/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef _RTW88_COMPAT_PCI_H
#define _RTW88_COMPAT_PCI_H

#include "types.h"
#include "slab.h"
#include "dma-mapping.h"
#include "device.h"
#include "interrupt.h"

/*
 * PCI shims for rtw88 macOS port.
 *
 * The actual PCI operations (config space reads/writes, MMIO mapping,
 * DMA, interrupts) are provided by RTW88PCIDevice via the global
 * rtw88_pci_ops pointer set at driver start.
 */

#define PCI_ANY_ID  (~0U)

struct pci_device_id {
    u32 vendor, device;
    u32 subvendor, subdevice;
    u32 class_val, class_mask;
    unsigned long driver_data;
};

#define PCI_DEVICE(vend, dev) \
    .vendor = (vend), .device = (dev), \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

#define PCI_DEVICE_DATA(vend, dev, data) \
    .vendor = PCI_VENDOR_ID_##vend, .device = (dev), \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, \
    .driver_data = (unsigned long)(data)

/* PCI Vendor/Device IDs for rtw88 chips */
#define PCI_VENDOR_ID_REALTEK  0x10EC

/* rtw8822BE */
#define PCI_DEVICE_ID_RTL8822B  0xB822
/* rtw8822CE */
#define PCI_DEVICE_ID_RTL8822C  0xC822
/* rtw8821CE */
#define PCI_DEVICE_ID_RTL8821C  0xC821

struct pci_dev {
    u16 vendor;
    u16 device;
    u16 subsystem_vendor;
    u16 subsystem_device;
    u8  revision;
    u8  irq;
    u32 class_val;

    /* Bus address of BAR regions */
    resource_size_t resource[6];
    resource_size_t resource_len[6];

    /* Private driver data */
    void *driver_data;

    /* Embedded generic device (required by driver code: pdev->dev) */
    struct device dev;

    /* Pointer back to our kext device object */
    void *kext_dev;
};

/* PCI config space */
#define PCI_COMMAND         0x04
#define PCI_COMMAND_MASTER  0x04
#define PCI_COMMAND_MEMORY  0x02
#define PCI_CAP_ID_EXP      0x10
#define PCI_EXP_LNKCTL                   0x10
#define PCI_EXP_LNKCTL_CLKREQ_EN        0x100
#define PCI_EXP_LNKCTL_ASPM_L0S         0x01
#define PCI_EXP_LNKCTL_ASPM_L1          0x02
#define PCI_EXP_DEVCTL2                  0x28
#define PCI_EXP_DEVCTL2_COMP_TMOUT_DIS   0x0010

struct pci_ops_rtw88 {
    int  (*read_config_byte)(struct pci_dev *, int where, u8 *val);
    int  (*read_config_word)(struct pci_dev *, int where, u16 *val);
    int  (*read_config_dword)(struct pci_dev *, int where, u32 *val);
    int  (*write_config_byte)(struct pci_dev *, int where, u8 val);
    int  (*write_config_word)(struct pci_dev *, int where, u16 val);
    int  (*write_config_dword)(struct pci_dev *, int where, u32 val);
    void *(*ioremap)(struct pci_dev *, int bar, size_t len);
    void (*iounmap)(struct pci_dev *, void *addr);
    int  (*enable_msi)(struct pci_dev *);
    void (*disable_msi)(struct pci_dev *);
    int  (*pci_find_capability)(struct pci_dev *, int cap);
};

extern struct pci_ops_rtw88 *rtw88_pci_io_ops;

static inline int pci_read_config_byte(struct pci_dev *dev, int where, u8 *val)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->read_config_byte(dev, where, val);
    *val = 0xff; return -1;
}
static inline int pci_read_config_word(struct pci_dev *dev, int where, u16 *val)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->read_config_word(dev, where, val);
    *val = 0xffff; return -1;
}
static inline int pci_read_config_dword(struct pci_dev *dev, int where, u32 *val)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->read_config_dword(dev, where, val);
    *val = 0xffffffff; return -1;
}
static inline int pci_write_config_byte(struct pci_dev *dev, int where, u8 val)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->write_config_byte(dev, where, val);
    return -1;
}
static inline int pci_write_config_word(struct pci_dev *dev, int where, u16 val)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->write_config_word(dev, where, val);
    return -1;
}
static inline int pci_write_config_dword(struct pci_dev *dev, int where, u32 val)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->write_config_dword(dev, where, val);
    return -1;
}

static inline int pci_find_capability(struct pci_dev *dev, int cap)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->pci_find_capability(dev, cap);
    return 0;
}

/* pcie_capability_*: @where is an offset inside the PCI Express capability
 * structure (capability ID 0x10), wherever that sits in config space. */
static inline int pcie_capability_set_word(struct pci_dev *pdev, int where, u16 set)
{
    int cap = pci_find_capability(pdev, 0x10);
    u16 val;
    int ret;
    if (!cap) return -EINVAL;
    ret = pci_read_config_word(pdev, cap + where, &val);
    if (ret) return ret;
    return pci_write_config_word(pdev, cap + where, val | set);
}

static inline int pcie_capability_clear_word(struct pci_dev *pdev, int where, u16 clear)
{
    int cap = pci_find_capability(pdev, 0x10);
    u16 val;
    int ret;
    if (!cap) return -EINVAL;
    ret = pci_read_config_word(pdev, cap + where, &val);
    if (ret) return ret;
    return pci_write_config_word(pdev, cap + where, val & ~clear);
}

static inline void *pci_ioremap_bar(struct pci_dev *dev, int bar)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->ioremap(dev, bar, dev->resource_len[bar]);
    return NULL;
}

static inline void pci_iounmap(struct pci_dev *dev, void __iomem *addr)
{
    if (rtw88_pci_io_ops) rtw88_pci_io_ops->iounmap(dev, addr);
}

/* MMIO accessors — mapped to the memory window */
static inline u8 readb(const volatile void __iomem *addr)
{
    return *(volatile u8 *)addr;
}
static inline u16 readw(const volatile void __iomem *addr)
{
    return *(volatile u16 *)addr;
}
static inline u32 readl(const volatile void __iomem *addr)
{
    return *(volatile u32 *)addr;
}
static inline void writeb(u8 val, volatile void __iomem *addr)
{
    *(volatile u8 *)addr = val;
}
static inline void writew(u16 val, volatile void __iomem *addr)
{
    *(volatile u16 *)addr = val;
}
static inline void writel(u32 val, volatile void __iomem *addr)
{
    *(volatile u32 *)addr = val;
}

static inline int pci_enable_device(struct pci_dev *dev) { return 0; }
static inline void pci_disable_device(struct pci_dev *dev) {}
static inline int pci_request_regions(struct pci_dev *dev, const char *res_name) { return 0; }
static inline void pci_release_regions(struct pci_dev *dev) {}
static inline void pci_set_master(struct pci_dev *dev) {}

static inline int pci_enable_msi(struct pci_dev *dev)
{
    if (rtw88_pci_io_ops) return rtw88_pci_io_ops->enable_msi(dev);
    return -EOPNOTSUPP;
}

static inline void pci_disable_msi(struct pci_dev *dev)
{
    if (rtw88_pci_io_ops) rtw88_pci_io_ops->disable_msi(dev);
}

static inline void pci_set_drvdata(struct pci_dev *dev, void *data)
{
    dev->driver_data = data;
}

static inline void *pci_get_drvdata(struct pci_dev *dev)
{
    return dev->driver_data;
}

static inline u64 pci_resource_start(struct pci_dev *dev, int bar)
{
    return (u64)dev->resource[bar];
}

static inline u64 pci_resource_len(struct pci_dev *dev, int bar)
{
    return (u64)dev->resource_len[bar];
}

/* PCI power states */
#define PCI_D0     0
#define PCI_D1     1
#define PCI_D2     2
#define PCI_D3hot  3
#define PCI_D3cold 4

/* IRQ allocation flags */
#define PCI_IRQ_INTX  (1 << 0)
#define PCI_IRQ_MSI   (1 << 1)
#define PCI_IRQ_ALL_TYPES (PCI_IRQ_INTX | PCI_IRQ_MSI)

/* PCI vendor IDs */
#define PCI_VENDOR_ID_INTEL  0x8086

/* IRQ alloc/free */
static inline int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min,
                                         unsigned int max, unsigned int flags) { return 1; }
static inline void pci_free_irq_vectors(struct pci_dev *dev) {}

/* Wake-on-LAN */
static inline int pci_enable_wake(struct pci_dev *dev, int state, bool en) { return 0; }

/* PM ops */
struct dev_pm_ops {
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);
};

#define SIMPLE_DEV_PM_OPS(name, suspend_fn, resume_fn) \
    const struct dev_pm_ops name = { .suspend = (suspend_fn), .resume = (resume_fn) }

/* PCI error recovery */
typedef int pci_ers_result_t;
typedef int pci_channel_state_t;
#define PCI_ERS_RESULT_NEED_RESET  1
#define PCI_ERS_RESULT_RECOVERED   2
#define PCI_ERS_RESULT_DISCONNECT  3

struct pci_error_handlers {
    pci_ers_result_t (*error_detected)(struct pci_dev *, pci_channel_state_t);
    pci_ers_result_t (*slot_reset)(struct pci_dev *);
    void             (*resume)(struct pci_dev *);
};

static inline void netif_device_attach(struct net_device *dev) {}
static inline void netif_device_detach(struct net_device *dev) {}

struct pci_driver {
    const char *name;
    const struct pci_device_id *id_table;
    int (*probe)(struct pci_dev *, const struct pci_device_id *);
    void (*remove)(struct pci_dev *);
    void (*shutdown)(struct pci_dev *);
    struct {
        const struct dev_pm_ops *pm;
    } driver;
    const struct pci_error_handlers *err_handler;
};

static inline int pci_register_driver(struct pci_driver *drv) { return 0; }
static inline void pci_unregister_driver(struct pci_driver *drv) {}

static inline int pci_save_state(struct pci_dev *dev) { return 0; }
static inline int pci_restore_state(struct pci_dev *dev) { return 0; }
static inline int pci_prepare_to_sleep(struct pci_dev *dev) { return 0; }
static inline int pci_back_from_sleep(struct pci_dev *dev) { return 0; }

/* pci_iomap — map a PCI BAR (alias pci_ioremap_bar) */
static inline void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
    (void)maxlen;
    return pci_ioremap_bar(dev, bar);
}

/* pci_set_power_state — no-op; power management not implemented */
static inline int pci_set_power_state(struct pci_dev *dev, int state)
{
    (void)dev; (void)state;
    return 0;
}

/* pci_upstream_bridge — return parent bus's bridge device (stub: NULL) */
static inline struct pci_dev *pci_upstream_bridge(struct pci_dev *dev)
{
    (void)dev;
    return NULL;
}

/* pcie_capability_read_word — read a PCIe capability register word */
static inline int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
    int cap = pci_find_capability(dev, 0x10);
    if (!cap) { *val = 0; return -EINVAL; }
    return pci_read_config_word(dev, cap + pos, val);
}

/* to_pci_dev — cast a struct device * to struct pci_dev * */
#define to_pci_dev(d) container_of(d, struct pci_dev, dev)

#endif /* _RTW88_COMPAT_PCI_H */
