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

#include "rtw89_crypto.h"
#include "rtw89_glue.h"

#define MMIO_LEN (1u << 20)

/* See kfree() in src/compat/linux/slab.h: userspace pointers are low. */
extern uintptr_t rtw88_kfree_min_addr;

/* tools/hosttest/selftest.c */
int rtw89_selftest(void);

/* src/compat_rtw89/rtw89_glue.c: where received beacons end up */
void rtw89_glue_note_bss(const uint8_t *frame, size_t len, uint16_t freq, int8_t signal);

void rtw89_glue_test_rx(const uint8_t *frame, size_t len, uint16_t freq, int8_t signal,
                        bool decrypted);
/* src/compat_rtw89/rtw89_mlme.c and rtw89_data.c: see every management and
 * data frame the station transmits */
void rtw89_mlme_set_tx_tap(void (*tap)(const uint8_t *frame, size_t len));
void rtw89_data_set_tx_tap(void (*tap)(const uint8_t *frame, size_t len));

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
    rtw89_glue_test_rx(frame, len, 2437, -40, false);
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

/* ---- the authenticator side of WPA2-PSK, to test the supplicant against ---- */

#define AP_PASSWORD "correct horse battery"

static struct {
    uint8_t pmk[32], kck[16], kek[16], tk[16];
    uint8_t anonce[32], snonce[32];
    uint64_t replay;
    /* the last EAPOL frame the station sent, as seen by the TX tap */
    uint8_t eapol[256];
    size_t eapol_len;
    bool eapol_protected;
    uint64_t eapol_pn;          /* CCMP packet number, if it was protected */
    volatile int eapol_count;
    /* the last other data frame and the last action frame it sent */
    uint8_t data[2400];
    size_t data_len;
    int data_count;
    uint8_t action[64];
    size_t action_len;
    volatile int action_count;
    /* packet numbers of the frames we send it */
    uint64_t tx_pn, gtk_pn;
} ap;

/* What the station hands to the network stack. */
static struct {
    uint8_t frame[2400];
    size_t len;
    int count;
} sta_rx;

static void sta_rx_frame(void *ctx, const uint8_t *frame, size_t len)
{
    sta_rx.count++;
    sta_rx.len = len;
    if (len <= sizeof(sta_rx.frame))
        memcpy(sta_rx.frame, frame, len);
}

static void ap_tx_tap(const uint8_t *frame, size_t len)
{
    static const uint8_t llc[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };
    size_t hdrlen = (frame[0] & 0x80) ? 26 : 24;    /* QoS data has a QoS control field */
    bool prot = frame[1] & 0x40;

    if (frame[0] == 0xd0 && len <= sizeof(ap.action)) {
        memcpy(ap.action, frame, len);
        ap.action_len = len;
        ap.action_count++;
    }
    if ((frame[0] & 0x0c) != 0x08)                  /* not a data frame */
        return;
    if (prot) {
        const uint8_t *c = frame + hdrlen;          /* CCMP header */

        ap.eapol_pn = c[0] | c[1] << 8 | c[4] << 16 | (uint64_t)c[5] << 24 |
                      (uint64_t)c[6] << 32 | (uint64_t)c[7] << 40;
        hdrlen += 8;
    }
    if (len < hdrlen + 8 || memcmp(frame + hdrlen, llc, 8)) {
        if (len <= sizeof(ap.data)) {
            memcpy(ap.data, frame, len);
            ap.data_len = len;
            ap.data_count++;
        }
        return;
    }
    ap.eapol_len = len - hdrlen - 8;
    if (ap.eapol_len > sizeof(ap.eapol))
        return;
    memcpy(ap.eapol, frame + hdrlen + 8, ap.eapol_len);
    ap.eapol_protected = prot;
    ap.eapol_count++;
}

static int ap_wait_eapol(int count)
{
    int i;

    for (i = 0; i < 400 && ap.eapol_count < count; i++)
        usleep(25 * 1000);
    return ap.eapol_count >= count;
}

