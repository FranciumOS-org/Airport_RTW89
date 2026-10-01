/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Boundary between the platform (the IOKit kext, or the host smoke test) and
 * everything Linux-shaped. This header is plain C with no Linux types, so the
 * C++ side never sees the compat headers; rtw89_glue.c translates.
 *
 * The platform supplies config-space access, the mapped register BAR and DMA
 * memory. The glue builds the struct pci_dev, installs the compat PCI/DMA ops
 * and runs the driver's own probe()/remove().
 */
#ifndef _RTW89_GLUE_H
#define _RTW89_GLUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rtw89_glue_platform {
    void *ctx;

    /* PCI configuration space; @offset up to 4095, @width 1, 2 or 4 bytes. */
    uint32_t (*cfg_read)(void *ctx, unsigned int offset, unsigned int width);
    void (*cfg_write)(void *ctx, unsigned int offset, unsigned int width,
                      uint32_t value);

    /*
     * Wired, physically contiguous memory the device can reach (below 4 GB).
     * *@bus_addr is the address to program into the device; *@cookie is handed
     * back to dma_free(). May sleep.
     */
    void *(*dma_alloc)(void *ctx, size_t size, uint64_t *bus_addr, void **cookie);
    void (*dma_free)(void *ctx, void *cookie);
};

struct rtw89_glue_device {
    uint16_t vendor;
    uint16_t device;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    uint8_t  revision;

    /* The memory BAR the driver uses for its registers (BAR 2), mapped. */
    volatile void *mmio_base;
    size_t mmio_len;
};

struct rtw89_glue_info {
    uint8_t  mac[6];
    char     fw_version[32];    /* "0.29.29.18"; empty if not recognised */
    uint32_t fw_commit;
    uint8_t  chip_cut;          /* 0 = A, 1 = B, ... */
    uint8_t  rfe_type;
    uint8_t  tx_streams;
    uint8_t  rx_streams;
};

/* True if the driver's id_table has this vendor/device pair. */
bool rtw89_glue_supports(uint16_t vendor, uint16_t device);

/*
 * Bring the compat runtime up and run the driver's probe(): allocate the hw,
 * power the chip on, download firmware, read the efuse, power it off again and
 * register. Returns 0 or a negative Linux errno. One device at a time.
 */
int rtw89_glue_probe(const struct rtw89_glue_platform *platform,
                     const struct rtw89_glue_device *device);

/* Undo rtw89_glue_probe(). Safe to call if probe failed or never ran. */
void rtw89_glue_remove(void);

/* The device raised its interrupt. Called from a thread, never from the
 * primary interrupt: the handler takes sleeping locks. */
void rtw89_glue_interrupt(void);

/* Valid between a successful probe and remove. */
bool rtw89_glue_get_info(struct rtw89_glue_info *info);

#ifdef __cplusplus
}
#endif

#endif /* _RTW89_GLUE_H */
