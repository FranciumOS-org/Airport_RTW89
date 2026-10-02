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
#include <pthread.h>
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
/* ... left waiting for the chip's status report, as the driver leaves data frames */
void rtw89_glue_test_scan_done(void);
void rtw89_glue_test_beacon_loss(void);
void rtw89_glue_test_rx_parked(const uint8_t *frame, size_t len, uint16_t freq, int8_t signal,
                               bool decrypted);
/* src/compat_rtw89/rtw89_mlme.c and rtw89_data.c: see every management and
 * data frame the station transmits */
void rtw89_mlme_set_tx_tap(void (*tap)(const uint8_t *frame, size_t len));
void rtw89_data_set_tx_tap(void (*tap)(const uint8_t *frame, size_t len));
/* src/compat_rtw89/rtw89_data.c: frames waiting on a TID's transmit queue */
unsigned int rtw89_data_tx_waiting(uint8_t tid);

/* The access point the smoke test pretends to be, and the card (fakechip_core.c). */
static uint8_t ap_mac[6] = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01 };
static const uint8_t sta_mac[6] = { 0x00, 0xe0, 0x4c, 0x88, 0x52, 0xbe };

static uint8_t ap_ies[] = {
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
#define AP_RSN_AKM      50      /* the key management type in the RSN element */
#define AP_DS_CHANNEL   21      /* offsets into ap_ies: the channel in the DS element, */
#define AP_HT_CHANNEL   (sizeof(ap_ies) - 26 - 22)      /* in the HT operation element, */
#define AP_WMM_COUNT    (sizeof(ap_ies) - 26 + 8)       /* and the WMM parameter set count */

/* A second network: 5 GHz channel 36, 80 MHz wide (centre channel 42), 802.11ac. */
static uint8_t ap5_ies[] = {
    0x00, 0x08, 't', 'e', 's', 't', 'n', 'e', 't', '5',              /* SSID */
    0x01, 0x08, 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c,      /* rates */
    0x05, 0x04, 0x00, 0x02, 0x00, 0x00,                              /* TIM */
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,      /* RSN: CCMP/PSK */
    0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
    0x2d, 0x1a, 0xef, 0x01, 0x03, 0xff, 0xff, 0x00, 0x00, 0x00,      /* HT capabilities, 40 MHz */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x3d, 0x16, 0x24, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,      /* HT operation: 36, */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,      /* secondary above */
    0x00, 0x00, 0x00, 0x00,
    0xbf, 0x0c, 0x32, 0x00, 0x00, 0x00, 0xfa, 0xff, 0x00, 0x00,      /* VHT capabilities: */
    0xfa, 0xff, 0x00, 0x00,                                          /* 2 streams, MCS 0-9 */
#define AP5_VHT_OPER 114                                             /* offset of the next element */
    0xc0, 0x05, 0x01, 0x2a, 0x00, 0xfc, 0xff,                        /* VHT operation: 80, centre 42 */
    0xdd, 0x18, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x00, 0x00,      /* WMM parameters */
    0x03, 0xa4, 0x00, 0x00, 0x27, 0xa4, 0x00, 0x00,
    0x42, 0x43, 0x5e, 0x00, 0x62, 0x32, 0x2f, 0x00,
};

/* What an 802.11ax AP adds to either network. */
static uint8_t ap_he_ies[] = {
    0xff, 0x16, 0x23,                                                /* HE capabilities: */
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00,                              /* MAC: HT control field */
    0x06, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* PHY: 40 and 80 MHz, LDPC */
    0xfa, 0xff, 0xfa, 0xff,                                          /* 2 streams, MCS 0-11 */
#define AP_HE_OPER 24                                                /* offset of the next element */
    0xff, 0x07, 0x24, 0xf4, 0x3f, 0x00, 0x05,                        /* HE operation: colour 5, */
    0xfc, 0xff,                                                      /* MCS 0-7 on 1 stream required */
    0xff, 0x0e, 0x26, 0x01,                                          /* MU EDCA parameters */
    0x03, 0xa4, 0x08, 0x27, 0xa4, 0x08, 0x42, 0x43, 0x08, 0x62, 0x32, 0x08,
};
static bool ap_he;              /* the beacon has them */
static bool ap_he_resp;         /* ... and the association response */

/* More elements for the beacons alone, such as a channel switch announcement. */
static const uint8_t *ap_beacon_extra;
static size_t ap_beacon_extra_len;

/* Which network the pretend AP is at the moment. */
static const uint8_t *ap_cur_ies = ap_ies;
static size_t ap_cur_ies_len = sizeof(ap_ies);
static uint16_t ap_freq = 2437;
static const char *ap_ssid = "testnet";

static void ap_select(bool five_ghz)
{
    ap_cur_ies = five_ghz ? ap5_ies : ap_ies;
    ap_cur_ies_len = five_ghz ? sizeof(ap5_ies) : sizeof(ap_ies);
    ap_freq = five_ghz ? 5180 : 2437;
    ap_ssid = five_ghz ? "testnet5" : "testnet";
    ap_mac[5] = five_ghz ? 0x05 : 0x01;
}

/* The element with id @eid in the current network's beacon. */
static const uint8_t *ap_find_ie(uint8_t eid)
{
    size_t off = 0;

    while (off + 2 <= ap_cur_ies_len && ap_cur_ies[off] != eid)
        off += 2 + ap_cur_ies[off + 1];
    return off + 2 <= ap_cur_ies_len ? ap_cur_ies + off : NULL;
}

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
    rtw89_glue_test_rx(frame, len, ap_freq, -40, false);
}

static void ap_send_beacon(void)
{
    uint8_t body[12 + 400] = { 0, 0, 0, 0, 0, 0, 0, 0, 0x64, 0x00, 0x11, 0x04 };

    size_t len = 12 + ap_cur_ies_len;

    memcpy(body + 12, ap_cur_ies, ap_cur_ies_len);
    if (ap_he) {
        memcpy(body + len, ap_he_ies, sizeof(ap_he_ies));
        len += sizeof(ap_he_ies);
    }
    if (ap_beacon_extra) {
        memcpy(body + len, ap_beacon_extra, ap_beacon_extra_len);
        len += ap_beacon_extra_len;
    }
    ap_send(0x80, body, len);
}

