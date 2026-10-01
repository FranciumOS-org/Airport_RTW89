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

void rtw89_glue_test_rx(const uint8_t *frame, size_t len, uint16_t freq, int8_t signal);

/* The access point the smoke test pretends to be, and the card (fakechip_core.c). */
static const uint8_t ap_mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01 };
static const uint8_t sta_mac[6] = { 0x00, 0xe0, 0x4c, 0x88, 0x52, 0xbe };

static const uint8_t ap_ies[] = {
    0x00, 0x07, 't', 'e', 's', 't', 'n', 'e', 't',                   /* SSID */
    0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24,      /* rates */
    0x03, 0x01, 0x06,                                                /* channel 6 */
    0x05, 0x04, 0x00, 0x02, 0x00, 0x00,                              /* TIM, DTIM period 2 */
    0x2a, 0x01, 0x00,                                                /* ERP */
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,      /* RSN: CCMP/PSK */
    0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
    0x32, 0x04, 0x30, 0x48, 0x60, 0x6c,                              /* extended rates */
    0x2d, 0x1a, 0x2c, 0x00, 0x03, 0xff, 0xff, 0x00, 0x00, 0x00,      /* HT capabilities */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3d, 0x16, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,      /* HT operation */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0xdd, 0x18, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x00, 0x00,      /* WMM parameters */
    0x03, 0xa4, 0x00, 0x00, 0x27, 0xa4, 0x00, 0x00,
    0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00,
};

/* Build a management frame from the AP to the card and deliver it. */
static void ap_send(uint8_t subtype, const uint8_t *body, size_t body_len)
{
    uint8_t frame[600];
    size_t len = 24;

    memset(frame, 0, sizeof(frame));
    frame[0] = subtype;
    memcpy(frame + 4, subtype == 0x80 ? (const uint8_t *)"\xff\xff\xff\xff\xff\xff" : sta_mac, 6);
    memcpy(frame + 10, ap_mac, 6);
    memcpy(frame + 16, ap_mac, 6);
    memcpy(frame + len, body, body_len);
    len += body_len;
    rtw89_glue_test_rx(frame, len, 2437, -40);
}

static void ap_send_beacon(void)
{
    uint8_t body[12 + sizeof(ap_ies)] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x11, 0x04 };

    memcpy(body + 12, ap_ies, sizeof(ap_ies));
    ap_send(0x80, body, sizeof(body));
}

static void ap_send_auth(uint16_t status)
{
    uint8_t body[6] = { 0x00, 0x00, 0x02, 0x00, (uint8_t)status, (uint8_t)(status >> 8) };

    ap_send(0xb0, body, sizeof(body));
}

static void ap_send_assoc_resp(uint16_t status)
{
    uint8_t body[6 + sizeof(ap_ies)] = { 0x11, 0x04, (uint8_t)status, (uint8_t)(status >> 8),
                                         0x05, 0xc0 };

    /* an association response carries the same elements minus the SSID */
    memcpy(body + 6, ap_ies + 9, sizeof(ap_ies) - 9);
    ap_send(0x10, body, 6 + sizeof(ap_ies) - 9);
}

static void ap_send_deauth(uint16_t reason)
{
    uint8_t body[2] = { (uint8_t)reason, (uint8_t)(reason >> 8) };

    ap_send(0xc0, body, sizeof(body));
}

/* First message of the WPA2 handshake: an unencrypted data frame with EAPOL. */
static void ap_send_eapol(void)
{
    uint8_t frame[24 + 8 + 99];

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08;                    /* data */
    frame[1] = 0x02;                    /* from the distribution system */
    memcpy(frame + 4, sta_mac, 6);
    memcpy(frame + 10, ap_mac, 6);
    memcpy(frame + 16, ap_mac, 6);
    memcpy(frame + 24, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8);
    frame[32] = 0x02;                   /* EAPOL version 2, type 3 (key) */
    frame[33] = 0x03;
    rtw89_glue_test_rx(frame, sizeof(frame), 2437, -40);
}

static enum rtw89_glue_link_state link_state(void)
{
    struct rtw89_glue_link link;

    rtw89_glue_link(&link);
    return link.state;
}

/*
 * Wait for the link to reach @state. Generous: against hardware that never
 * answers, every driver call the join makes runs into its own timeouts.
 */
static int wait_link(enum rtw89_glue_link_state state)
{
    int i;

    for (i = 0; i < 600 && link_state() != state; i++)
        usleep(50 * 1000);
    return link_state() == state;
}

/* Authenticate and associate with the pretend AP answering. */
static int join_testnet(uint16_t assoc_status)
{
    if (rtw89_glue_join((const uint8_t *)"testnet", 7))
        return 0;
    ap_send_auth(0);
    /* the association request goes out once the auth answer is processed */
    usleep(300 * 1000);
    ap_send_assoc_resp(assoc_status);
    return 1;
}

/* Join the pretend network, playing the AP's side of each exchange. */
static int test_join(void)
{
    struct rtw89_glue_link link;
    int failures = 0;

#define EXPECT(cond) do { if (!(cond)) { failures++; printf("== FAIL %s\n", #cond); } } while (0)
    ap_send_beacon();
    EXPECT(rtw89_glue_join((const uint8_t *)"nosuchnet", 9) == -2);     /* -ENOENT */

    /* the normal case: both answers arrive */
    EXPECT(join_testnet(0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(link.aid == 5 && link.freq == 2437 && !link.last_error);
    ap_send_eapol();
    usleep(300 * 1000);
    rtw89_glue_link(&link);
    EXPECT(link.eapol_rx == 1);
    printf("== associated, AID %u, %u EAPOL frame(s)\n", link.aid, link.eapol_rx);

    /* the AP throws us out */
    ap_send_deauth(7);
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -2007);

    /* refused association */
    EXPECT(join_testnet(17));
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -1017);

    /* an AP that never answers: three tries, then give up */
    EXPECT(rtw89_glue_join((const uint8_t *)"testnet", 7) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -110);

    /* leave while associated; then the radio goes down while associated */
    EXPECT(join_testnet(0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_leave();
    EXPECT(link_state() == RTW89_GLUE_LINK_DOWN);

    EXPECT(join_testnet(0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
#undef EXPECT
    printf("== join test: %d failure(s)\n", failures);
    return failures;
}

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
        failures += test_join();
        /* Long enough for the driver's 2 s tracking work to run, associated. */
        usleep(2500 * 1000);
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