/* An EAPOL-Key frame from the AP; @kd is the plaintext key data (wrapped here
 * with the KEK when @key_info says it is encrypted). */
static void ap_send_key(uint16_t key_info, const uint8_t *kd, size_t kd_len, bool protect)
{
    uint8_t frame[24 + 8 + 8 + 4 + 95 + 128 + 8], *e, *k, mic[20];
    size_t hdr = 24 + (protect ? 8 : 0), wrapped = kd_len, len;
    int i;

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08;                    /* data */
    frame[1] = 0x02 | (protect ? 0x40 : 0);     /* from the DS, protected */
    memcpy(frame + 4, sta_mac, 6);
    memcpy(frame + 10, ap_mac, 6);
    memcpy(frame + 16, ap_mac, 6);
    if (protect) {
        /* the CCMP header: a packet number that goes up, key id 0 */
        ap.tx_pn++;
        frame[24] = (uint8_t)ap.tx_pn;
        frame[25] = (uint8_t)(ap.tx_pn >> 8);
        frame[27] = 0x20;
        frame[28] = (uint8_t)(ap.tx_pn >> 16);
    }
    memcpy(frame + hdr, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8);

    e = frame + hdr + 8;
    k = e + 4;
    if (key_info & 0x1000)
        wrapped = kd_len + 8;
    e[0] = 2;
    e[1] = 3;
    e[2] = (uint8_t)((95 + wrapped) >> 8);
    e[3] = (uint8_t)(95 + wrapped);
    k[0] = 2;
    k[1] = (uint8_t)(key_info >> 8);
    k[2] = (uint8_t)key_info;
    k[4] = 16;                          /* key length: CCMP */
    ap.replay++;
    for (i = 0; i < 8; i++)
        k[5 + i] = (uint8_t)(ap.replay >> (56 - 8 * i));
    if (key_info & 0x0008)
        memcpy(k + 13, ap.anonce, 32);
    if (key_info & 0x1000) {
        /* key RSC: how far the group key's packet numbers have got */
        k[61] = (uint8_t)ap.gtk_pn;
        k[62] = (uint8_t)(ap.gtk_pn >> 8);
    }
    k[93] = (uint8_t)(wrapped >> 8);
    k[94] = (uint8_t)wrapped;
    if (key_info & 0x1000)
        rtw89_aes_wrap(ap.kek, kd, kd_len, k + 95);
    else if (kd_len)
        memcpy(k + 95, kd, kd_len);
    if (key_info & 0x0100) {
        rtw89_hmac_sha1(ap.kck, 16, e, 4 + 95 + wrapped, mic);
        memcpy(k + 77, mic, 16);
    }

    len = hdr + 8 + 4 + 95 + wrapped + (protect ? 8 : 0);   /* + CCMP MIC */
    rtw89_glue_test_rx(frame, len, 2437, -40, protect);
}

/* Check the MIC of the station's last EAPOL-Key frame; returns its key info. */
static int ap_check_sta_key(void)
{
    uint8_t copy[256], mic[20];

    if (ap.eapol_len < 4 + 95)
        return -1;
    memcpy(copy, ap.eapol, ap.eapol_len);
    memset(copy + 4 + 77, 0, 16);
    rtw89_hmac_sha1(ap.kck, 16, copy, ap.eapol_len, mic);
    if (memcmp(mic, ap.eapol + 4 + 77, 16))
        return -1;
    return ap.eapol[5] << 8 | ap.eapol[6];
}

/* Message 3: our RSN element and the group key, encrypted with the KEK. */
static void ap_send_msg3(const uint8_t gtk[16], int gtk_idx)
{
    uint8_t kd[64];
    size_t kd_len;

    memcpy(kd, ap_ies + 31, 22);                    /* the RSN element of the beacon */
    kd[22] = 0xdd; kd[23] = 22; kd[24] = 0x00; kd[25] = 0x0f; kd[26] = 0xac; kd[27] = 1;
    kd[28] = (uint8_t)gtk_idx; kd[29] = 0;
    memcpy(kd + 30, gtk, 16);
    kd_len = 46;
    kd[kd_len++] = 0xdd;                            /* pad to a multiple of 8 */
    while (kd_len % 8)
        kd[kd_len++] = 0;
    ap_send_key(0x13ca, kd, kd_len, false);
}

