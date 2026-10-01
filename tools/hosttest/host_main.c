// SPDX-License-Identifier: GPL-2.0
/*
 * Smoke test: run the driver's real probe()/remove() in userspace against a
 * PCI device that does not answer. Nothing here can make the chip work; the
 * point is to walk the same code a first kext load walks (hw allocation,
 * firmware blob lookup and parsing, ring allocation, the power-on attempt and
 * its failure, every error unwind, compat teardown) where a bad pointer is a
 * crash report instead of a kernel panic.
 *
 *   hosttest 00         registers read back what was written (start at 0)
 *   hosttest ff         registers start as all-ones (device gone)
 *   hosttest 00 ok      probe must succeed: for the hosttest_fakechip binary,
 *                       where the hardware steps are replaced (fakechip_core.c)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rtw89_glue.h"

#define MMIO_LEN (1u << 20)

/* See kfree() in src/compat/linux/slab.h: userspace pointers are low. */
extern uintptr_t rtw88_kfree_min_addr;

/* tools/hosttest/selftest.c */
int rtw89_selftest(void);

/* src/compat_rtw89/rtw89_glue.c: where received beacons end up */
void rtw89_glue_note_bss(const uint8_t *frame, size_t len, uint16_t freq, int8_t signal);

/* A probed device: scan bookkeeping, and bringing the radio up, which must
 * fail cleanly here because nothing answers the power-on sequence. */
static int test_probed_device(void)
{
    static const uint8_t beacon[] = {
        0x80, 0x00, 0x00, 0x00,                             /* beacon */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,                 /* DA */
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,                 /* SA */
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,                 /* BSSID */
        0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x11, 0x04,     /* timestamp, interval, capab */
        0x00, 0x04, 't', 'e', 's', 't',                     /* SSID */
        0x03, 0x01, 0x06,                                   /* DS: channel 6 */
    };
    struct rtw89_glue_bss bss[4];
    int failures = 0, ret;

#define EXPECT(cond) do { if (!(cond)) { failures++; printf("== FAIL %s\n", #cond); } } while (0)
    rtw89_glue_note_bss(beacon, sizeof(beacon), 2437, -60);
    rtw89_glue_note_bss(beacon, sizeof(beacon), 2437, -55);
    rtw89_glue_note_bss(beacon, 10, 2437, -40);             /* truncated: ignored */
    EXPECT(rtw89_glue_scan_results(bss, 4) == 1);
    EXPECT(!strcmp(bss[0].ssid, "test") && bss[0].ssid_len == 4);
    EXPECT(bss[0].channel == 6 && bss[0].signal == -55 && bss[0].seen == 2);
    EXPECT(bss[0].capability == 0x0411);

    EXPECT(rtw89_glue_scan() != 0);                         /* radio is down */

    /* fakechip_core.c stubs the hardware start, so up() gets as far as adding
     * the interface; whether that and the scan succeed depends on commands
     * no firmware answers. What matters is that nothing crashes or leaks. */
    ret = rtw89_glue_up();
    printf("== radio up returned %d\n", ret);
    if (!ret) {
        ret = rtw89_glue_scan();
        printf("== scan returned %d\n", ret);
        /* Long enough for the driver's 2 s tracking work to run twice. */
        usleep(4500 * 1000);
        rtw89_glue_down();
        EXPECT(!rtw89_glue_is_up() && !rtw89_glue_scanning());
        printf("== radio down\n");
    }
#undef EXPECT
    return failures;
}

static uint8_t cfg[4096];
static unsigned long dma_live, dma_total;

static uint32_t cfg_read(void *ctx, unsigned int offset, unsigned int width)
{
    uint32_t v = 0;

    if (offset + width > sizeof(cfg))
        return 0xffffffff;
    memcpy(&v, cfg + offset, width);
    return v;
}

static void cfg_write(void *ctx, unsigned int offset, unsigned int width, uint32_t value)
{
    if (offset + width <= sizeof(cfg))
        memcpy(cfg + offset, &value, width);
}

/* The "bus address" is the virtual address: nothing ever dereferences it. */
static void *dma_alloc(void *ctx, size_t size, uint64_t *bus, void **cookie)
{
    void *p = NULL;

    if (posix_memalign(&p, 4096, (size + 4095) & ~4095ul))
        return NULL;
    memset(p, 0, size);
    *bus = (uint64_t)(uintptr_t)p;
    *cookie = p;
    dma_live++;
    dma_total++;
    return p;
}

static void dma_free(void *ctx, void *cookie)
{
    dma_live--;
    free(cookie);
}

int main(int argc, char **argv)
{
    struct rtw89_glue_platform plat = {
        .cfg_read = cfg_read, .cfg_write = cfg_write,
        .dma_alloc = dma_alloc, .dma_free = dma_free,
    };
    struct rtw89_glue_device dev = {
        .vendor = 0x10ec, .device = 0xb852,
        .subsystem_vendor = 0x1a3b, .subsystem_device = 0x5471,
        .mmio_len = MMIO_LEN,
    };
    struct rtw89_glue_info info;
    void *mmio;
    int ret, round, rounds = 2;
    int expect_ok = argc > 2 && !strcmp(argv[2], "ok");
    int failed = 0;

    rtw88_kfree_min_addr = 0;

    if (posix_memalign(&mmio, 4096, MMIO_LEN))
        return 1;
    dev.mmio_base = mmio;

    /* Config space: IDs, capability list -> PCI Express capability at 0x70. */
    cfg[0x00] = 0xec; cfg[0x01] = 0x10; cfg[0x02] = 0x52; cfg[0x03] = 0xb8;
    cfg[0x06] = 0x10;           /* status: capability list */
    cfg[0x34] = 0x70;
    cfg[0x70] = 0x10;           /* PCI Express */
    cfg[0x71] = 0x00;

    ret = rtw89_selftest();
    printf("== selftest: %d failure(s)\n", ret);
    if (ret)
        return 3;

    printf("== supports 10ec:b852: %d, 10ec:c822: %d\n",
           rtw89_glue_supports(0x10ec, 0xb852), rtw89_glue_supports(0x10ec, 0xc822));

    /* Twice: the second round proves the first one cleaned up after itself. */
    for (round = 1; round <= rounds; round++) {
        memset(mmio, argc > 1 && !strcmp(argv[1], "ff") ? 0xff : 0x00, MMIO_LEN);

        printf("== round %d: probe\n", round);
        ret = rtw89_glue_probe(&plat, &dev);
        printf("== round %d: probe returned %d, %lu DMA allocation(s) live, %lu total\n",
               round, ret, dma_live, dma_total);

        if (!ret && rtw89_glue_get_info(&info))
            printf("== MAC %02x:%02x:%02x:%02x:%02x:%02x fw %s, cut %c, %uT%uR\n",
                   info.mac[0], info.mac[1], info.mac[2], info.mac[3],
                   info.mac[4], info.mac[5], info.fw_version, 'A' + info.chip_cut,
                   info.tx_streams, info.rx_streams);
        if (expect_ok && ret)
            failed = 1;
        if (!ret && test_probed_device())
            failed = 1;

        /* Give queued works (regulatory hint, firmware load) time to run. */
        usleep(300 * 1000);

        rtw89_glue_interrupt();
        rtw89_glue_remove();
        printf("== round %d: removed, %lu DMA allocation(s) live\n", round, dma_live);
    }

    /* Let detached workqueue threads finish exiting before the process does. */
    usleep(200 * 1000);
    printf("== done\n");
    if (failed)
        return 4;
    return dma_live ? 2 : 0;
}