static void ap_send_auth(uint16_t status)
{
    uint8_t body[6] = { 0x00, 0x00, 0x02, 0x00, (uint8_t)status, (uint8_t)(status >> 8) };

    ap_send(0xb0, body, sizeof(body));
}

static void ap_send_assoc_resp(uint16_t status)
{
    uint8_t body[6 + 400] = { 0x11, 0x04, (uint8_t)status, (uint8_t)(status >> 8), 0x05, 0xc0 };
    size_t ssid = 2 + ap_cur_ies[1], len = 6 + ap_cur_ies_len - ssid;

    /* an association response carries the same elements minus the SSID */
    memcpy(body + 6, ap_cur_ies + ssid, ap_cur_ies_len - ssid);
    if (ap_he && ap_he_resp) {
        memcpy(body + len, ap_he_ies, sizeof(ap_he_ies));
        len += sizeof(ap_he_ies);
    }
    ap_send(0x10, body, len);
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
    /* the last association request */
    uint8_t assoc_req[400];
    size_t assoc_req_len;
    volatile int auth_count;    /* authentication requests */
    volatile int null_count;    /* null data frames */
    /* BlockAck action frames from the station, by TID */
    struct {
        volatile int count;
        uint8_t dialog;
        uint16_t ssn, capab;
        int ext;                /* the ADDBA extension element's byte, -1 without one */
    } addba_req[16];
    struct {
        volatile int count;
        uint8_t dialog;
        uint16_t status, capab;
        int ext;
    } addba_resp[16];
    struct {
        volatile int count;
        bool initiator;
    } delba[16];
    /* packet numbers of the frames we send it */
    uint64_t tx_pn, gtk_pn;
} ap;

/* What the station hands to the network stack. */
static struct {
    uint8_t frame[2400];
    size_t len;
    volatile int count;
    uint8_t order[256];         /* the last byte of each frame, in arrival order */
} sta_rx;

/* How often the driver told the stack that it has room for frames again. */
static volatile int sta_tx_wakes;

static void sta_tx_wake(void *ctx)
{
    sta_tx_wakes++;
}