/* Run the 4-way handshake as the authenticator. Returns 0 if the station did
 * everything right, 1 if its message 2 does not verify (wrong password). */
static int ap_handshake(const uint8_t gtk[16], int gtk_idx)
{
    uint8_t data[76], ptk[48];
    int base = ap.eapol_count, info;

    rtw89_pbkdf2_sha1((const uint8_t *)AP_PASSWORD, strlen(AP_PASSWORD),
                      (const uint8_t *)"testnet", 7, 4096, ap.pmk, 32);
    memset(ap.anonce, 0x5a, sizeof(ap.anonce));
    ap.anonce[0] = (uint8_t)ap.replay;              /* fresh per handshake */

    ap_send_key(0x008a, NULL, 0, false);            /* message 1: pairwise, ack */
    if (!ap_wait_eapol(base + 1) || ap.eapol_len < 4 + 95 + 22)
        return -1;
    memcpy(ap.snonce, ap.eapol + 4 + 13, 32);
    /* our retry of message 1 crosses the answer: the station must stay with
     * the SNonce it already sent, or message 3 would not verify */
    ap_send_key(0x008a, NULL, 0, false);
    if (!ap_wait_eapol(base + 2) || memcmp(ap.snonce, ap.eapol + 4 + 13, 32))
        return -1;

    /* min/max ordering of addresses and nonces, as the standard prescribes */
    memcpy(data, memcmp(ap_mac, sta_mac, 6) < 0 ? ap_mac : sta_mac, 6);
    memcpy(data + 6, memcmp(ap_mac, sta_mac, 6) < 0 ? sta_mac : ap_mac, 6);
    memcpy(data + 12, memcmp(ap.anonce, ap.snonce, 32) < 0 ? ap.anonce : ap.snonce, 32);
    memcpy(data + 44, memcmp(ap.anonce, ap.snonce, 32) < 0 ? ap.snonce : ap.anonce, 32);
    rtw89_sha1_prf(ap.pmk, 32, "Pairwise key expansion", data, sizeof(data), ptk, 48);
    memcpy(ap.kck, ptk, 16);
    memcpy(ap.kek, ptk + 16, 16);
    memcpy(ap.tk, ptk + 32, 16);

    info = ap_check_sta_key();
    if (info < 0)
        return 1;                                   /* MIC does not verify */
    if (info != 0x010a || ap.eapol_protected)
        return -1;
    /* message 2 carries the RSN element from the association request */
    if (ap.eapol[4 + 93] != 0 || ap.eapol[4 + 94] != 22 || ap.eapol[4 + 95] != 0x30)
        return -1;

    ap_send_msg3(gtk, gtk_idx);
    if (!ap_wait_eapol(base + 3))
        return -1;
    info = ap_check_sta_key();
    if (info != 0x030a || ap.eapol_protected)       /* message 4: pairwise, mic, secure */
        return -1;
    return 0;
}

/* A new group key, sent encrypted under the pairwise key like any data. */
static int ap_group_rekey(const uint8_t gtk[16], int gtk_idx)
{
    uint8_t kd[32];
    int base = ap.eapol_count, info;

    kd[0] = 0xdd; kd[1] = 22; kd[2] = 0x00; kd[3] = 0x0f; kd[4] = 0xac; kd[5] = 1;
    kd[6] = (uint8_t)gtk_idx; kd[7] = 0;
    memcpy(kd + 8, gtk, 16);
    ap_send_key(0x1382, kd, 24, true);              /* ack, mic, secure, encrypted */

    if (!ap_wait_eapol(base + 1))
        return -1;
    info = ap_check_sta_key();
    /* the answer is group message 2, and now it must be encrypted */
    return info == 0x0302 && ap.eapol_protected ? 0 : -1;
}

/* ---- data frames, once connected ---- */

/* An 802.11 data frame from the AP. @body is what follows the 802.11 header
 * (and CCMP header). @keyid < 0: in the clear. @tid < 0: not a QoS frame. */
static void ap_send_data(const uint8_t *da, const uint8_t *sa, int tid, bool amsdu, bool retry,
                         uint16_t seq, int keyid, uint64_t pn, bool hw_decrypted,
                         const uint8_t *body, size_t body_len)
{
    uint8_t frame[2400];
    size_t len = 24;

    memset(frame, 0, 48);
    frame[0] = tid >= 0 ? 0x88 : 0x08;
    frame[1] = 0x02 | (retry ? 0x08 : 0) | (keyid >= 0 ? 0x40 : 0);
    memcpy(frame + 4, da, 6);
    memcpy(frame + 10, ap_mac, 6);
    memcpy(frame + 16, sa, 6);
    frame[22] = (uint8_t)(seq << 4);
    frame[23] = (uint8_t)(seq >> 4);
    if (tid >= 0) {
        frame[24] = (uint8_t)tid | (amsdu ? 0x80 : 0);
        len = 26;
    }
    if (keyid >= 0) {
        frame[len] = (uint8_t)pn;
        frame[len + 1] = (uint8_t)(pn >> 8);
        frame[len + 3] = 0x20 | (uint8_t)(keyid << 6);
        frame[len + 4] = (uint8_t)(pn >> 16);
        len += 8;
    }
    memcpy(frame + len, body, body_len);
    len += body_len;
    if (keyid >= 0) {
        memset(frame + len, 0xee, 8);       /* where the MIC was */
        len += 8;
    }
    rtw89_glue_test_rx(frame, len, 2437, -40, keyid >= 0 && hw_decrypted);
}

/* Hand an Ethernet frame to the driver the way the kext does. */
static int sta_tx(const uint8_t *eth, size_t len)
{
    uint8_t *buf = NULL;
    void *handle = rtw89_glue_tx_alloc(len, &buf);

    if (!handle)
        return -12;
    memcpy(buf, eth, len);
    return rtw89_glue_tx(handle);
}

static const uint8_t peer_mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t bcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* An IPv4/UDP packet with DSCP "expedited forwarding", so it maps to TID 5. */
static const uint8_t test_ip[] = {
    0x45, 0xb8, 0x00, 0x24, 0x12, 0x34, 0x00, 0x00, 0x40, 0x11, 0x00, 0x00,
    192, 168, 1, 10, 192, 168, 1, 1,
    0x30, 0x39, 0x00, 0x35, 0x00, 0x10, 0x00, 0x00,
    'h', 'e', 'l', 'l', 'o', ' ', 'a', 'p',
};

/* A small frame from this interface; returns what the driver said. */
static int sta_tx_test(void)
{
    uint8_t eth[14 + sizeof(test_ip)];

    memcpy(eth, peer_mac, 6);
    memcpy(eth + 6, sta_mac, 6);
    eth[12] = 0x08; eth[13] = 0x00;
    memcpy(eth + 14, test_ip, sizeof(test_ip));
    return sta_tx(eth, sizeof(eth));
}

/* Both directions of the data path, with the handshake done. gtk ids 1 and 2
 * are installed. Returns the number of failures. */