static void sta_rx_frame(void *ctx, const uint8_t *frame, size_t len)
{
    sta_rx.order[sta_rx.count % 256] = frame[len - 1];
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

    if (frame[0] == 0x00 && len <= sizeof(ap.assoc_req)) {
        memcpy(ap.assoc_req, frame, len);
        ap.assoc_req_len = len;
    }
    if (frame[0] == 0xb0)
        ap.auth_count++;
    if ((frame[0] & 0x4c) == 0x48) {                /* data without a body */
        ap.null_count++;
        return;
    }
    if (frame[0] == 0xd0 && len >= 24 + 6 && frame[24] == 3) {
        const uint8_t *b = frame + 26;      /* after category and action code */
        /* both ADDBA frames have 7 more fixed bytes, then elements */
        int ext = len >= 24 + 9 + 3 && frame[33] == 159 && frame[34] == 1 ? frame[35] : -1;
        int tid;

        if (frame[25] == 0 && len >= 24 + 9) {          /* ADDBA request */
            tid = (b[1] >> 2) & 0x0f;
            ap.addba_req[tid].dialog = b[0];
            ap.addba_req[tid].capab = (uint16_t)(b[1] | b[2] << 8);
            ap.addba_req[tid].ssn = (uint16_t)((b[5] | b[6] << 8) >> 4);
            ap.addba_req[tid].ext = ext;
            ap.addba_req[tid].count++;
        } else if (frame[25] == 1 && len >= 24 + 9) {   /* ADDBA response */
            tid = (b[3] >> 2) & 0x0f;
            ap.addba_resp[tid].dialog = b[0];
            ap.addba_resp[tid].status = (uint16_t)(b[1] | b[2] << 8);
            ap.addba_resp[tid].capab = (uint16_t)(b[3] | b[4] << 8);
            ap.addba_resp[tid].ext = ext;
            ap.addba_resp[tid].count++;
        } else if (frame[25] == 2) {                    /* DELBA */
            tid = b[1] >> 4;
            ap.delba[tid].initiator = b[1] & 0x08;
            ap.delba[tid].count++;
        }
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
    rtw89_glue_test_rx(frame, len, ap_freq, -40, protect);
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

    memcpy(kd, ap_find_ie(0x30), 22);               /* the RSN element of the beacon */
    kd[22] = 0xdd; kd[23] = 22; kd[24] = 0x00; kd[25] = 0x0f; kd[26] = 0xac; kd[27] = 1;
    kd[28] = (uint8_t)gtk_idx; kd[29] = 0;
    memcpy(kd + 30, gtk, 16);
    kd_len = 46;
    kd[kd_len++] = 0xdd;                            /* pad to a multiple of 8 */
    while (kd_len % 8)
        kd[kd_len++] = 0;
    ap_send_key(0x13ca, kd, kd_len, false);
}

/* Part of an 802.1X sign-in: an EAP request for the station's identity. */
static void ap_send_eap(void)
{
    uint8_t frame[24 + 8 + 4 + 5];

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x08;
    frame[1] = 0x02;
    memcpy(frame + 4, sta_mac, 6);
    memcpy(frame + 10, ap_mac, 6);
    memcpy(frame + 16, ap_mac, 6);
    memcpy(frame + 24, "\xaa\xaa\x03\x00\x00\x00\x88\x8e", 8);
    memcpy(frame + 32, "\x02\x00\x00\x05\x01\x01\x00\x05\x01", 9);
    rtw89_glue_test_rx(frame, sizeof(frame), ap_freq, -40, false);
}

/* Run the 4-way handshake as the authenticator with the master key in ap.pmk.
 * Returns 0 if the station did everything right, 1 if its message 2 does not
 * verify (wrong key). */
static int ap_handshake_pmk(const uint8_t gtk[16], int gtk_idx)
{
    uint8_t data[76], ptk[48];
    int base = ap.eapol_count, info;

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

/* The same with the master key that follows from the network's password. */
static int ap_handshake(const uint8_t gtk[16], int gtk_idx)
{
    rtw89_pbkdf2_sha1((const uint8_t *)AP_PASSWORD, strlen(AP_PASSWORD),
                      (const uint8_t *)ap_ssid, strlen(ap_ssid), 4096, ap.pmk, 32);
    return ap_handshake_pmk(gtk, gtk_idx);
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
    rtw89_glue_test_rx(frame, len, ap_freq, -40, keyid >= 0 && hw_decrypted);
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

    /* A frame the driver is holding for a status report that never comes is
     * passed on after a couple of milliseconds, not at the next reception. */
    {
        uint8_t frame[26 + 8 + sizeof(body) + 8];
        uint32_t flushed;

        memset(frame, 0, sizeof(frame));
        frame[0] = 0x88; frame[1] = 0x42;
        memcpy(frame + 4, sta_mac, 6);
        memcpy(frame + 10, ap_mac, 6);
        memcpy(frame + 16, peer_mac, 6);
        frame[22] = 0x10; frame[23] = 0x07;                 /* sequence number 113 */
        frame[24] = 0x01;                                   /* TID 1 */
        frame[26] = (uint8_t)(pn + 50); frame[27] = (uint8_t)((pn + 50) >> 8);
        frame[29] = 0x20; frame[30] = (uint8_t)((pn + 50) >> 16);
        memcpy(frame + 34, body, sizeof(body));
        rtw89_glue_link(&link);
        flushed = link.rx_ppdu_flushed;
        n = sta_rx.count;
        rtw89_glue_test_rx_parked(frame, sizeof(frame), 2437, -40, true);
        EXPECT(sta_rx.count == n);
        usleep(100 * 1000);
        EXPECT(sta_rx.count == n + 1 && sta_rx.len == 14 + sizeof(test_ip));
        rtw89_glue_link(&link);
        EXPECT(link.rx_ppdu_flushed == flushed + 1 && link.ppdu_flush);
    }

#undef EXPECT
    printf("== data path test: %d failure(s)\n", failures);
    return failures;
}

/* Wait (up to two seconds) for *@count to pass @above. */
static int wait_count(volatile int *count, int above)
{
    int i;

    for (i = 0; i < 100 && *count <= above; i++)
        usleep(20 * 1000);
    return *count > above;
}

static uint16_t link_ba(bool tx)
{
    struct rtw89_glue_link link;

    rtw89_glue_link(&link);
    return tx ? link.tx_ba : link.rx_ba;
}

/* One frame of the AP's aggregate on TID 0: sequence number @sn, marked with
 * its low byte so the order of arrival at the stack can be checked. */
static void ap_send_agg(uint16_t sn, uint64_t pn_base)
{
    uint8_t body[8 + sizeof(test_ip)];

    memcpy(body, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8);
    memcpy(body + 8, test_ip, sizeof(test_ip));
    body[sizeof(body) - 1] = (uint8_t)sn;
    ap_send_data(sta_mac, peer_mac, 0, false, false, sn, 0, pn_base + sn, true, body, sizeof(body));
}

static void ap_send_addba_resp(int tid, uint8_t dialog, uint16_t status)
{
    uint16_t capab = (uint16_t)(0x0002 | tid << 2 | 64 << 6);
    uint8_t body[9] = { 3, 1, dialog, (uint8_t)status, (uint8_t)(status >> 8),
                        (uint8_t)capab, (uint8_t)(capab >> 8), 0, 0 };

    ap_send(0xd0, body, sizeof(body));
}

/* A frame from this interface on @tid (through the DSCP field). */
static int sta_tx_tid(int tid)
{
    uint8_t eth[14 + sizeof(test_ip)];

    memcpy(eth, peer_mac, 6);
    memcpy(eth + 6, sta_mac, 6);
    eth[12] = 0x08; eth[13] = 0x00;
    memcpy(eth + 14, test_ip, sizeof(test_ip));
    eth[15] = (uint8_t)(tid << 5);
    return sta_tx(eth, sizeof(eth));
}

/* BlockAck sessions in both directions, connected. */
static int test_aggregation(void)
{
    const uint64_t pn = ap.tx_pn + 1000;
    int failures = 0, n, i;

#define EXPECT(cond) do { if (!(cond)) { failures++; printf("== FAIL %s\n", #cond); } } while (0)
#define ORDER(k) sta_rx.order[(n + (k)) % 256]
    /* ---- the AP sends us aggregates on TID 0, starting at 300 ---- */
    {
        /* dialog 9; A-MSDU ok, immediate, TID 0, 64 frames; no timeout; start 300 */
        static const uint8_t addba[] = { 3, 0, 9, 0x03, 0x10, 0x00, 0x00, 0xc0, 0x12 };

        n = ap.addba_resp[0].count;
        ap_send(0xd0, addba, sizeof(addba));
        EXPECT(wait_count(&ap.addba_resp[0].count, n));
        EXPECT(ap.addba_resp[0].dialog == 9 && ap.addba_resp[0].status == 0);
        EXPECT(ap.addba_resp[0].capab == 0x1003);
        EXPECT(link_ba(false) == 0x0001);
    }

    /* in order: straight through */
    n = sta_rx.count;
    ap_send_agg(300, pn);
    EXPECT(sta_rx.count == n + 1);
    /* 301 is missing: 302 and 303 wait for it, then all three go on in order */
    ap_send_agg(302, pn);
    ap_send_agg(303, pn);
    EXPECT(sta_rx.count == n + 1);
    ap_send_agg(301, pn);
    EXPECT(sta_rx.count == n + 4);
    EXPECT(ORDER(1) == (301 & 0xff) && ORDER(2) == (302 & 0xff) && ORDER(3) == (303 & 0xff));
    /* a retransmission of something already handed on, and one of a frame waiting */
    ap_send_agg(301, pn);
    ap_send_agg(306, pn);
    ap_send_agg(306, pn);
    EXPECT(sta_rx.count == n + 4);
    /* 304 and 305 never come: 306 is released once it has waited long enough */
    usleep(300 * 1000);
    EXPECT(sta_rx.count == n + 5 && ORDER(4) == (306 & 0xff));
    /* a BlockAck request tells us to stop waiting for what is before 310 */
    ap_send_agg(309, pn);
    EXPECT(sta_rx.count == n + 5);
    {
        uint8_t bar[20] = { 0x84, 0x00, 0, 0 };

        memcpy(bar + 4, sta_mac, 6);
        memcpy(bar + 10, ap_mac, 6);
        bar[16] = 0x04; bar[17] = 0x00;             /* compressed, TID 0 */
        bar[18] = (uint8_t)(310 << 4); bar[19] = (uint8_t)(310 >> 4);
        rtw89_glue_test_rx(bar, sizeof(bar), 2437, -40, false);
    }
    EXPECT(sta_rx.count == n + 6 && ORDER(5) == (309 & 0xff));
    /* a frame far ahead moves the window: what was waiting goes on first */
    ap_send_agg(312, pn);
    ap_send_agg(400, pn);
    EXPECT(sta_rx.count == n + 7 && ORDER(6) == (312 & 0xff));
    usleep(300 * 1000);
    EXPECT(sta_rx.count == n + 8 && ORDER(7) == (400 & 0xff));
    /* the AP ends the session: frames are taken as they come again */
    {
        static const uint8_t delba[] = { 3, 2, 0x00, 0x08, 0x01, 0x00 };

        ap_send(0xd0, delba, sizeof(delba));
        for (i = 0; i < 100 && link_ba(false); i++)
            usleep(20 * 1000);
        EXPECT(link_ba(false) == 0);
    }
    ap_send_agg(403, pn);
    ap_send_agg(405, pn);
    EXPECT(sta_rx.count == n + 10);

    /* ---- we send aggregates: the driver asks once it has sent a frame ---- */
    n = ap.addba_req[3].count;
    EXPECT(sta_tx_tid(3) == 0);
    EXPECT(wait_count(&ap.addba_req[3].count, n));
    /* immediate BlockAck, TID 3, 64 frames, starting after the frame already sent */
    EXPECT((ap.addba_req[3].capab & 0xfffe) == 0x100e && ap.addba_req[3].ssn == 1);
    /* until the AP answers, the TID's frames are held back */
    EXPECT(sta_tx_tid(3) == 0 && sta_tx_tid(3) == 0);
    usleep(100 * 1000);
    EXPECT(rtw89_data_tx_waiting(3) == 2 && rtw89_glue_tx_room() == 254);
    EXPECT(!(link_ba(true) & 0x08));
    /* a stack that keeps sending is told to stop when 256 frames are waiting */
    for (i = 0; i < 400 && rtw89_glue_tx_room(); i++)
        EXPECT(sta_tx_tid(3) == 0);
    EXPECT(i == 254 && rtw89_data_tx_waiting(3) == 256);
    n = sta_tx_wakes;
    ap_send_addba_resp(3, ap.addba_req[3].dialog, 0);
    for (i = 0; i < 100 && (!(link_ba(true) & 0x08) || rtw89_data_tx_waiting(3)); i++)
        usleep(20 * 1000);
    EXPECT((link_ba(true) & 0x08) && rtw89_data_tx_waiting(3) == 0);
    /* ... and once, when most of them have gone, to start again */
    EXPECT(sta_tx_wakes == n + 1 && rtw89_glue_tx_room() == 256);
    /* the AP ends it */
    {
        static const uint8_t delba[] = { 3, 2, 0x00, 0x30, 0x01, 0x00 };

        ap_send(0xd0, delba, sizeof(delba));
        for (i = 0; i < 100 && (link_ba(true) & 0x08); i++)
            usleep(20 * 1000);
        EXPECT(!(link_ba(true) & 0x08));
    }

    /* an AP that says no: nothing is held back afterwards */
    n = ap.addba_req[6].count;
    EXPECT(sta_tx_tid(6) == 0);
    EXPECT(wait_count(&ap.addba_req[6].count, n));
    EXPECT(sta_tx_tid(6) == 0);
    ap_send_addba_resp(6, ap.addba_req[6].dialog, 37);
    for (i = 0; i < 100 && rtw89_data_tx_waiting(6); i++)
        usleep(20 * 1000);
    EXPECT(rtw89_data_tx_waiting(6) == 0 && !(link_ba(true) & 0x40));

    /* an AP that does not answer (TID 5, from the data path test): after a
     * second the station gives up and says so */
    EXPECT(ap.addba_req[5].count >= 1);
    EXPECT(wait_count(&ap.delba[5].count, 0) && ap.delba[5].initiator);
    EXPECT(rtw89_data_tx_waiting(5) == 0);
#undef ORDER
#undef EXPECT
    printf("== aggregation test: %d failure(s)\n", failures);
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

/* An element of the last association request the station sent. */
static const uint8_t *sta_assoc_ie(uint8_t eid)
{
    size_t off = 24 + 4;        /* header, capabilities, listen interval */

    while (off + 2 <= ap.assoc_req_len && ap.assoc_req[off] != eid)
        off += 2 + ap.assoc_req[off + 1];
    return off + 2 <= ap.assoc_req_len ? ap.assoc_req + off : NULL;
}

/* An extension element of the last association request, from its extension id. */
static const uint8_t *sta_assoc_ext_ie(uint8_t ext_eid)
{
    size_t off = 24 + 4;

    while (off + 3 <= ap.assoc_req_len && !(ap.assoc_req[off] == 255 && ap.assoc_req[off + 2] == ext_eid))
        off += 2 + ap.assoc_req[off + 1];
    return off + 3 <= ap.assoc_req_len ? ap.assoc_req + off : NULL;
}

/* Authenticate and associate with the pretend AP answering. */
static int join_testnet(const char *password, uint16_t assoc_status)
{
    if (rtw89_glue_join((const uint8_t *)ap_ssid, strlen(ap_ssid), password, strlen(password)))
        return 0;
    ap_send_auth(0);
    /* the association request goes out once the auth answer is processed */
    usleep(300 * 1000);
    ap_send_assoc_resp(assoc_status);
    return 1;
}

/* The station is trying to get back: let its scan end if it started one, then
 * play the AP's side of the join. Returns 1 once it is connected again. */
static int ap_answer_rejoin(const uint8_t *gtk, int gtk_id)
{
    int base = ap.auth_count, i;

    for (i = 0; i < 300 && ap.auth_count == base; i++) {
        if (rtw89_glue_scanning()) {
            ap_send_beacon();
            rtw89_glue_test_scan_done();
        }
        usleep(50 * 1000);
    }
    if (ap.auth_count == base)
        return 0;
    ap_send_auth(0);
    usleep(300 * 1000);
    ap_send_assoc_resp(0);
    if (!wait_link(RTW89_GLUE_LINK_ASSOCIATED) || ap_handshake(gtk, gtk_id))
        return 0;
    return wait_link(RTW89_GLUE_LINK_CONNECTED);
}

/* Staying connected: beacons, a silent AP, an AP that sends us off or moves. */
static int test_keep(void)
{
    static const uint8_t gtk1[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    static const uint8_t csa[] = { 37, 3, 1, 11, 2 };   /* to channel 11 in two beacons */
    struct rtw89_glue_link link;
    int failures = 0, n, i;
    uint32_t rejoins;

#define EXPECT(cond) do { if (!(cond)) { failures++; printf("== FAIL %s\n", #cond); } } while (0)
    /* counters run for the driver's lifetime: the test runs more than once */
    rtw89_glue_link(&link);
    rejoins = link.rejoins;
    /* the pretend chip's scan from the start of the test never ended */
    rtw89_glue_test_scan_done();
    ap_send_beacon();
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    EXPECT(ap_handshake(gtk1, 1) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));

    /* ---- beacons ---- */
    {
        uint32_t seen, updates;

        ap_send_beacon();
        usleep(200 * 1000);
        rtw89_glue_link(&link);
        seen = link.beacons;
        updates = link.beacon_updates;
        EXPECT(seen >= 1);
        /* the same again changes nothing */
        ap_send_beacon();
        usleep(200 * 1000);
        rtw89_glue_link(&link);
        EXPECT(link.beacons == seen + 1 && link.beacon_updates == updates);
        /* heard while tuned to another channel: not counted */
        ap_freq = 2462;
        ap_send_beacon();
        ap_freq = 2437;
        usleep(200 * 1000);
        rtw89_glue_link(&link);
        EXPECT(link.beacons == seen + 1);
        ap_send_beacon();
        /* a new version of the EDCA parameters is taken in, once */
        ap_ies[AP_WMM_COUNT] = 0x02;
        ap_send_beacon();
        ap_send_beacon();
        usleep(200 * 1000);
        rtw89_glue_link(&link);
        EXPECT(link.beacons == seen + 4 && link.beacon_updates == updates + 1);
        EXPECT(link_state() == RTW89_GLUE_LINK_CONNECTED);
    }

    /* the rate of the last frame from the AP: this one came at 1 Mb/s */
    ap_send_agg(20, ap.tx_pn + 5000);
    rtw89_glue_link(&link);
    EXPECT(link.rx_rate.kbps == 1000 && link.rx_rate.mode == 0);

    /* ---- the driver misses beacons: the AP is probed with a null frame ---- */
    n = ap.null_count;
    rtw89_glue_test_beacon_loss();
    EXPECT(wait_count(&ap.null_count, n));
    /* a beacon is as good as an answer */
    ap_send_beacon();
    usleep(900 * 1000);
    rtw89_glue_link(&link);
    EXPECT(link.state == RTW89_GLUE_LINK_CONNECTED && link.beacon_losses == 1);
    /* no beacon and no acknowledgement (the pretend chip reports none): the
     * connection is given up after half a second; with rejoin off that is all */
    n = ap.auth_count;
    rtw89_glue_test_beacon_loss();
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    rtw89_glue_link(&link);
    EXPECT(link.last_error == -67 && !link.rejoining && !link.rejoin);
    usleep(1500 * 1000);
    EXPECT(link_state() == RTW89_GLUE_LINK_DOWN && ap.auth_count == n);

    /* ---- coming back ---- */
    rtw89_glue_set_rejoin(true);
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    EXPECT(ap_handshake(gtk1, 1) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));

    /* the AP sends us off: the station asks to be let in again */
    ap_send_deauth(7);
    EXPECT(ap_answer_rejoin(gtk1, 1));
    rtw89_glue_link(&link);
    EXPECT(link.rejoins == rejoins + 1 && !link.rejoining && link.rejoin && !link.last_error);
    EXPECT(sta_tx_test() == 0);

    /* the AP goes silent, and is there again a little later */
    rtw89_glue_test_beacon_loss();
    for (i = 0; i < 100 && !link.rejoining; i++) {
        usleep(50 * 1000);
        rtw89_glue_link(&link);
    }
    EXPECT(link.rejoining && link.state == RTW89_GLUE_LINK_DOWN);
    EXPECT(ap_answer_rejoin(gtk1, 1));
    rtw89_glue_link(&link);
    EXPECT(link.rejoins == rejoins + 2 && link.freq == 2437);

    /* the AP announces a move to channel 11, and moves */
    ap_beacon_extra = csa;
    ap_beacon_extra_len = sizeof(csa);
    ap_send_beacon();
    ap_beacon_extra = NULL;
    ap_freq = 2462;
    ap_ies[AP_DS_CHANNEL] = 11;
    ap_ies[AP_HT_CHANNEL] = 11;
    usleep(200 * 1000);
    EXPECT(link_state() == RTW89_GLUE_LINK_CONNECTED);     /* not before the move is due */
    for (i = 0; i < 100 && !link.rejoining; i++) {
        usleep(50 * 1000);
        rtw89_glue_link(&link);
    }
    EXPECT(link.rejoining && link.last_error == -102);
    ap_send_beacon();
    EXPECT(ap_answer_rejoin(gtk1, 1));
    rtw89_glue_link(&link);
    EXPECT(link.rejoins == rejoins + 3 && link.freq == 2462);
    EXPECT(sta_tx_test() == 0);

    /* an attempt that is refused is followed by another */
    ap_send_deauth(7);
    n = ap.auth_count;
    for (i = 0; i < 300 && ap.auth_count == n; i++) {
        if (rtw89_glue_scanning()) {
            ap_send_beacon();
            rtw89_glue_test_scan_done();
        }
        usleep(50 * 1000);
    }
    EXPECT(ap.auth_count > n);
    ap_send_auth(0);
    usleep(300 * 1000);
    ap_send_assoc_resp(17);
    EXPECT(ap_answer_rejoin(gtk1, 1));
    rtw89_glue_link(&link);
    EXPECT(link.rejoins == rejoins + 4);

    /* leaving is for good */
    n = ap.auth_count;
    rtw89_glue_leave();
    usleep(1500 * 1000);
    rtw89_glue_link(&link);
    EXPECT(link.state == RTW89_GLUE_LINK_DOWN && !link.rejoining && ap.auth_count == n);

    /* ---- 802.1X sign-in: the handshake is an outside supplicant's ---- */
    {
        static const uint8_t gtk[16] = { 9, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6 };
        static const uint8_t ptk[16] = { 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4 };
        const uint8_t *rsn;
        int rx;

        ap_ies[AP_RSN_AKM] = 1;     /* the AP offers 802.1X instead of a shared password */
        ap_send_beacon();
        /* a shared-password join is turned down now, an 802.1X one starts */
        EXPECT(rtw89_glue_join((const uint8_t *)"testnet", 7, AP_PASSWORD, strlen(AP_PASSWORD)) == -95);
        EXPECT(rtw89_glue_join_ext((const uint8_t *)"testnet", 7, NULL, NULL, 0) == 0);
        ap_send_auth(0);
        usleep(300 * 1000);
        ap_send_assoc_resp(0);
        /* connected as far as the link goes, but nothing but EAPOL passes */
        EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
        rtw89_glue_link(&link);
        EXPECT(!link.authorized);
        rsn = sta_assoc_ie(48);
        EXPECT(rsn && rsn[1] == 20 && rsn[2 + 17] == 1);
        EXPECT(sta_tx_test() == -100);
        /* the AP's sign-in frames go to the stack, not to the driver's supplicant */
        rx = sta_rx.count;
        ap_send_eap();
        usleep(200 * 1000);
        EXPECT(sta_rx.count == rx + 1 && sta_rx.frame[12] == 0x88 && sta_rx.frame[13] == 0x8e);
        /* keys from outside: with both in place data flows */
        EXPECT(rtw89_glue_set_key(true, 0, ptk, 16, 0) == 0);
        rtw89_glue_link(&link);
        EXPECT(!link.authorized);
        EXPECT(rtw89_glue_set_key(false, 1, gtk, 16, 0) == 0);
        rtw89_glue_link(&link);
        EXPECT(link.authorized && link.state == RTW89_GLUE_LINK_CONNECTED);
        EXPECT(sta_tx_test() == 0);
        EXPECT(rtw89_glue_set_key(true, 0, ptk, 5, 0) != 0);
        rtw89_glue_leave();
        EXPECT(link_state() == RTW89_GLUE_LINK_DOWN);
        EXPECT(rtw89_glue_set_key(true, 0, ptk, 16, 0) != 0);

        /* the other way: the sign-in is the system's and hands over its
         * master key, the key handshake is the driver's */
        memset(ap.pmk, 0x3c, sizeof(ap.pmk));
        EXPECT(rtw89_glue_set_pmk(ap.pmk, 32) != 0);
        EXPECT(rtw89_glue_join_ext((const uint8_t *)"testnet", 7, NULL, NULL, 0) == 0);
        ap_send_auth(0);
        usleep(300 * 1000);
        ap_send_assoc_resp(0);
        EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
        rx = sta_rx.count;
        ap_send_eap();
        usleep(200 * 1000);
        EXPECT(sta_rx.count == rx + 1);
        /* the AP's message 1 is early: not passed up, not answered yet */
        n = ap.eapol_count;
        ap.replay = 1;
        memset(ap.anonce, 0x77, sizeof(ap.anonce));
        ap_send_key(0x008a, NULL, 0, false);
        usleep(200 * 1000);
        EXPECT(sta_rx.count == rx + 1 && ap.eapol_count == n);
        EXPECT(rtw89_glue_set_pmk(ap.pmk, 31) != 0);
        /* with the master key it is answered, and the handshake goes through */
        EXPECT(rtw89_glue_set_pmk(ap.pmk, 32) == 0);
        EXPECT(ap_wait_eapol(n + 1));
        EXPECT(ap_handshake_pmk(gtk, 1) == 0);
        usleep(200 * 1000);
        rtw89_glue_link(&link);
        EXPECT(link.authorized && link.state == RTW89_GLUE_LINK_CONNECTED);
        EXPECT(sta_tx_test() == 0);
        EXPECT(sta_rx.count == rx + 1);
        /* a wrong master key gets nowhere */
        rtw89_glue_leave();
        EXPECT(link_state() == RTW89_GLUE_LINK_DOWN);
        EXPECT(rtw89_glue_join_ext((const uint8_t *)"testnet", 7, NULL, NULL, 0) == 0);
        ap_send_auth(0);
        usleep(300 * 1000);
        ap_send_assoc_resp(0);
        EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
        EXPECT(rtw89_glue_set_pmk(gtk, 16) != 0);
        memset(ap.pmk, 0x3d, sizeof(ap.pmk));
        EXPECT(rtw89_glue_set_pmk(ap.pmk, 32) == 0);
        memset(ap.pmk, 0x3c, sizeof(ap.pmk));
        EXPECT(ap_handshake_pmk(gtk, 1) == 1);
        rtw89_glue_link(&link);
        EXPECT(!link.authorized);
        rtw89_glue_leave();
        EXPECT(link_state() == RTW89_GLUE_LINK_DOWN);
        ap_ies[AP_RSN_AKM] = 2;
        ap_send_beacon();
    }

    /* a join that never worked is not repeated */
    EXPECT(join_testnet("not the password", 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    EXPECT(ap_handshake(gtk1, 1) == 1);
    n = ap.auth_count;
    ap_send_deauth(15);
    EXPECT(wait_link(RTW89_GLUE_LINK_DOWN));
    usleep(1500 * 1000);
    rtw89_glue_link(&link);
    EXPECT(link.state == RTW89_GLUE_LINK_DOWN && !link.rejoining && ap.auth_count == n);

    ap_freq = 2437;
    ap_ies[AP_DS_CHANNEL] = 6;
    ap_ies[AP_HT_CHANNEL] = 6;
    ap_ies[AP_WMM_COUNT] = 0x00;
    ap_send_beacon();
#undef EXPECT
    printf("== keep test: %d failure(s)\n", failures);
    return failures;
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
    /* each of these looks at what one join does; coming back by itself has
     * its own test at the end */
    rtw89_glue_set_rejoin(false);
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
    /* an 802.11n network on 20 MHz: no 40 MHz claimed, no 802.11ac element */
    EXPECT(link.width == 20 && link.center_freq == 2437 && link.mode == 1 && link.nss == 2);
    EXPECT(sta_assoc_ie(45) && !(sta_assoc_ie(45)[2] & 0x02) && !sta_assoc_ie(191));
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
    failures += test_aggregation();

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
    rtw89_glue_leave();

    /* ---- an 802.11ac network on an 80 MHz channel ---- */
    ap_select(true);
    ap_send_beacon();
    {
        struct rtw89_glue_bss list[8];
        unsigned int i, n = rtw89_glue_scan_results(list, 8), found = 0;

        for (i = 0; i < n; i++) {
            if (!strcmp(list[i].ssid, "testnet5")) {
                found++;
                EXPECT(list[i].mode == 2 && list[i].width == 80 &&
                       list[i].security == RTW89_GLUE_SEC_WPA2_PSK);
            } else if (!strcmp(list[i].ssid, "testnet")) {
                found++;
                EXPECT(list[i].mode == 1 && list[i].width == 20 &&
                       list[i].security == RTW89_GLUE_SEC_WPA2_PSK);
            }
        }
        EXPECT(found == 2);
    }
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(link.freq == 5180 && link.width == 80 && link.center_freq == 5210);
    EXPECT(link.mode == 2 && link.nss == 2);
    /* the request says so: 40 MHz in the HT element, and a VHT element */
    EXPECT(sta_assoc_ie(45) && (sta_assoc_ie(45)[2] & 0x02));
    EXPECT(sta_assoc_ie(191) && sta_assoc_ie(191)[1] == 12);
    EXPECT(!sta_assoc_ext_ie(35));
    EXPECT(ap_handshake(gtk1, 1) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
    EXPECT(sta_tx_test() == 0);
    rtw89_glue_leave();
    EXPECT(link_state() == RTW89_GLUE_LINK_DOWN);

    /* the same AP on 160 MHz (channels 36-64, centre 50): this card stays in
     * the 80 MHz half that has the control channel */
    ap5_ies[AP5_VHT_OPER + 2] = 2;
    ap5_ies[AP5_VHT_OPER + 3] = 50;
    ap_send_beacon();
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(link.width == 80 && link.center_freq == 5210 && link.mode == 2);
    rtw89_glue_leave();

    /* VHT operation that contradicts the HT element (an 80 MHz channel the
     * 40 MHz one is not part of): 802.11n rules, 40 MHz */
    ap5_ies[AP5_VHT_OPER + 2] = 1;
    ap5_ies[AP5_VHT_OPER + 3] = 58;
    ap_send_beacon();
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(link.width == 40 && link.center_freq == 5190 && link.mode == 1);
    EXPECT(!sta_assoc_ie(191));
    rtw89_glue_leave();
    ap5_ies[AP5_VHT_OPER + 3] = 42;

    /* ---- the same network with 802.11ax ---- */
    ap_he = true;
    ap_he_resp = true;
    ap_send_beacon();
    {
        struct rtw89_glue_bss list[8];
        unsigned int i, n = rtw89_glue_scan_results(list, 8), found = 0;

        for (i = 0; i < n; i++)
            if (!strcmp(list[i].ssid, "testnet5")) {
                found++;
                EXPECT(list[i].mode == 3 && list[i].width == 80);
            }
        EXPECT(found == 1);
    }
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(link.width == 80 && link.center_freq == 5210 && link.mode == 3 && link.nss == 2);
    {
        /* our HE capabilities: no PPE thresholds, rates for 80 MHz only, and
         * of the widths 40/80 MHz on 5 GHz alone */
        const uint8_t *he = sta_assoc_ext_ie(35);

        EXPECT(he && he[1] == 22 && he[3 + 6] == 0x04);
        EXPECT(he && he[3 + 17] == 0xfa && he[3 + 18] == 0xff);
        EXPECT(sta_assoc_ie(191) != NULL);
    }
    EXPECT(ap_handshake(gtk1, 1) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
    EXPECT(sta_tx_test() == 0);
    {
        /* The AP wants to send aggregates of 256 frames on TID 1 (start 1),
         * with the extension element. The card takes 64 and answers with
         * the element too. */
        static const uint8_t addba[] = { 3, 0, 11, 0x07, 0x40, 0x00, 0x00, 0x10, 0x00, 159, 1, 0x01 };
        int n = ap.addba_resp[1].count;

        ap_send(0xd0, addba, sizeof(addba));
        EXPECT(wait_count(&ap.addba_resp[1].count, n));
        EXPECT(ap.addba_resp[1].dialog == 11 && ap.addba_resp[1].status == 0);
        EXPECT(ap.addba_resp[1].capab == 0x1007 && ap.addba_resp[1].ext == 0x01);

        /* and our own request: 128 frames (what the chip sends), with the element */
        n = ap.addba_req[2].count;
        EXPECT(sta_tx_tid(2) == 0);
        EXPECT(wait_count(&ap.addba_req[2].count, n));
        EXPECT((ap.addba_req[2].capab & 0xffc0) == 128 << 6 && ap.addba_req[2].ext == 0x01);
    }
    rtw89_glue_leave();
    EXPECT(link_state() == RTW89_GLUE_LINK_DOWN);

    /* an AP whose association response leaves the HE elements out: 802.11ac */
    ap_he_resp = false;
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(sta_assoc_ext_ie(35) != NULL);
    EXPECT(link.width == 80 && link.mode == 2 && link.nss == 2);
    rtw89_glue_leave();
    ap_he_resp = true;

    /* 802.11ax switched off (rtw89ctl ax off): the same AP as 802.11ac */
    rtw89_glue_set_ax(false);
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(!sta_assoc_ext_ie(35) && sta_assoc_ie(191));
    EXPECT(link.width == 80 && link.mode == 2 && !link.ax);
    rtw89_glue_leave();
    rtw89_glue_set_ax(true);
    rtw89_glue_link(&link);
    EXPECT(link.ax);

    /* an AP that requires MCS 0-7 on three streams of every station: not
     * with this card, so it is joined as 802.11ac and HE is not offered */
    ap_he_ies[AP_HE_OPER + 7] = 0xc0;
    ap_send_beacon();
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(!sta_assoc_ext_ie(35) && sta_assoc_ie(191));
    EXPECT(link.width == 80 && link.mode == 2);
    rtw89_glue_leave();
    ap_he_ies[AP_HE_OPER + 7] = 0xfc;

    /* 802.11ax on 2.4 GHz: 20 MHz here, and no 802.11ac involved */
    ap_select(false);
    ap_send_beacon();
    EXPECT(join_testnet(AP_PASSWORD, 0));
    EXPECT(wait_link(RTW89_GLUE_LINK_ASSOCIATED));
    rtw89_glue_link(&link);
    EXPECT(link.width == 20 && link.center_freq == 2437 && link.mode == 3 && link.nss == 2);
    /* the widths left in our capabilities: 40 MHz on 2.4 GHz, as the AP's
     * channel being narrower does not limit what we can do */
    EXPECT(sta_assoc_ext_ie(35) && sta_assoc_ext_ie(35)[3 + 6] == 0x02 && !sta_assoc_ie(191));
    EXPECT(ap_handshake(gtk1, 1) == 0);
    EXPECT(wait_link(RTW89_GLUE_LINK_CONNECTED));
    EXPECT(sta_tx_test() == 0);
    rtw89_glue_leave();
    ap_he = false;
    ap_send_beacon();
    ap_select(true);
    ap_send_beacon();

    ap_select(false);
    failures += test_keep();
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
    /* privacy bit without an RSN element: WEP or WPA1, 802.11b/g */
    EXPECT(bss[0].mode == 0 && bss[0].width == 20 && bss[0].security == RTW89_GLUE_SEC_WEP_WPA1);

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

        /* Sleep and wake: down while it was in use, then up again on the
         * same probed chip, and a join on top. */
        ret = rtw89_glue_up();
        printf("== radio up after down (wake) returned %d\n", ret);
        EXPECT(ret == 0 && rtw89_glue_is_up());
        if (!ret) {
            failures += test_join();
            rtw89_glue_down();
            EXPECT(!rtw89_glue_is_up() && !rtw89_glue_scanning());
            printf("== radio down again (sleep)\n");
        }
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

/*
 * The one thing the pretend device does on its own (fakechip runs only): it
 * "consumes" what the driver puts on its transmit rings. The driver writes its
 * index into the low half of each ring's index register and expects the
 * hardware's read index in the high half; without this the firmware command
 * ring fills up after a few joins and every later command fails for lack of
 * room. Registers R_AX_ACH0_TXBD_IDX .. R_AX_CH12_TXBD_IDX.
 */
#define FAKE_TXBD_IDX_FIRST 0x1058
#define FAKE_TXBD_IDX_LAST  0x1080

static volatile int fake_device_run;
static uint8_t *fake_device_mmio;

static void *fake_device_thread(void *arg)
{
    while (fake_device_run) {
        unsigned int reg;
        uint16_t host;

        for (reg = FAKE_TXBD_IDX_FIRST; reg <= FAKE_TXBD_IDX_LAST; reg += 4) {
            memcpy(&host, fake_device_mmio + reg, 2);
            host &= 0x0fff;
            memcpy(fake_device_mmio + reg + 2, &host, 2);
        }
        usleep(2000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    struct rtw89_glue_platform plat = {
        .cfg_read = cfg_read, .cfg_write = cfg_write,
        .dma_alloc = dma_alloc, .dma_free = dma_free,
        .rx_frame = sta_rx_frame,
        .tx_wake = sta_tx_wake,
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
    pthread_t fake_device;
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

    if (expect_ok) {
        fake_device_mmio = mmio;
        fake_device_run = 1;
        pthread_create(&fake_device, NULL, fake_device_thread, NULL);
    }

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

    if (expect_ok) {
        fake_device_run = 0;
        pthread_join(fake_device, NULL);
    }

    /* Let detached workqueue threads finish exiting before the process does. */
    usleep(200 * 1000);
    printf("== done\n");
    if (failed)
        return 4;
    return dma_live ? 2 : 0;
}