static int test_data(void)
{
    uint8_t eth[14 + sizeof(test_ip)], body[8 + sizeof(test_ip)], amsdu[2 * (14 + 8 + 64)];
    struct rtw89_glue_link link;
    const uint8_t *f = ap.data;
    int failures = 0, n;
    uint64_t pn;
    size_t len;

#define EXPECT(cond) do { if (!(cond)) { failures++; printf("== FAIL %s\n", #cond); } } while (0)
    /* ---- transmit ---- */
    memcpy(eth, peer_mac, 6);
    memcpy(eth + 6, sta_mac, 6);
    eth[12] = 0x08; eth[13] = 0x00;
    memcpy(eth + 14, test_ip, sizeof(test_ip));
    pn = ap.eapol_pn;
    n = ap.data_count;
    EXPECT(sta_tx(eth, sizeof(eth)) == 0);
    EXPECT(ap.data_count == n + 1);
    /* QoS data to the DS, protected; receiver the AP, source us, destination the peer */
    EXPECT(f[0] == 0x88 && f[1] == 0x41);
    EXPECT(!memcmp(f + 4, ap_mac, 6) && !memcmp(f + 10, sta_mac, 6) && !memcmp(f + 16, peer_mac, 6));
    EXPECT((f[24] & 0x0f) == 5 && !(f[24] & 0x80));
    /* CCMP header: extended IV, key id 0, the next packet number */
    EXPECT(f[29] == 0x20 && (f[26] | f[27] << 8) == (int)pn + 1);
    EXPECT(!memcmp(f + 34, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8));
    EXPECT(ap.data_len == 34 + 8 + sizeof(test_ip) && !memcmp(f + 42, test_ip, sizeof(test_ip)));
    /* sequence numbers count per TID */
    EXPECT(sta_tx(eth, sizeof(eth)) == 0);
    EXPECT((f[22] >> 4 | f[23] << 4) == 1 && (f[26] | f[27] << 8) == (int)pn + 2);
    /* an 802.3 frame (a length instead of a type) keeps its own LLC header */
    eth[12] = 0x00; eth[13] = sizeof(test_ip);
    EXPECT(sta_tx(eth, sizeof(eth)) == 0);
    EXPECT((f[24] & 0x0f) == 0 && ap.data_len == 34 + sizeof(test_ip) &&
           !memcmp(f + 34, test_ip, sizeof(test_ip)));
    /* frames that are not from this interface are not sent */
    memcpy(eth + 6, peer_mac, 6);
    EXPECT(sta_tx(eth, sizeof(eth)) == -22);
    EXPECT(sta_tx(eth, 10) == -12);

    /* ---- receive ---- */
    memcpy(body, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8);
    memcpy(body + 8, test_ip, sizeof(test_ip));
    pn = ap.tx_pn + 10;

    n = sta_rx.count;
    ap_send_data(sta_mac, peer_mac, 0, false, false, 100, 0, pn, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 1 && sta_rx.len == 14 + sizeof(test_ip));
    EXPECT(!memcmp(sta_rx.frame, sta_mac, 6) && !memcmp(sta_rx.frame + 6, peer_mac, 6));
    EXPECT(sta_rx.frame[12] == 0x08 && sta_rx.frame[13] == 0x00 &&
           !memcmp(sta_rx.frame + 14, test_ip, sizeof(test_ip)));

    /* the same packet number again: a replay */
    ap_send_data(sta_mac, peer_mac, 0, false, false, 101, 0, pn, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 1);
    /* a retry of a frame already received */
    ap_send_data(sta_mac, peer_mac, 0, false, true, 101, 0, pn + 1, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 1);
    /* packet numbers count per TID: this one is new on TID 3 */
    ap_send_data(sta_mac, peer_mac, 3, false, false, 7, 0, pn, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 2);
    /* in the clear on an encrypted network, or not decrypted by the chip */
    ap_send_data(sta_mac, peer_mac, 0, false, false, 102, -1, 0, false, body, sizeof(body));
    ap_send_data(sta_mac, peer_mac, 0, false, false, 103, 0, pn + 2, false, body, sizeof(body));
    /* for somebody else, or from another access point's client */
    ap_send_data(peer_mac, peer_mac, 0, false, false, 104, 0, pn + 3, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 2);

    /* broadcast: the group key with the id the AP names, above its RSC */
    ap_send_data(bcast_mac, peer_mac, -1, false, false, 1, 2, ap.gtk_pn, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 2);
    ap_send_data(bcast_mac, peer_mac, -1, false, false, 2, 2, ap.gtk_pn + 1, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 3 && !memcmp(sta_rx.frame, bcast_mac, 6));
    ap_send_data(bcast_mac, peer_mac, -1, false, false, 3, 1, ap.gtk_pn + 1, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 4);
    /* a group key id that was never installed, and our own broadcast echoed */
    ap_send_data(bcast_mac, peer_mac, -1, false, false, 4, 3, ap.gtk_pn + 2, true, body, sizeof(body));
    ap_send_data(bcast_mac, sta_mac, -1, false, false, 5, 2, ap.gtk_pn + 2, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 4);
    /* a frame without SNAP header arrives as 802.3 with a length */
    ap_send_data(sta_mac, peer_mac, 0, false, false, 105, 0, pn + 4, true, test_ip, sizeof(test_ip));
    EXPECT(sta_rx.count == n + 5 && sta_rx.len == 14 + sizeof(test_ip));
    EXPECT(sta_rx.frame[12] == 0 && sta_rx.frame[13] == sizeof(test_ip));

    /* an A-MSDU with two subframes, the first padded to four bytes */
    memset(amsdu, 0, sizeof(amsdu));
    memcpy(amsdu, sta_mac, 6);
    memcpy(amsdu + 6, peer_mac, 6);
    amsdu[13] = 8 + 21;
    memcpy(amsdu + 14, "\xaa\xaa\x03\x00\x00\x00\x08\x06", 8);
    memset(amsdu + 22, 0x11, 21);
    len = (14 + 8 + 21 + 3) & ~3u;
    memcpy(amsdu + len, sta_mac, 6);
    memcpy(amsdu + len + 6, peer_mac, 6);
    amsdu[len + 13] = 8 + 30;
    memcpy(amsdu + len + 14, "\xaa\xaa\x03\x00\x00\x00\x86\xdd", 8);
    memset(amsdu + len + 22, 0x22, 30);
    ap_send_data(sta_mac, peer_mac, 0, true, false, 106, 0, pn + 5, true, amsdu, len + 14 + 8 + 30);
    EXPECT(sta_rx.count == n + 7 && sta_rx.len == 14 + 30);
    EXPECT(sta_rx.frame[12] == 0x86 && sta_rx.frame[13] == 0xdd && sta_rx.frame[14 + 29] == 0x22);
    /* a normal frame with the A-MSDU bit set by an attacker */
    ap_send_data(sta_mac, peer_mac, 0, true, false, 107, 0, pn + 6, true, body, sizeof(body));
    EXPECT(sta_rx.count == n + 7);

    rtw89_glue_link(&link);
    EXPECT(link.tx_frames >= 3 && link.rx_frames >= 7 && link.rx_replay >= 2 &&
           link.rx_undecrypted >= 1);

    /* the AP asks for a BlockAck agreement: declined for now */
    n = ap.action_count;
    {
        static const uint8_t addba[] = { 3, 0, 9, 0x02, 0x10, 0x00, 0x00, 0x10, 0x00 };
        int i;

        ap_send(0xd0, addba, sizeof(addba));
        for (i = 0; i < 100 && ap.action_count == n; i++)
            usleep(20 * 1000);
    }
    EXPECT(ap.action_count == n + 1 && ap.action_len == 24 + 9);
    EXPECT(ap.action[24] == 3 && ap.action[25] == 1 && ap.action[26] == 9 &&
           ap.action[27] == 37 && ap.action[28] == 0);
#undef EXPECT
    printf("== data path test: %d failure(s)\n", failures);
    return failures;
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
static int join_testnet(const char *password, uint16_t assoc_status)
{
    if (rtw89_glue_join((const uint8_t *)"testnet", 7, password, strlen(password)))
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
    static const uint8_t gtk1[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    static const uint8_t gtk2[16] = { 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 };
    struct rtw89_glue_link link;
    int failures = 0;

#define EXPECT(cond) do { if (!(cond)) { failures++; printf("== FAIL %s\n", #cond); } } while (0)
    rtw89_mlme_set_tx_tap(ap_tx_tap);
    rtw89_data_set_tx_tap(ap_tx_tap);
    ap_send_beacon();
    EXPECT(rtw89_glue_join((const uint8_t *)"nosuchnet", 9, AP_PASSWORD, strlen(AP_PASSWORD)) == -2);
    /* a WPA2 network needs a password of 8 to 63 characters */
    EXPECT(rtw89_glue_join((const uint8_t *)"testnet", 7, NULL, 0) == -13);
    EXPECT(rtw89_glue_join((const uint8_t *)"testnet", 7, "short", 5) == -13);

    /* the normal case: associate, then the complete 4-way handshake */
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(link.aid == 5 && link.freq == 2437 && !link.last_error);
    EXPECT(ap_handshake(gtk1, 1) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
    printf("== connected: 4-way handshake verified by the test authenticator\n");

    /* connected: the AP renews the group key, encrypted with the pairwise key */
    EXPECT(ap_group_rekey(gtk2, 2) == 0);
    EXPECT(ap.eapol_pn == 1);
    /* Message 4 got lost and the AP repeats message 3. The station answers
     * again but must keep counting packet numbers: installing the same key
     * again would restart them (KRACK). */
    {
        int base = ap.eapol_count;

        ap_send_msg3(gtk2, 2);
        EXPECT(ap_wait_eapol(base + 1));
        EXPECT(ap_check_sta_key() == 0x030a && ap.eapol_protected && ap.eapol_pn == 2);
    }
    /* a replayed message 3 (old replay counter) must be ignored */
    {
        int base = ap.eapol_count;

        ap.replay -= 2;
        ap_send_msg3(gtk1, 1);
        ap.replay += 1;
        /* and so must a forged message 1: it is not authenticated, and must
         * not disturb the keys of the working connection */
        ap.replay += 100;
        memset(ap.anonce, 0x77, sizeof(ap.anonce));
        ap_send_key(0x008a, NULL, 0, false);
        usleep(300 * 1000);
        EXPECT(link_state() == RTW89_GLUE_LINK_CONNECTED);
        /* the forged message 1 got an answer (it could be a real rekey), the
         * replay did not */
        EXPECT(ap.eapol_count == base + 1);
        /* the real AP's next group key still verifies with the real key */
        EXPECT(ap_group_rekey(gtk1, 1) == 0);
        EXPECT(ap.eapol_pn == 4);
    }

    failures += test_data();

    /* the AP throws us out; nothing can be sent after that */
    ap_send_deauth(7);
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -2007);
    EXPECT(sta_tx_test() == -100);

    /* wrong password: the AP cannot verify message 2 and gives up */
    EXPECT(join_testnet("not the password", 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    EXPECT(ap_handshake(gtk1, 1) == 1);
    EXPECT(link_state() == RTW89_GLUE_LINK_ASSOCIATED);
    /* associated but not authorised: no data in either direction */
    EXPECT(sta_tx_test() == -100);
    {
        int n = sta_rx.count;

        ap_send_data(sta_mac, peer_mac, 0, false, false, 1, -1, 0, false,
                     (const uint8_t *)"\xaa\xaa\x03\x00\x00\x00\x08\x00test", 12);
        EXPECT(sta_rx.count == n);
    }
    ap_send_deauth(15);
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -2015);

    /* refused association */
    EXPECT(join_testnet(AP_PASSWORD, 17));
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -1017);

    /* an AP that never answers: three tries, then give up */
    EXPECT(rtw89_glue_join((const uint8_t *)"testnet", 7, AP_PASSWORD, strlen(AP_PASSWORD)) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -110);

    /* an AP that associates us and then never starts the handshake */
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -110);

    /* leave while connected; then the radio goes down while connected */
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    EXPECT(ap_handshake(gtk1, 1) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
    rtw89_glue_leave();
    EXPECT(link_state() == RTW89_GLUE_LINK_DOWN);

    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    EXPECT(ap_handshake(gtk2, 2) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
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
        .rx_frame = sta_rx_frame,
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
