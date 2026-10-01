// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// RTW88IEEE80211.cpp — 802.11 state machine

#include "RTW88IEEE80211.hpp"
#include "RTW88AWDLProtocol.hpp"
#include "RTW88MgmtValidation.hpp"
#include "RTW88PCIDevice.hpp"
#include "RTW88UserClient.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <sys/mbuf.h>
#include <string.h>
#include <sys/random.h>

/* Debug stage checkpoint — logs message only (no sleep). */
#define RTW88_STAGE(fmt, ...) IOLog("rtw88: ---- STAGE: " fmt " ----\n", ##__VA_ARGS__)

/* Chain-safe packet mbuf builder (defined below). */
static mbuf_t rtw88_make_packet_mbuf(const void *src, uint32_t len);

extern "C" {
#include "../compat/rtw88_compat.h"

/* Linux driver public API */
int  rtw_core_init(struct rtw_dev *rtwdev);
void rtw_core_deinit(struct rtw_dev *rtwdev);
int  rtw_core_start(struct rtw_dev *rtwdev);
void rtw_core_stop(struct rtw_dev *rtwdev);
void rtw_tx(struct rtw_dev *rtwdev, struct ieee80211_tx_control *control,
            struct sk_buff *skb);

/* PCI probe shim declared in pci.c */
int  rtw_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id);
void rtw_pci_remove(struct pci_dev *pdev);

/* Exported from the compat layer for hooking */
void rtw88_set_hw_callbacks(struct rtw88_hw_callbacks *cbs, void *kext_hw);

/* chip hw_spec structs — driver_data for rtw_pci_probe */
extern const struct rtw_chip_info rtw8822b_hw_spec;
extern const struct rtw_chip_info rtw8822c_hw_spec;
extern const struct rtw_chip_info rtw8821c_hw_spec;

} /* extern "C" */

/* ------------------------------------------------------------------ */
/*  WPA2 cryptographic functions (SHA1, HMAC-SHA1, PBKDF2, PRF)      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t buffer[64];
} KERN_SHA1_CTX;

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

#define blk0(i) (block->l[i] = (rol(block->l[i], 24) & 0xFF00FF00) | (rol(block->l[i], 8) & 0x00FF00FF))
#define blk(i) (block->l[i & 15] = rol(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ block->l[(i + 2) & 15] ^ block->l[i & 15], 1))

#define r0(v,w,x,y,z,i) z += ((w & (x ^ y)) ^ y) + blk0(i) + 0x5A827999 + rol(v, 5); w = rol(w, 30);
#define r1(v,w,x,y,z,i) z += ((w & (x ^ y)) ^ y) + blk(i) + 0x5A827999 + rol(v, 5); w = rol(w, 30);
#define r2(v,w,x,y,z,i) z += (w ^ x ^ y) + blk(i) + 0x6ED9EBA1 + rol(v, 5); w = rol(w, 30);
#define r3(v,w,x,y,z,i) z += (((w | x) & y) | (w & x)) + blk(i) + 0x8F1BBCDC + rol(v, 5); w = rol(w, 30);
#define r4(v,w,x,y,z,i) z += (w ^ x ^ y) + blk(i) + 0xCA62C1D6 + rol(v, 5); w = rol(w, 30);

static void kern_sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e;
    typedef union {
        uint8_t c[64];
        uint32_t l[16];
    } CHAR64LONG16;
    CHAR64LONG16 block[1];
    memcpy(block, buffer, 64);
    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    r0(a,b,c,d,e,0);  r0(e,a,b,c,d,1);  r0(d,e,a,b,c,2);  r0(c,d,e,a,b,3);
    r0(b,c,d,e,a,4);  r0(a,b,c,d,e,5);  r0(e,a,b,c,d,6);  r0(d,e,a,b,c,7);
    r0(c,d,e,a,b,8);  r0(b,c,d,e,a,9);  r0(a,b,c,d,e,10); r0(e,a,b,c,d,11);
    r0(d,e,a,b,c,12); r0(c,d,e,a,b,13); r0(b,c,d,e,a,14); r0(a,b,c,d,e,15);
    r1(e,a,b,c,d,16); r1(d,e,a,b,c,17); r1(c,d,e,a,b,18); r1(b,c,d,e,a,19);
    r2(a,b,c,d,e,20); r2(e,a,b,c,d,21); r2(d,e,a,b,c,22); r2(c,d,e,a,b,23);
    r2(b,c,d,e,a,24); r2(a,b,c,d,e,25); r2(e,a,b,c,d,26); r2(d,e,a,b,c,27);
    r2(c,d,e,a,b,28); r2(b,c,d,e,a,29); r2(a,b,c,d,e,30); r2(e,a,b,c,d,31);
    r2(d,e,a,b,c,32); r2(c,d,e,a,b,33); r2(b,c,d,e,a,34); r2(a,b,c,d,e,35);
    r2(e,a,b,c,d,36); r2(d,e,a,b,c,37); r2(c,d,e,a,b,38); r2(b,c,d,e,a,39);
    r3(a,b,c,d,e,40); r3(e,a,b,c,d,41); r3(d,e,a,b,c,42); r3(c,d,e,a,b,43);
    r3(b,c,d,e,a,44); r3(a,b,c,d,e,45); r3(e,a,b,c,d,46); r3(d,e,a,b,c,47);
    r3(c,d,e,a,b,48); r3(b,c,d,e,a,49); r3(a,b,c,d,e,50); r3(e,a,b,c,d,51);
    r3(d,e,a,b,c,52); r3(c,d,e,a,b,53); r3(b,c,d,e,a,54); r3(a,b,c,d,e,55);
    r3(e,a,b,c,d,56); r3(d,e,a,b,c,57); r3(c,d,e,a,b,58); r3(b,c,d,e,a,59);
    r4(a,b,c,d,e,60); r4(e,a,b,c,d,61); r4(d,e,a,b,c,62); r4(c,d,e,a,b,63);
    r4(b,c,d,e,a,64); r4(a,b,c,d,e,65); r4(e,a,b,c,d,66); r4(d,e,a,b,c,67);
    r4(c,d,e,a,b,68); r4(b,c,d,e,a,69); r4(a,b,c,d,e,70); r4(e,a,b,c,d,71);
    r4(d,e,a,b,c,72); r4(c,d,e,a,b,73); r4(b,c,d,e,a,74); r4(a,b,c,d,e,75);
    r4(e,a,b,c,d,76); r4(d,e,a,b,c,77); r4(c,d,e,a,b,78); r4(b,c,d,e,a,79);
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void kern_sha1_init(KERN_SHA1_CTX *context) {
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}

static void kern_sha1_update(KERN_SHA1_CTX *context, const uint8_t *data, uint32_t len) {
    uint32_t i, j;
    j = context->count[0];
    if ((context->count[0] += len << 3) < j)
        context->count[1]++;
    context->count[1] += (len >> 29);
    j = (j >> 3) & 63;
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64 - j));
        kern_sha1_transform(context->state, context->buffer);
        for (; i + 63 < len; i += 64) {
            kern_sha1_transform(context->state, &data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }
    memcpy(&context->buffer[j], &data[i], len - i);
}

static void kern_sha1_final(uint8_t digest[20], KERN_SHA1_CTX *context) {
    unsigned char finalcount[8];
    for (int i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    unsigned char c = 0200;
    kern_sha1_update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
        c = 0;
        kern_sha1_update(context, &c, 1);
    }
    kern_sha1_update(context, finalcount, 8);
    for (int i = 0; i < 20; i++) {
        digest[i] = (uint8_t)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

static void kern_hmac_sha1(const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           uint8_t mac[20])
{
    KERN_SHA1_CTX ctx;
    uint8_t k_ipad[64] = {};
    uint8_t k_opad[64] = {};
    uint8_t tmp_key[20];

    if (key_len > 64) {
        kern_sha1_init(&ctx);
        kern_sha1_update(&ctx, key, (uint32_t)key_len);
        kern_sha1_final(tmp_key, &ctx);
        key = tmp_key;
        key_len = 20;
    }

    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);

    for (int i = 0; i < 64; i++) {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5c;
    }

    kern_sha1_init(&ctx);
    kern_sha1_update(&ctx, k_ipad, 64);
    kern_sha1_update(&ctx, data, (uint32_t)data_len);
    kern_sha1_final(mac, &ctx);

    kern_sha1_init(&ctx);
    kern_sha1_update(&ctx, k_opad, 64);
    kern_sha1_update(&ctx, mac, 20);
    kern_sha1_final(mac, &ctx);
}

static void derivePMK(const uint8_t *passphrase, const uint8_t *ssid, size_t ssid_len, uint8_t pmk[32])
{
    size_t pass_len = strlen((const char *)passphrase);
    uint8_t salt[128];
    if (ssid_len > 120) ssid_len = 120;
    memcpy(salt, ssid, ssid_len);

    for (int block = 1; block <= 2; block++) {
        salt[ssid_len]     = (uint8_t)((block >> 24) & 0xff);
        salt[ssid_len + 1] = (uint8_t)((block >> 16) & 0xff);
        salt[ssid_len + 2] = (uint8_t)((block >> 8) & 0xff);
        salt[ssid_len + 3] = (uint8_t)(block & 0xff);

        uint8_t u[20];
        uint8_t t[20];
        kern_hmac_sha1(passphrase, pass_len, salt, ssid_len + 4, u);
        memcpy(t, u, 20);

        for (int iter = 1; iter < 4096; iter++) {
            kern_hmac_sha1(passphrase, pass_len, u, 20, u);
            for (int i = 0; i < 20; i++) {
                t[i] ^= u[i];
            }
        }

        if (block == 1) {
            memcpy(pmk, t, 20);
        } else {
            memcpy(pmk + 20, t, 12);
        }
    }
}

static void derivePTK(const uint8_t pmk[32], const uint8_t anonce[32], const uint8_t snonce[32],
                      const uint8_t spa[6], const uint8_t aa[6], uint8_t ptk[64])
{
    uint8_t min_mac[6], max_mac[6];
    if (memcmp(spa, aa, 6) < 0) {
        memcpy(min_mac, spa, 6);
        memcpy(max_mac, aa, 6);
    } else {
        memcpy(min_mac, aa, 6);
        memcpy(max_mac, spa, 6);
    }

    uint8_t min_nonce[32], max_nonce[32];
    if (memcmp(snonce, anonce, 32) < 0) {
        memcpy(min_nonce, snonce, 32);
        memcpy(max_nonce, anonce, 32);
    } else {
        memcpy(min_nonce, anonce, 32);
        memcpy(max_nonce, snonce, 32);
    }

    uint8_t data[100];
    const char *label = "Pairwise key expansion";
    memcpy(data, label, 22);
    data[22] = 0;
    memcpy(data + 23, min_mac, 6);
    memcpy(data + 29, max_mac, 6);
    memcpy(data + 35, min_nonce, 32);
    memcpy(data + 67, max_nonce, 32);
    size_t data_len = 23 + 6 + 6 + 32 + 32;

    uint8_t hash[20];
    for (int i = 0; i < 4; i++) {
        data[data_len] = (uint8_t)i;
        kern_hmac_sha1(pmk, 32, data, data_len + 1, hash);
        if (i < 3) {
            memcpy(ptk + i * 20, hash, 20);
        } else {
            memcpy(ptk + i * 20, hash, 4);
        }
    }
}

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static uint8_t aes_xtime(uint8_t x)
{
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static uint8_t aes_mul(uint8_t x, uint8_t y)
{
    uint8_t r = 0;
    while (y) {
        if (y & 1) r ^= x;
        x = aes_xtime(x);
        y >>= 1;
    }
    return r;
}

static void aes128_key_expand(const uint8_t key[16], uint8_t round_key[176])
{
    static const uint8_t rcon[10] = {0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    memcpy(round_key, key, 16);
    for (int bytes = 16, r = 0; bytes < 176; bytes += 4) {
        uint8_t t[4];
        memcpy(t, round_key + bytes - 4, 4);
        if ((bytes & 15) == 0) {
            uint8_t tmp = t[0];
            t[0] = aes_sbox[t[1]] ^ rcon[r++];
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[tmp];
        }
        for (int i = 0; i < 4; i++)
            round_key[bytes + i] = round_key[bytes - 16 + i] ^ t[i];
    }
}

static void aes_add_round_key(uint8_t state[16], const uint8_t *rk)
{
    for (int i = 0; i < 16; i++) state[i] ^= rk[i];
}

static void aes_inv_shift_rows(uint8_t s[16])
{
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static void aes_inv_sub_bytes(uint8_t s[16])
{
    for (int i = 0; i < 16; i++) s[i] = aes_rsbox[s[i]];
}

static void aes_inv_mix_columns(uint8_t s[16])
{
    for (int c = 0; c < 4; c++) {
        uint8_t *p = s + c * 4;
        uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = aes_mul(a0, 0x0e) ^ aes_mul(a1, 0x0b) ^ aes_mul(a2, 0x0d) ^ aes_mul(a3, 0x09);
        p[1] = aes_mul(a0, 0x09) ^ aes_mul(a1, 0x0e) ^ aes_mul(a2, 0x0b) ^ aes_mul(a3, 0x0d);
        p[2] = aes_mul(a0, 0x0d) ^ aes_mul(a1, 0x09) ^ aes_mul(a2, 0x0e) ^ aes_mul(a3, 0x0b);
        p[3] = aes_mul(a0, 0x0b) ^ aes_mul(a1, 0x0d) ^ aes_mul(a2, 0x09) ^ aes_mul(a3, 0x0e);
    }
}

static void aes128_decrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176];
    uint8_t s[16];
    aes128_key_expand(key, rk);
    memcpy(s, in, 16);
    aes_add_round_key(s, rk + 160);
    for (int round = 9; round >= 1; round--) {
        aes_inv_shift_rows(s);
        aes_inv_sub_bytes(s);
        aes_add_round_key(s, rk + round * 16);
        aes_inv_mix_columns(s);
    }
    aes_inv_shift_rows(s);
    aes_inv_sub_bytes(s);
    aes_add_round_key(s, rk);
    memcpy(out, s, 16);
}

static bool aes_unwrap_128(const uint8_t kek[16], const uint8_t *in,
                           uint16_t in_len, uint8_t *out, uint16_t *out_len)
{
    if (in_len < 16 || (in_len & 7))
        return false;

    uint8_t a[8];
    uint8_t r[32][8];
    int n = in_len / 8 - 1;
    if (n <= 0 || n > 32)
        return false;

    memcpy(a, in, 8);
    for (int i = 0; i < n; i++)
        memcpy(r[i], in + 8 + i * 8, 8);

    for (int j = 5; j >= 0; j--) {
        for (int i = n - 1; i >= 0; i--) {
            uint8_t block[16], plain[16];
            uint64_t t = (uint64_t)(n * j + i + 1);
            memcpy(block, a, 8);
            for (int k = 7; k >= 0 && t; k--, t >>= 8)
                block[k] ^= (uint8_t)(t & 0xff);
            memcpy(block + 8, r[i], 8);
            aes128_decrypt_block(kek, block, plain);
            memcpy(a, plain, 8);
            memcpy(r[i], plain + 8, 8);
        }
    }

    static const uint8_t iv[8] = {0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6,0xa6};
    if (memcmp(a, iv, 8) != 0)
        return false;

    for (int i = 0; i < n; i++)
        memcpy(out + i * 8, r[i], 8);
    *out_len = (uint16_t)(n * 8);
    return true;
}

static bool eapol_mic_ok(const uint8_t kck[16], const uint8_t *eapol,
                         uint32_t eapol_len)
{
    if (eapol_len < 99)
        return false;

    uint8_t *tmp = (uint8_t *)IOMalloc(eapol_len);
    if (!tmp)
        return false;

    memcpy(tmp, eapol, eapol_len);
    uint8_t rx_mic[16];
    memcpy(rx_mic, tmp + 81, sizeof(rx_mic));
    memset(tmp + 81, 0, sizeof(rx_mic));

    uint8_t mic[20];
    kern_hmac_sha1(kck, 16, tmp, eapol_len, mic);
    IOFree(tmp, eapol_len);
    return memcmp(rx_mic, mic, sizeof(rx_mic)) == 0;
}

static bool extract_gtk_from_kde(const uint8_t *key_data, uint16_t key_data_len,
                                 uint8_t gtk[32], uint8_t *gtk_len,
                                 uint8_t *gtk_idx)
{
    const uint8_t rsn_gtk_oui[4] = {0x00, 0x0f, 0xac, 0x01};

    for (uint16_t pos = 0; pos + 2 <= key_data_len; ) {
        uint8_t id = key_data[pos];
        uint8_t len = key_data[pos + 1];
        const uint8_t *body = key_data + pos + 2;
        if (pos + 2 + len > key_data_len)
            break;

        if (id == 0xdd && len >= 6 && memcmp(body, rsn_gtk_oui, 4) == 0) {
            uint8_t key_len = (uint8_t)(len - 6);
            if (key_len > 32)
                return false;
            *gtk_idx = body[4] & 0x3;
            *gtk_len = key_len;
            memcpy(gtk, body + 6, key_len);
            return true;
        }
        pos += 2 + len;
    }

    return false;
}

#undef rol
#undef blk0
#undef blk
#undef r0
#undef r1
#undef r2
#undef r3
#undef r4

/* PCI device-ID → chip_info lookup (PCIe chips only) */
struct rtw88_pci_id_entry {
    uint16_t device;
    const struct rtw_chip_info *chip;
};

static const struct rtw88_pci_id_entry rtw88_pci_chip_table[] = {
    { 0xB822, &rtw8822b_hw_spec },  /* RTL8822BE */
    { 0xC822, &rtw8822c_hw_spec },  /* RTL8822CE */
    { 0xC82F, &rtw8822c_hw_spec },  /* RTL8822CE variant */
    { 0xC821, &rtw8821c_hw_spec },  /* RTL8821CE */
    { 0xB821, &rtw8821c_hw_spec },  /* RTL8821CE variant */
    { 0, nullptr }
};

/* Forward declaration of hw_callbacks struct from compat.c */
struct rtw88_hw_callbacks {
    void (*rx_frame)(void *kext_hw, struct sk_buff *skb);
    void (*tx_status)(void *kext_hw, struct sk_buff *skb);
    void (*scan_done)(void *kext_hw, bool aborted);
};

#define super OSObject
OSDefineMetaClassAndStructors(RTW88IEEE80211, OSObject)

/* ------------------------------------------------------------------ */
/*  Static compat callbacks                                             */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::compat_rx_frame(void *kext_hw, struct sk_buff *skb)
{
    RTW88IEEE80211 *self = (RTW88IEEE80211 *)kext_hw;
    if (self) self->rxFrame(skb);
}

void RTW88IEEE80211::compat_tx_status(void *kext_hw, struct sk_buff *skb)
{
    RTW88IEEE80211 *self = (RTW88IEEE80211 *)kext_hw;
    if (self) self->txStatus(skb);
    kfree_skb(skb);
}

void RTW88IEEE80211::compat_scan_done(void *kext_hw, bool aborted)
{
    RTW88IEEE80211 *self = (RTW88IEEE80211 *)kext_hw;
    if (self) self->scanDone(aborted);
}

/* ------------------------------------------------------------------ */
/*  Factory / init / free                                               */
/* ------------------------------------------------------------------ */

RTW88IEEE80211 *RTW88IEEE80211::create(RTW88PCIDevice *dev, struct pci_dev *pci)
{
    RTW88IEEE80211 *obj = new RTW88IEEE80211;
    if (obj && !obj->init(dev, pci)) {
        obj->release();
        return nullptr;
    }
    return obj;
}

RTW88IEEE80211 *RTW88IEEE80211::createAirport(RTW88RxDelegate *delegate, struct pci_dev *pci)
{
    RTW88IEEE80211 *obj = new RTW88IEEE80211;
    if (obj && !obj->init(delegate, pci)) {
        obj->release();
        return nullptr;
    }
    return obj;
}

bool RTW88IEEE80211::init(RTW88RxDelegate *delegate, struct pci_dev *pci)
{
    if (!super::init()) return false;
    _parent = delegate;
    _pcidev = pci;
    _lock    = IOLockAlloc();
    _bssLock = IOLockAlloc();
    if (!_lock || !_bssLock) return false;
    _connectTC = thread_call_allocate((thread_call_func_t)RTW88IEEE80211::connectTCFn,
                                       (thread_call_param_t)this);
    _manualScanTC = thread_call_allocate((thread_call_func_t)RTW88IEEE80211::manualScanTCFn,
                                         (thread_call_param_t)this);
    if (!_connectTC || !_manualScanTC) return false;
    /* Install callbacks into compat layer */
    static struct rtw88_hw_callbacks cbs = {
        .rx_frame  = RTW88IEEE80211::compat_rx_frame,
        .tx_status = RTW88IEEE80211::compat_tx_status,
        .scan_done = RTW88IEEE80211::compat_scan_done,
    };
    rtw88_set_hw_callbacks(&cbs, this);
    /* Set up workloop / timer for state machine */
    _wl = IOWorkLoop::workLoop();
    if (!_wl) return false;
    _gate = IOCommandGate::commandGate(this);
    if (!_gate) return false;
    if (_wl->addEventSource(_gate) != kIOReturnSuccess) return false;
    _timer = IOTimerEventSource::timerEventSource(this,
        &RTW88IEEE80211::timerFired);
    if (!_timer) return false;
    if (_wl->addEventSource(_timer) != kIOReturnSuccess) return false;
    /* RX A-MPDU reorder: lock + hole-flush timer.  The timer lives on the
     * RX/interrupt workloop (not _wl) so reorder-released frames and normal RX
     * frames are delivered from the same thread — injectRxFrame's queue+
     * flush is not safe against concurrent callers. */
    _rxBaLock = IOLockAlloc();
    if (!_rxBaLock) return false;
    IOWorkLoop *rxwl = _parent ? _parent->getRxWorkLoop() : nullptr;
    if (!rxwl) return false;
    _reorderTimer = IOTimerEventSource::timerEventSource(this,
        &RTW88IEEE80211::reorderTimerFired);
    if (!_reorderTimer) return false;
    if (rxwl->addEventSource(_reorderTimer) != kIOReturnSuccess) return false;
    IOLog("rtw88: RTW88IEEE80211 initialized\n");
    return true;
}

bool RTW88IEEE80211::init(RTW88PCIDevice *dev, struct pci_dev *pci)
{
    if (!super::init()) return false;
    _parent = dev;
    _pcidev = pci;

    _lock    = IOLockAlloc();
    _bssLock = IOLockAlloc();
    if (!_lock || !_bssLock) return false;

    _connectTC = thread_call_allocate((thread_call_func_t)RTW88IEEE80211::connectTCFn,
                                       (thread_call_param_t)this);
    _manualScanTC = thread_call_allocate((thread_call_func_t)RTW88IEEE80211::manualScanTCFn,
                                          (thread_call_param_t)this);
    if (!_connectTC || !_manualScanTC) return false;

    /* Install callbacks into compat layer */
    static struct rtw88_hw_callbacks cbs = {
        .rx_frame  = RTW88IEEE80211::compat_rx_frame,
        .tx_status = RTW88IEEE80211::compat_tx_status,
        .scan_done = RTW88IEEE80211::compat_scan_done,
    };
    rtw88_set_hw_callbacks(&cbs, this);

    /* Set up workloop / timer for state machine */
    _wl = IOWorkLoop::workLoop();
    if (!_wl) return false;

    _gate = IOCommandGate::commandGate(this);
    if (!_gate) return false;
    if (_wl->addEventSource(_gate) != kIOReturnSuccess) return false;

    _timer = IOTimerEventSource::timerEventSource(this,
        &RTW88IEEE80211::timerFired);
    if (!_timer) return false;
    if (_wl->addEventSource(_timer) != kIOReturnSuccess) return false;

    /* RX A-MPDU reorder: lock + hole-flush timer.  The timer lives on the
     * RX/interrupt workloop (not _wl) so reorder-released frames and normal RX
     * frames are delivered from the same thread — injectRxFrame's queue+flush
     * is not safe against concurrent callers. */
    _rxBaLock = IOLockAlloc();
    if (!_rxBaLock) return false;
    IOWorkLoop *rxwl = _parent ? _parent->getRxWorkLoop() : nullptr;
    if (!rxwl) return false;
    _reorderTimer = IOTimerEventSource::timerEventSource(this,
        &RTW88IEEE80211::reorderTimerFired);
    if (!_reorderTimer) return false;
    if (rxwl->addEventSource(_reorderTimer) != kIOReturnSuccess) return false;

    IOLog("rtw88: RTW88IEEE80211 initialized\n");
    return true;
}

void RTW88IEEE80211::free()
{
    _manualScanAbort = true;
    if (_manualScanTC) { thread_call_cancel_wait(_manualScanTC); thread_call_free(_manualScanTC); _manualScanTC = nullptr; }
    if (_connectTC) { thread_call_cancel_wait(_connectTC); thread_call_free(_connectTC); _connectTC = nullptr; }

    /* The compat layer keeps a raw callback context.  Clear it before this
     * object can disappear so a late RX/TX completion cannot call freed memory. */
    rtw88_set_hw_callbacks(nullptr, nullptr);
    clearKeys();
    releaseSta();
    if (_rxBaLock)
        rxBaTeardownAll();
    if (_reorderTimer) {
        IOWorkLoop *rxwl = _parent ? _parent->getRxWorkLoop() : nullptr;
        if (rxwl) rxwl->removeEventSource(_reorderTimer);
        _reorderTimer->release();
        _reorderTimer = nullptr;
    }
    if (_rxBaLock) { IOLockFree(_rxBaLock); _rxBaLock = nullptr; }
    if (_timer)  { if (_wl) _wl->removeEventSource(_timer); _timer->release(); _timer = nullptr; }
    if (_gate)   { if (_wl) _wl->removeEventSource(_gate); _gate->release(); _gate = nullptr; }
    if (_wl)     { _wl->release();   _wl = nullptr; }
    if (_lock)   { IOLockFree(_lock);    _lock = nullptr; }
    if (_bssLock){ IOLockFree(_bssLock); _bssLock = nullptr; }

    /* Free BSS list */
    RTW88BSS *b = _bssList;
    while (b) {
        RTW88BSS *n = b->next;
        IOFree(b, sizeof(*b));
        b = n;
    }
    _bssList = nullptr;
    if (_scanRequest) { IOFree(_scanRequest, sizeof(*_scanRequest)); _scanRequest = nullptr; }
    super::free();
}

void RTW88IEEE80211::clearKeys()
{
    if (_powered && _hw && _hw->ops && _hw->ops->set_key) {
        if (_ptkConf)
            _hw->ops->set_key(_hw, DISABLE_KEY, _vif, _sta, _ptkConf);
        if (_gtkConf)
            _hw->ops->set_key(_hw, DISABLE_KEY, _vif, nullptr, _gtkConf);
    }

    if (_ptkConf) {
        IOFree(_ptkConf, sizeof(*_ptkConf));
        _ptkConf = nullptr;
    }
    if (_gtkConf) {
        IOFree(_gtkConf, sizeof(*_gtkConf));
        _gtkConf = nullptr;
    }
    memset(_pmk, 0, sizeof(_pmk));
    memset(_password, 0, sizeof(_password));
    _pmkProvided = false;
    _externalPTKInstalled = false;
    _externalGTKInstalled = false;
    memset(_ptk, 0, sizeof(_ptk));
    memset(_gtk, 0, sizeof(_gtk));
    memset(_ccmpTxPn, 0, sizeof(_ccmpTxPn));
    _rxCcmpIvSkipLogged = false;
}

void RTW88IEEE80211::releaseSta()
{
    _txBaPending = false;
    if (!_sta)
        return;

    if (_hw && _hw->ops && _hw->ops->sta_remove && _vif)
        _hw->ops->sta_remove(_hw, _vif, _sta);

    IOFree(_sta, _staAllocSize ? _staAllocSize : sizeof(struct ieee80211_sta));
    _sta = nullptr;
    _staAllocSize = 0;
    _txBaActive = false;
    _dataSeq = 0;
    rxBaTeardownAll();
}

static const char *rtw88CipherName(uint32_t cipher)
{
    switch (cipher) {
    case WLAN_CIPHER_SUITE_CCMP:
        return "CCMP";
    case WLAN_CIPHER_SUITE_TKIP:
        return "TKIP";
    default:
        return "unknown";
    }
}

bool RTW88IEEE80211::installKey(struct ieee80211_key_conf **slot, bool pairwise,
                                uint8_t keyidx, uint32_t cipher,
                                const uint8_t *tk, uint8_t tk_len)
{
    if (!_hw || !_hw->ops || !_hw->ops->set_key || !slot || !tk || tk_len == 0 || tk_len > 32)
        return false;
    if (cipher != WLAN_CIPHER_SUITE_CCMP && cipher != WLAN_CIPHER_SUITE_TKIP)
        return false;
    if (cipher == WLAN_CIPHER_SUITE_CCMP && tk_len != 16)
        return false;
    if (cipher == WLAN_CIPHER_SUITE_TKIP && tk_len != 32)
        return false;

    if (*slot) {
        _hw->ops->set_key(_hw, DISABLE_KEY, _vif, pairwise ? _sta : nullptr, *slot);
        IOFree(*slot, sizeof(**slot));
        *slot = nullptr;
    }

    struct ieee80211_key_conf *key =
        (struct ieee80211_key_conf *)IOMallocZero(sizeof(*key));
    if (!key)
        return false;

    key->cipher = cipher;
    key->keyidx = (s8)keyidx;
    key->flags = pairwise ? IEEE80211_KEY_FLAG_PAIRWISE : 0;
    key->keylen = tk_len;
    key->iv_len = 8;
    key->icv_len = (cipher == WLAN_CIPHER_SUITE_TKIP) ? 4 : 8;
    memcpy(key->key, tk, tk_len);

    int ret = _hw->ops->set_key(_hw, SET_KEY, _vif, pairwise ? _sta : nullptr, key);
    if (ret) {
        IOLog("rtw88: failed to install %s %s key ret=%d\n",
              pairwise ? "pairwise" : "group", rtw88CipherName(cipher), ret);
        IOFree(key, sizeof(*key));
        return false;
    }

    *slot = key;
    if (pairwise)
        memset(_ccmpTxPn, 0, sizeof(_ccmpTxPn));
    IOLog("rtw88: installed %s %s key idx=%u hw_idx=%u\n",
          pairwise ? "pairwise" : "group", rtw88CipherName(cipher),
          keyidx, key->hw_key_idx);
    return true;
}

/* ------------------------------------------------------------------ */
/*  start / stop                                                        */
/* ------------------------------------------------------------------ */

IOReturn RTW88IEEE80211::start()
{
    RTW88_STAGE("IEEE80211::start entered");

    /* Look up chip info from PCI device ID */
    const struct rtw_chip_info *chip = nullptr;
    for (int i = 0; rtw88_pci_chip_table[i].device != 0; i++) {
        if (rtw88_pci_chip_table[i].device == _pcidev->device) {
            chip = rtw88_pci_chip_table[i].chip;
            break;
        }
    }
    if (!chip) {
        IOLog("rtw88: unknown PCI device %04x — cannot probe\n", _pcidev->device);
        return kIOReturnUnsupported;
    }
    RTW88_STAGE("chip matched: device=%04x", _pcidev->device);

    const struct pci_device_id fake_id = {
        .vendor      = _pcidev->vendor,
        .device      = _pcidev->device,
        .subvendor   = PCI_ANY_ID,
        .subdevice   = PCI_ANY_ID,
        .driver_data = (unsigned long)chip,
    };

    RTW88_STAGE("calling rtw_pci_probe");
    int ret = rtw_pci_probe(_pcidev, &fake_id);
    RTW88_STAGE("rtw_pci_probe returned %d", ret);
    if (ret != 0) {
        IOLog("rtw88: rtw_pci_probe failed: %d\n", ret);
        return kIOReturnError;
    }

    /* Use the hw pointer that ieee80211_alloc_hw() registered in the compat
     * layer via rtw88_register_hw().  rtw88_get_hw() is the external-linkage
     * accessor for the static g_rtw88_hw variable — avoids both the fragile
     * *(ieee80211_hw **)rtwdev double-dereference and the UB of declaring
     * 'extern' on a static variable from another TU. */
    _hw = rtw88_get_hw();

    /* rtwdev is hw->priv (allocated contiguously after ieee80211_hw in alloc_hw).
     * Note: rtw_pci_probe stores hw (not rtwdev) in pdev->driver_data via pci_set_drvdata(). */
    _rtwdev = _hw ? (struct rtw_dev *)_hw->priv : nullptr;

    RTW88_STAGE("rtwdev=%p hw=%p", (void *)_rtwdev, (void *)_hw);

    if (!_hw || !_rtwdev || !_hw->wiphy || !_hw->ops) {
        IOLog("rtw88: probe returned incomplete hardware state\n");
        goto fail_probe;
    }

    /* Read MAC address — SET_IEEE80211_PERM_ADDR() copies EFuse MAC into
     * hw->wiphy->perm_addr during rtw_register_hw(); read it from there. */
    memcpy(_macAddr, _hw->wiphy->perm_addr, 6);
    IOLog("rtw88: MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n",
          _macAddr[0], _macAddr[1], _macAddr[2],
          _macAddr[3], _macAddr[4], _macAddr[5]);

    /* Create virtual interface in the driver.
     * NOTE: _rtwdev must NOT be reassigned here. It has been correctly set from
     * _hw->priv above. */
    RTW88_STAGE("adding STA interface");
    _vifAllocSize = sizeof(struct ieee80211_vif) + (size_t)_hw->vif_data_size;
    _vif = (struct ieee80211_vif *)IOMallocZero(_vifAllocSize);
    if (!_vif) {
        IOLog("rtw88: failed to allocate STA interface\n");
        goto fail_probe;
    }
    _vif->type = NL80211_IFTYPE_STATION;
    memcpy(_vif->addr, _macAddr, 6);
    /* bss_conf.bssid must never be NULL — iterators dereference it
     * for every RX frame even before association. */
    _vif->bss_conf.bssid = _vif->bss_conf.bssid_buf;
    if (!_hw->ops->add_interface) {
        IOLog("rtw88: driver has no add_interface operation\n");
        goto fail_vif;
    }
    if (!_hw->ops->start) {
        IOLog("rtw88: driver has no start operation\n");
        goto fail_vif;
    }
    RTW88_STAGE("calling hw->ops->start");
    ret = _hw->ops->start(_hw);
    RTW88_STAGE("hw->ops->start returned %d", ret);
    if (ret != 0) {
        IOLog("rtw88: hw->ops->start failed: %d\n", ret);
        goto fail_vif;
    }
    _powered = true;

    ret = _hw->ops->add_interface(_hw, _vif);
    if (ret != 0) {
        IOLog("rtw88: add_interface failed: %d\n", ret);
        goto fail_vif;
    }
    rtw88_register_vif(_vif);
    RTW88_STAGE("add_interface done");


    _state = RTW88_STATE_IDLE;
    _associatedVisible = false;
    _scanReturnState = RTW88_STATE_IDLE;
    RTW88_STAGE("IEEE80211::start complete — SUCCESS");
    return kIOReturnSuccess;

fail_vif:
    if (_powered && _hw && _hw->ops && _hw->ops->stop) {
        _hw->ops->stop(_hw, false);
        _powered = false;
    }
    if (_vif) {
        IOFree(_vif, _vifAllocSize);
        _vifAllocSize = 0;
        _vif = nullptr;
    }
fail_probe:
    rtw_pci_remove(_pcidev);
    _rtwdev = nullptr;
    _hw = nullptr;
    _powered = false;
    return kIOReturnError;
}

void RTW88IEEE80211::stop()
{
    IOLog("rtw88: IEEE80211 stop\n");
    cancelAuthentication();
    _manualScanAbort = true;
    if (_manualScanTC) thread_call_cancel_wait(_manualScanTC);
    if (_connectTC) thread_call_cancel_wait(_connectTC);
    if (_reorderTimer) { _reorderTimer->cancelTimeout(); _reorderTimer->disable(); }
    if (_timer)
        _timer->cancelTimeout();

    if ((_state == RTW88_STATE_CONNECTED ||
         (_state == RTW88_STATE_SCANNING &&
          _scanReturnState == RTW88_STATE_CONNECTED)) && _powered)
        doDisconnect();
    else {
        clearKeys();
        releaseSta();
    }

    if (_state == RTW88_STATE_SCANNING && _hw && _hw->ops && _hw->ops->cancel_hw_scan)
        _hw->ops->cancel_hw_scan(_hw, _vif);

    if (_vif && _hw && _hw->ops) {
        rtw88_unregister_vif();
        if (_hw->ops->remove_interface) {
            _hw->ops->remove_interface(_hw, _vif);
        }
        IOFree(_vif, _vifAllocSize);
        _vifAllocSize = 0;
        _vif = nullptr;
    } else if (_vif) {
        rtw88_unregister_vif();
        IOFree(_vif, _vifAllocSize);
        _vifAllocSize = 0;
        _vif = nullptr;
    }

    if (_powered && _hw && _hw->ops && _hw->ops->stop)
        _hw->ops->stop(_hw, false);
    _powered = false;

    if (_pcidev) rtw_pci_remove(_pcidev);
    _rtwdev = nullptr;
    _hw     = nullptr;
    _state  = RTW88_STATE_IDLE;
    _associatedVisible = false;
    _scanReturnState = RTW88_STATE_IDLE;
}

/* ------------------------------------------------------------------ */
/*  Power on/off (called from enable/disable)                          */
/* ------------------------------------------------------------------ */

IOReturn RTW88IEEE80211::setReceiveMulticast(bool active)
{
    if (!_hw || !_hw->ops || !_hw->ops->configure_filter)
        return kIOReturnNotReady;
    _receiveMulticast = active;
    if (_powered) {
        unsigned int flags = (active || _awdlReceiveMode) ? FIF_ALLMULTI : 0;
        _hw->ops->configure_filter(_hw, FIF_ALLMULTI, &flags, 0);
    }
    return kIOReturnSuccess;
}

IOReturn RTW88IEEE80211::setAWDLReceiveMode(bool active)
{
    if (!_hw || !_hw->ops || !_hw->ops->configure_filter)
        return kIOReturnNotReady;

    if (!active && _awdlOffChannel)
        restoreSTAChannelAfterAWDL();
    _awdlReceiveMode = active;
    if (!active) {
        _awdlChannel = 0;
        _awdlPreparedChannel = 0;
        _awdlOffChannel = false;
    }

    /* AWDL's virtual MAC is not programmed as the primary hardware address.
     * FIF_OTHER_BSS maps to Realtek BIT_AAP (accept all physical addresses),
     * allowing direct AWDL unicast frames to reach our software classifier.
     * Preserve multicast reception while AWDL is active. */
    // Accepting a second destination MAC is insufficient: AWDL uses its own
    // BSSID too. Clear the hardware BSSID checks through the rtw88 filter API.
    unsigned int changed = FIF_OTHER_BSS | FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC;
    unsigned int flags = active ? (FIF_OTHER_BSS | FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC)
                                : (_receiveMulticast ? FIF_ALLMULTI : 0);
    if (_powered) _hw->ops->configure_filter(_hw, changed, &flags, 0);
    IOLog("rtw88: AWDL receive mode %s flags=0x%x\n",
          active ? "enabled" : "disabled", flags);
    return kIOReturnSuccess;
}

void RTW88IEEE80211::setAWDLAddress(const uint8_t *mac)
{
    if (mac) memcpy(_awdlAddress, mac, sizeof(_awdlAddress));
}

bool RTW88IEEE80211::canTransmitAWDL() const
{
    return _powered && _awdlReceiveMode && _hw && _hw->conf.chandef.chan &&
        _awdlChannel && _hw->conf.chandef.chan->hw_value == _awdlChannel &&
        (_state == RTW88_STATE_IDLE || _state == RTW88_STATE_CONNECTED);
}

void RTW88IEEE80211::restoreSTAChannelAfterAWDL()
{
    if (!_awdlOffChannel) return;

    /* If association disappeared while AWDL owned the PHY, there is no AP
     * channel to restore.  Just clear the ownership latch. */
    if (!_powered || !_hw || !_vif || !_associatedVisible ||
        _state != RTW88_STATE_CONNECTED || !_targetBSS.channel) {
        _awdlOffChannel = false;
        _awdlPreparedChannel = 0;
        return;
    }

    struct ieee80211_channel *home = nullptr;
    for (int b = 0; b < NL80211_NUM_BANDS && !home; ++b) {
        struct ieee80211_supported_band *band = _hw->wiphy ? _hw->wiphy->bands[b] : nullptr;
        if (!band) continue;
        for (int i = 0; i < band->n_channels; ++i) {
            if (band->channels[i].hw_value == _targetBSS.channel) {
                home = &band->channels[i];
                home->band = band->band;
                break;
            }
        }
    }
    if (!home) {
        _awdlOffChannel = false;
        _awdlPreparedChannel = 0;
        return;
    }

    setConnectedChandef(home);
    rtw88_restore_connected_hw_timeslice(_hw, _vif, _targetBSS.bssid);
    _awdlOffChannel = false;
    _awdlPreparedChannel = 0;

    /* Tell the AP it may release buffered frames now that the single PHY is
     * back on the infrastructure channel. */
    (void)txNullFunc(false);
}

IOReturn RTW88IEEE80211::setAWDLChannel(uint16_t channel)
{
    if (!_powered || !_hw || !_hw->wiphy || channel == 0)
        return kIOReturnNotReady;

    struct ieee80211_channel *target = nullptr;
    for (int b = 0; b < NL80211_NUM_BANDS && !target; ++b) {
        struct ieee80211_supported_band *band = _hw->wiphy->bands[b];
        if (!band) continue;
        for (int i = 0; i < band->n_channels; ++i) {
            if (band->channels[i].hw_value == channel &&
                !(band->channels[i].flags & (IEEE80211_CHAN_DISABLED | IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR))) {
                target = &band->channels[i];
                target->band = band->band;
                break;
            }
        }
    }
    if (!target)
        return kIOReturnUnsupported;

    _awdlChannel = channel;

    /* Same-channel AWDL needs no timeslice at all.  If a previous window left
     * us off-channel, return home first. */
    if (_state == RTW88_STATE_CONNECTED && _associatedVisible &&
        channel == _targetBSS.channel) {
        if (_awdlOffChannel) restoreSTAChannelAfterAWDL();
        return (_hw->conf.chandef.chan &&
                _hw->conf.chandef.chan->hw_value == channel)
                   ? kIOReturnSuccess : kIOReturnNotReady;
    }

    /* Connected single-PHY coexistence.  The AWDL scheduler calls this only
     * inside a bounded common availability window.  Put the AP into PS before
     * retuning so it buffers unicast traffic, exactly like our working
     * connected software-scan path. */
    if (_state == RTW88_STATE_CONNECTED && _associatedVisible) {
        if (_awdlOffChannel && _hw->conf.chandef.chan == target)
            return kIOReturnSuccess;
        if (_awdlOffChannel)
            restoreSTAChannelAfterAWDL();

        if (!txNullFunc(true))
            return kIOReturnNotReady;
        IOSleep(6);

        _hw->conf.chandef.chan = target;
        _hw->conf.chandef.width = NL80211_CHAN_WIDTH_20_NOHT;
        _hw->conf.chandef.center_freq1 = target->center_freq;
        rtw88_awdl_timeslice_switch_channel(_hw);
        _awdlPreparedChannel = channel;
        _awdlOffChannel = true;
        return kIOReturnSuccess;
    }

    /* A scan/auth/join owns the radio exclusively. */
    if (_state != RTW88_STATE_IDLE)
        return kIOReturnBusy;

    if (_awdlPreparedChannel == channel && _hw->conf.chandef.chan == target &&
        _hw->conf.chandef.width == NL80211_CHAN_WIDTH_20_NOHT)
        return kIOReturnSuccess;

    _hw->conf.chandef.chan = target;
    _hw->conf.chandef.width = NL80211_CHAN_WIDTH_20_NOHT;
    _hw->conf.chandef.center_freq1 = target->center_freq;
    rtw88_awdl_switch_channel(_hw);
    _awdlPreparedChannel = channel;
    return kIOReturnSuccess;
}



IOReturn RTW88IEEE80211::powerOn()
{
    IOLog("rtw88: IEEE80211 powerOn\n");
    if (_powered) return kIOReturnSuccess;
    if (!_hw || !_hw->ops || !_hw->ops->start) return kIOReturnNotReady;
    int ret = _hw->ops->start(_hw);
    if (ret) {
        IOLog("rtw88: rtw_core_start failed: %d\n", ret);
        return kIOReturnError;
    }
    _powered = true;
    _awdlPreparedChannel = 0;
    _awdlOffChannel = false;
    if (rtw88_restore_interface(_hw, _vif) != 0) {
        powerOff();
        return kIOReturnError;
    }
    IOReturn filterResult = _awdlReceiveMode ? setAWDLReceiveMode(true) : setReceiveMulticast(_receiveMulticast);
    if (filterResult != kIOReturnSuccess) powerOff();
    return filterResult;
}

void RTW88IEEE80211::powerOff()
{
    IOLog("rtw88: IEEE80211 powerOff\n");
    if (!_powered) return;
    if (_hw && _hw->ops && _hw->ops->stop)
        _hw->ops->stop(_hw, false);
    _powered = false;
    _awdlPreparedChannel = 0;
    _awdlOffChannel = false;
}

/* ------------------------------------------------------------------ */
/*  Interrupt dispatch                                                  */
/* ------------------------------------------------------------------ */

extern "C" void rtw88_trigger_interrupt(void);

void RTW88IEEE80211::handleInterrupt()
{
    /* Never log every IRQ: console I/O on the RX/TX hot path adds measurable
     * scheduling jitter and can amplify stalls while traffic is heavy. */
    rtw88_trigger_interrupt();
}

/* ------------------------------------------------------------------ */
/*  TX path: Ethernet → 802.11 data frame                              */
/* ------------------------------------------------------------------ */

UInt32 RTW88IEEE80211::outputPacket(mbuf_t m)
{
    /* Need an associated STA before data frames can be sent. */
    bool connected = (_state == RTW88_STATE_CONNECTED) ||
                     (_state == RTW88_STATE_SCANNING &&
                      _scanReturnState == RTW88_STATE_CONNECTED &&
                      (_manualScanChannelCount == 0 ||
                       _manualScanOnHomeChannel));
    if (!_powered || !connected || !_rtwdev || !_hw || !_vif || !_sta) {
        mbuf_freem(m);
        return kIOReturnOutputDropped;
    }

    /* Apple RSN needs the BSD link marked up to exchange EAPOL, but before
     * PTK+GTK are installed the controlled port must carry EAPOL only. */
    if (_wpa2 && _externalSupplicant && !externalKeysReady()) {
        uint8_t eh[14] = {};
        if (mbuf_pkthdr_len(m) < sizeof(eh) ||
            mbuf_copydata(m, 0, sizeof(eh), eh) != 0 ||
            (uint16_t)((eh[12] << 8) | eh[13]) != ETH_P_PAE) {
            mbuf_freem(m);
            return kIOReturnOutputDropped;
        }
    }

    /* txDataFrame() encapsulates the Ethernet frame as an 802.11 data frame
     * and consumes (frees) the mbuf in all paths. */
    return txDataFrame(m) ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
}

/* ------------------------------------------------------------------ */
/*  RX path: sk_buff from driver → mbuf to macOS                       */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::rxFrame(struct sk_buff *skb)
{
    if (!skb) return;
    if (skb->len < 24) { kfree_skb(skb); return; }

    struct ieee80211_rx_status *rxs = IEEE80211_SKB_RXCB(skb);
    /* A corrupted frame must not change association state, peer clocks, or
     * enter either network interface even if a diagnostic RX filter admits it. */
    if (rxs->flag & RX_FLAG_FAILED_FCS_CRC) {
        kfree_skb(skb);
        return;
    }
    if (rtw88StationSignal(skb->data, skb->len, _macAddr, _targetBSS.bssid,
                          rxs->signal,
                          (rxs->flag & (RX_FLAG_NO_SIGNAL_VAL | RX_FLAG_FAILED_FCS_CRC)) != 0))
        _rssi = rxs->signal;

    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
    __le16 fc = hdr->frame_control;

    _rxFrameCount++;

    if (ieee80211_is_mgmt(fc)) {
        processRxMgmt(skb);
    } else if (ieee80211_is_data(fc)) {
        /* AWDL data is a direct (no-DS) 802.11 frame and remains valid even
         * when the infrastructure STA is disconnected. Consume it before the
         * normal ToDS/FromDS path applies association-state assumptions. */
        if (!tryDeliverAWDLDataFrame(skb))
            processRxData(skb);
    } else {
        kfree_skb(skb);
    }
}

void RTW88IEEE80211::processRxMgmt(struct sk_buff *skb)
{
    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
    __le16 fc = hdr->frame_control;
    uint16_t stype = le16_to_cpu(fc) & 0x00f0;

    switch (stype) {
    case 0x0080: /* beacon */
    case 0x0050: /* probe response */
        if (_state == RTW88_STATE_SCANNING)
            processScanResult(skb);
        else
            kfree_skb(skb);
        break;

    case 0x00B0: /* auth */
        if (_state == RTW88_STATE_AUTHENTICATING && !authenticationCancelled()) {
            uint16_t status = 0;
            if (rtw88AuthResponse(skb->data, skb->len, _macAddr, _targetBSS.bssid, &status)) {
                if (status == 0) {
                    IOLog("rtw88: auth success, sending assoc\n");
                    doAssociate();
                } else {
                    IOLog("rtw88: auth rejected by AP status=%u\n", status);
                    _timer->cancelTimeout();
                    _associatedVisible = false;
                    _state = RTW88_STATE_IDLE;
                }
            }
        }
        kfree_skb(skb);
        break;

    case 0x0010: /* assoc response */
        if (_state == RTW88_STATE_ASSOCIATING)
            processAssocResponse(skb);
        else
            kfree_skb(skb);
        break;

    case 0x0030: /* reassoc response */
        if (_state == RTW88_STATE_ASSOCIATING)
            processAssocResponse(skb);
        else
            kfree_skb(skb);
        break;

    case 0x00A0: /* disassoc */
    case 0x00C0: /* deauth */
        /* Both subtypes require the two-byte reason code. A header-only
         * truncated frame must not tear down keys or the Wi-Fi association. */
        if (skb->len < sizeof(struct ieee80211_hdr_3addr) + 2) {
            kfree_skb(skb);
            break;
        }
        if (_state == RTW88_STATE_CONNECTED ||
            _state == RTW88_STATE_HANDSHAKING ||
            (_state == RTW88_STATE_SCANNING &&
             _scanReturnState == RTW88_STATE_CONNECTED)) {
            struct ieee80211_hdr_3addr *h3 =
                (struct ieee80211_hdr_3addr *)skb->data;
            bool fromTarget = memcmp(h3->addr3, _targetBSS.bssid, 6) == 0 ||
                              memcmp(h3->addr2, _targetBSS.bssid, 6) == 0;
            bool addressedToUs = memcmp(h3->addr1, _macAddr, 6) == 0 ||
                                 is_broadcast_ether_addr(h3->addr1);
            if (!fromTarget || !addressedToUs) {
                kfree_skb(skb);
                break;
            }
            /* Log which frame and the reason code — the AP's reason for
             * dropping us a few seconds after connect (e.g. excessive retries
             * vs. class-3 violation) is the key diagnostic. */
            {
                const uint8_t *rb = skb->data + sizeof(*h3);
                uint16_t reason = (skb->len >= sizeof(*h3) + 2) ?
                                  (uint16_t)(rb[0] | (rb[1] << 8)) : 0;
                _deauthReason = reason;
                IOLog("rtw88: %s from AP, reason=%u — disconnecting\n",
                      (stype == 0x00C0) ? "deauth" : "disassoc", reason);
            }
            clearKeys();
            _txBaActive = false;
            rxBaTeardownAll();
            _associatedVisible = false;
            _state = RTW88_STATE_IDLE;
            _scanReturnState = RTW88_STATE_IDLE;
            if (_parent)
                _parent->setLinkStatus(kIONetworkLinkValid);
            /* IO80211/CoreWiFi needs the asynchronous deauth notification,
             * not only a link-state change.  AirportItlwm forwards the same
             * net80211 event as APPLE80211_M_DEAUTH_RECEIVED. */
            if (_delegate)
                _delegate->rtw88Event(kRTW88EventDeauth, &_deauthReason);
        }
        kfree_skb(skb);
        break;

    case 0x00D0: /* action */
        /* AWDL/P2P action frames are meaningful even while the infrastructure
         * STA is not associated. Give the native virtual interface a copy
         * before handling infrastructure BlockAck actions locally. */
        if (_parent) {
            struct ieee80211_rx_status *rxs = IEEE80211_SKB_RXCB(skb);
            uint16_t rxChannel = 0;
            if (rxs && rxs->band < NL80211_NUM_BANDS && _hw && _hw->wiphy) {
                struct ieee80211_supported_band *band = _hw->wiphy->bands[rxs->band];
                if (band) {
                    /* rx_status::freq is not consistently populated by every
                     * rtw88 path in this port. Prefer the currently tuned
                     * chandef and fall back to the AWDL home channel. */
                    if (_hw->conf.chandef.chan)
                        rxChannel = (uint16_t)_hw->conf.chandef.chan->hw_value;
                    else if (_awdlChannel)
                        rxChannel = _awdlChannel;
                }
            }
            _parent->injectRxActionFrame(skb->data, skb->len,
                                         rxs ? (int8_t)rxs->signal : 0,
                                         rxChannel);
        }

        if (_state == RTW88_STATE_CONNECTED) {
            struct ieee80211_hdr_3addr *h3 =
                (struct ieee80211_hdr_3addr *)skb->data;
            const uint8_t *b = skb->data + sizeof(*h3);
            uint32_t blen = (skb->len > sizeof(*h3)) ?
                            skb->len - (uint32_t)sizeof(*h3) : 0;
            /* BlockAck (ADDBA/DELBA) action frames from our AP only. */
            if (blen >= 2 && b[0] == WLAN_CATEGORY_BACK &&
                memcmp(h3->addr3, _targetBSS.bssid, 6) == 0)
                handleBackAction(b, blen);
        }
        kfree_skb(skb);
        break;

    default:
        kfree_skb(skb);
        break;
    }
}

static uint32_t rtw88ReadSuite(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void rtw88WriteSuite(uint8_t *p, uint32_t suite)
{
    p[0] = (uint8_t)(suite >> 24);
    p[1] = (uint8_t)(suite >> 16);
    p[2] = (uint8_t)(suite >> 8);
    p[3] = (uint8_t)suite;
}

static bool rtw88RsnSelectCcmpPsk(const uint8_t *rsn, uint8_t len,
                                  uint32_t *pairwise_cipher,
                                  uint32_t *group_cipher)
{
    if (!rsn || len < 8) return false;
    const uint8_t *p = rsn;
    const uint8_t *end = rsn + len;

    if ((size_t)(end - p) < 8)
        return false;

    if (p[0] != 1 || p[1] != 0) return false;
    p += 2; /* RSN version 1 */
    uint32_t group = rtw88ReadSuite(p);
    p += 4;

    if ((size_t)(end - p) < 2)
        return false;
    uint16_t pairwiseCount = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    if (pairwiseCount > (size_t)(end - p) / 4)
        return false;

    bool hasCcmp = false;
    for (uint16_t i = 0; i < pairwiseCount; i++, p += 4) {
        if (rtw88ReadSuite(p) == WLAN_CIPHER_SUITE_CCMP)
            hasCcmp = true;
    }

    if ((size_t)(end - p) < 2)
        return false;
    uint16_t akmCount = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;
    if (akmCount > (size_t)(end - p) / 4)
        return false;

    bool hasPsk = false;
    for (uint16_t i = 0; i < akmCount; i++, p += 4) {
        if (rtw88ReadSuite(p) == 0x000FAC02) /* 00-0f-ac:2 PSK */
            hasPsk = true;
    }

    if (!hasCcmp || !hasPsk)
        return false;

    if (group != WLAN_CIPHER_SUITE_CCMP &&
        group != WLAN_CIPHER_SUITE_TKIP)
        return false;

    if (pairwise_cipher)
        *pairwise_cipher = WLAN_CIPHER_SUITE_CCMP;
    if (group_cipher)
        *group_cipher = group;
    return true;
}

static uint16_t rtw88BuildSelectedRsnIe(uint8_t *out, uint32_t group_cipher)
{
    if (group_cipher != WLAN_CIPHER_SUITE_TKIP)
        group_cipher = WLAN_CIPHER_SUITE_CCMP;

    uint8_t *p = out;
    *p++ = WLAN_EID_RSN;
    *p++ = 20;           /* body length */
    *p++ = 1; *p++ = 0;  /* version */
    rtw88WriteSuite(p, group_cipher); p += 4;
    *p++ = 1; *p++ = 0;  /* one pairwise cipher */
    rtw88WriteSuite(p, WLAN_CIPHER_SUITE_CCMP); p += 4;
    *p++ = 1; *p++ = 0;  /* one AKM */
    rtw88WriteSuite(p, 0x000FAC02); p += 4; /* PSK */
    *p++ = 0; *p++ = 0;  /* RSN capabilities */
    return (uint16_t)(p - out);
}

void RTW88IEEE80211::processScanResult(struct sk_buff *skb)
{
    if (!skb || skb->len < sizeof(struct ieee80211_hdr_3addr) + 12) {
        kfree_skb(skb);
        return;
    }

    /* Parse beacon/probe-resp minimal fields */
    const uint8_t *body = skb->data + sizeof(struct ieee80211_hdr_3addr);
    const uint8_t *end  = skb->data + skb->len;
    /* Skip: timestamp(8), beacon_int(2), capability(2) */
    body += 12;

    RTW88BSS *bss = (RTW88BSS *)IOMallocZero(sizeof(RTW88BSS));
    if (!bss) { kfree_skb(skb); return; }

    bss->beacon_interval = skb->data[32] | (uint16_t(skb->data[33]) << 8);
    bss->capabilities = skb->data[34] | (uint16_t(skb->data[35]) << 8);

    /* BSSID is addr3 in a beacon from AP */
    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
    memcpy(bss->bssid, hdr->addr3, 6);

    /* Walk IEs */
    while (body + 2 <= end) {
        uint8_t id = body[0];
        uint8_t len = body[1];
        if (body + 2 + len > end) break;

        if (id == WLAN_EID_SSID && len > 0 && len <= 32) {
            memcpy(bss->ssid, body + 2, len);
            bss->ssid_len = len;
        } else if (id == WLAN_EID_DS_PARAMS && len >= 1) {
            bss->channel = body[2];
        } else if (id == WLAN_EID_HT_OPERATION && len >= 1 && bss->channel == 0) {
            bss->channel = body[2];
        } else if (id == WLAN_EID_RSN) {
            uint32_t pairwise = 0;
            uint32_t group = 0;
            if (rtw88RsnSelectCcmpPsk(body + 2, len, &pairwise, &group)) {
                bss->cipher = pairwise;
                bss->group_cipher = group;
                bss->akm = 0x000FAC02; /* PSK */
            }
        } else if (id == WLAN_EID_VENDOR_SPECIFIC &&
                   len >= 8 && body[2] == 0x00 && body[3] == 0x50 &&
                   body[4] == 0xf2 && body[5] == 0x01 &&
                   bss->cipher == 0) {
            /* Legacy WPA IE. Keep this as scan metadata only; association
             * still prefers RSN/WPA2 when the AP advertises it. */
            bss->cipher = WLAN_CIPHER_SUITE_TKIP;
            bss->group_cipher = WLAN_CIPHER_SUITE_TKIP;
            bss->akm    = 0x000FAC02; /* PSK */
        }

        /* Copy all IEs */
        uint16_t copy = (uint16_t)(2 + len);
        if (bss->ies_len + copy < sizeof(bss->ies)) {
            memcpy(bss->ies + bss->ies_len, body, copy);
            bss->ies_len += copy;
        }
        body += 2 + len;
    }

    struct ieee80211_rx_status *rxs = IEEE80211_SKB_RXCB(skb);
    bss->rssi = rxs->signal;
    bss->freq = rxs->freq;
    bss->last_seen_scan = _scanGeneration;
    
    if (bss->channel == 0 && bss->freq) {
        int f = bss->freq;
        if (f == 2484)
            bss->channel = 14;
        else if (f >= 2412 && f <= 2472)
            bss->channel = (f - 2407) / 5;
        else if (f >= 5000 && f <= 5900)
            bss->channel = (f - 5000) / 5;
    }

    /* Add to BSS list (deduplicate by BSSID) */
    IOLockLock(_bssLock);
    for (RTW88BSS *e = _bssList; e; e = e->next) {
        if (memcmp(e->bssid, bss->bssid, 6) == 0) {
            /* Update existing */
            RTW88BSS *saved_next = e->next;
            memcpy(e, bss, sizeof(*bss));
            e->next = saved_next; /* preserve linkage */
            IOFree(bss, sizeof(*bss));
            IOLockUnlock(_bssLock);
            kfree_skb(skb);
            return;
        }
    }
    bss->next = _bssList;
    _bssList  = bss;
    _bssCount++;
    IOLockUnlock(_bssLock);

    kfree_skb(skb);
}

void RTW88IEEE80211::processRxData(struct sk_buff *skb)
{
    bool connected = (_state == RTW88_STATE_CONNECTED) ||
                     (_state == RTW88_STATE_HANDSHAKING) ||
                     (_state == RTW88_STATE_SCANNING &&
                      _scanReturnState == RTW88_STATE_CONNECTED);
    if (!connected) {
        kfree_skb(skb);
        return;
    }

    /* AWDL receive mode asks the chip to accept frames for an additional
     * virtual MAC. Re-establish the infrastructure address/BSSID filter in
     * software before the legacy Ethernet conversion path. */
    struct ieee80211_hdr *infraHdr = (struct ieee80211_hdr *)skb->data;
    uint16_t infraFc = le16_to_cpu(infraHdr->frame_control);
    bool fromDS = (infraFc & IEEE80211_FCTL_FROMDS) != 0;
    bool toUs = memcmp(infraHdr->addr1, _macAddr, 6) == 0 ||
                is_broadcast_ether_addr(infraHdr->addr1) ||
                is_multicast_ether_addr(infraHdr->addr1);
    bool fromAP = memcmp(infraHdr->addr2, _targetBSS.bssid, 6) == 0;
    if (!fromDS || !toUs || !fromAP) {
        kfree_skb(skb);
        return;
    }

    /* If this TID has an active downlink BlockAck agreement, run the frame
     * through the per-TID reorder buffer so A-MPDU subframes (and frames
     * retransmitted in a later A-MPDU) reach the stack in order.  Delivering
     * out of order collapses TCP and trips CCMP replay drops — that is the RX
     * regression aggregation otherwise causes. */
    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
    if (ieee80211_is_data_qos(hdr->frame_control)) {
        uint16_t hdrlen = ieee80211_get_hdrlen_from_skb(skb);
        if (skb->len >= hdrlen) {
            uint8_t tid = (uint8_t)(skb->data[hdrlen - 2] & 0x0f);
            if (tid < kRxBaNumTid && _rxBa[tid] && _rxBa[tid]->active) {
                uint16_t sn = (uint16_t)
                    ((le16_to_cpu(hdr->seq_ctrl) & 0xFFF0) >> 4);
                rxReorderInput(tid, skb, sn);   /* takes ownership of skb */
                return;
            }
        }
    }

    deliverDataFrame(skb);   /* no active RX BA — deliver immediately */
}

/* Strip the 802.11 header (+ optional CCMP IV), de-aggregate A-MSDU if present,
 * and hand each MSDU to the network stack as Ethernet.  Takes ownership of skb. */
void RTW88IEEE80211::deliverDataFrame(struct sk_buff *skb)
{
    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
    uint16_t hdrlen = ieee80211_get_hdrlen_from_skb(skb);
    if (skb->len < hdrlen) { kfree_skb(skb); return; }

    bool amsdu = false;
    if (ieee80211_is_data_qos(hdr->frame_control))
        amsdu = (skb->data[hdrlen - 2] & 0x80) != 0;  /* QoS-ctl A-MSDU bit */

    if (amsdu) {
        /* QoS A-MSDU: header [+ CCMP IV] then a chain of subframes.  rtw88
         * leaves the CCMP IV in the frame (mac80211 would normally strip it). */
        uint32_t off = hdrlen;
        if (ieee80211_has_protected(hdr->frame_control)) {
            if (skb->len < off + 8) { kfree_skb(skb); return; }
            off += 8;
        }
        deAmsdu(skb->data + off, skb->len - off);
        kfree_skb(skb);
        return;
    }

    /* Single MSDU.  Decrypted CCMP frames may still carry the 8-byte CCMP
     * header before the plaintext LLC/SNAP bytes. */
    uint32_t payload_off = hdrlen;
    if (skb->len < payload_off + 8) { kfree_skb(skb); return; }
    const uint8_t *llc = skb->data + payload_off;
    if ((llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) &&
        ieee80211_has_protected(hdr->frame_control) &&
        skb->len >= payload_off + 8 + 8) {
        const uint8_t *ccmp_llc = skb->data + payload_off + 8;
        if (ccmp_llc[0] == 0xAA && ccmp_llc[1] == 0xAA && ccmp_llc[2] == 0x03) {
            payload_off += 8;
            llc = ccmp_llc;
            if (!_rxCcmpIvSkipLogged) {
                IOLog("rtw88: rx protected data includes CCMP IV, skipping it\n");
                _rxCcmpIvSkipLogged = true;
            }
        }
    }

    /* LLC SNAP: AA AA 03 00 00 00 ETHERTYPE */
    uint16_t ethertype = 0;
    if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03) {
        ethertype = (uint16_t)((llc[6] << 8) | llc[7]);

        if (ethertype == ETH_P_PAE) {
            IOLog("rtw88: EAPOL RX external=%d state=%d len=%u\\n",
                  _externalSupplicant, _state,
                  skb->len - payload_off - 8);
        }

        /* Check for EAPOL during handshake (never aggregated). */
        if (ethertype == ETH_P_PAE && _state == RTW88_STATE_HANDSHAKING && !_externalSupplicant) {
            handleEAPOL(llc + 8, skb->len - payload_off - 8);
            kfree_skb(skb);
            return;
        }
    }

    /* DA = addr1 (recipient = us), SA = addr3 (original source via DS) */
    uint32_t paylen = skb->len - payload_off - 8; /* strip 802.11/CCMP/LLC */
    deliverEthernet(hdr->addr1, hdr->addr3, ethertype, llc + 8, paylen);
    kfree_skb(skb);
}

/* Build one Ethernet frame [da][sa][ethertype][payload] and inject it. */
void RTW88IEEE80211::deliverEthernet(const uint8_t *da, const uint8_t *sa,
                                     uint16_t ethertype,
                                     const uint8_t *payload, uint32_t paylen)
{
    /* Allocate via IONetworkController::allocatePacket (through the parent) —
     * NOT mbuf_allocpacket.  allocatePacket sets m_len/pkthdr.len consistently
     * for every segment, which the dlil input validator requires.
     * mbuf_copyback fills the data and is chain-safe. */
    if (!_parent) return;
    mbuf_t m = _parent->allocateInputPacket(14 + paylen);
    if (!m) return;

    uint8_t ehdr[14];
    memcpy(ehdr,     da, 6);
    memcpy(ehdr + 6, sa, 6);
    ehdr[12] = (uint8_t)(ethertype >> 8);
    ehdr[13] = (uint8_t)(ethertype & 0xff);

    if (mbuf_copyback(m, 0, 14, ehdr, MBUF_WAITOK) != 0 ||
        (paylen && mbuf_copyback(m, 14, paylen, payload, MBUF_WAITOK) != 0)) {
        mbuf_freem(m);
        return;
    }
    _parent->injectRxFrame(m);
}

/* Split an A-MSDU payload into its constituent MSDUs and deliver each. */
void RTW88IEEE80211::deAmsdu(const uint8_t *data, uint32_t len)
{
    uint32_t pos = 0;
    /* Subframe: DA(6) SA(6) Length(2, big-endian) | MSDU(Length) | pad to a
     * 4-byte boundary (the last subframe is not padded). */
    while (pos + 14 <= len) {
        const uint8_t *sf = data + pos;
        uint16_t sublen = (uint16_t)((sf[12] << 8) | sf[13]);
        if (sublen < 8 || pos + 14 + sublen > len)
            break;   /* truncated / malformed */
        const uint8_t *msdu = sf + 14;
        if (msdu[0] == 0xAA && msdu[1] == 0xAA && msdu[2] == 0x03) {
            uint16_t ethertype = (uint16_t)((msdu[6] << 8) | msdu[7]);
            deliverEthernet(sf, sf + 6, ethertype, msdu + 8, sublen - 8);
        }
        pos += (14u + sublen + 3u) & ~3u;   /* next subframe (4-byte aligned) */
    }
}

/* ------------------------------------------------------------------ */
/*  RX A-MPDU reorder buffer                                            */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::rxBaSetup(uint8_t tid, uint16_t ssn, uint16_t bufsize)
{
    if (tid >= kRxBaNumTid) return;
    if (bufsize == 0 || bufsize > kRxBaMaxBuf) bufsize = kRxBaMaxBuf;

    IOLockLock(_rxBaLock);
    RxReorder *r = _rxBa[tid];
    if (!r) {
        r = (RxReorder *)IOMallocZero(sizeof(RxReorder));
        _rxBa[tid] = r;
    } else {
        for (uint32_t i = 0; i < kRxBaMaxBuf; i++)
            if (r->buf[i]) { kfree_skb(r->buf[i]); r->buf[i] = nullptr; }
    }
    if (r) {
        r->active  = true;
        r->headSn  = (uint16_t)(ssn & 0xFFF);
        r->bufSize = bufsize;
        r->stored  = 0;
    }
    IOLockUnlock(_rxBaLock);
}

void RTW88IEEE80211::rxBaTeardown(uint8_t tid)
{
    if (tid >= kRxBaNumTid) return;
    struct sk_buff *freelist[kRxBaMaxBuf];
    uint32_t nf = 0;

    IOLockLock(_rxBaLock);
    RxReorder *r = _rxBa[tid];
    if (r) {
        for (uint32_t i = 0; i < kRxBaMaxBuf; i++)
            if (r->buf[i]) { freelist[nf++] = r->buf[i]; r->buf[i] = nullptr; }
        _rxBa[tid] = nullptr;
    }
    IOLockUnlock(_rxBaLock);

    if (r) {
        IOFree(r, sizeof(RxReorder));
        for (uint32_t i = 0; i < nf; i++)
            kfree_skb(freelist[i]);
    }
}

void RTW88IEEE80211::rxBaTeardownAll()
{
    for (uint8_t tid = 0; tid < kRxBaNumTid; tid++)
        rxBaTeardown(tid);
    /* Deliberately do NOT cancel _reorderTimer here: teardown can run on a
     * different workloop than the timer, and a stray fire is harmless (it finds
     * every TID inactive/empty and does nothing). */
}

void RTW88IEEE80211::rxReorderInput(uint8_t tid, struct sk_buff *skb, uint16_t sn)
{
    struct sk_buff *out[kRxBaMaxBuf];
    uint32_t nout = 0;
    bool needTimer = false;

    IOLockLock(_rxBaLock);
    RxReorder *r = _rxBa[tid];
    if (!r || !r->active) {            /* torn down between dispatch and here */
        IOLockUnlock(_rxBaLock);
        deliverDataFrame(skb);
        return;
    }

    uint16_t rel = (uint16_t)((sn - r->headSn) & 0xFFF);
    if (rel >= 2048) {                 /* before the window: stale / duplicate */
        IOLockUnlock(_rxBaLock);
        kfree_skb(skb);
        return;
    }

    if (rel >= r->bufSize) {
        /* sn is ahead of the window — slide the window up, releasing buffered
         * frames that fall out the bottom (in order; missing ones are lost). */
        uint16_t newHead = (uint16_t)((sn + 1 - r->bufSize) & 0xFFF);
        while (((newHead - r->headSn) & 0xFFF) != 0 &&
               ((newHead - r->headSn) & 0xFFF) < 2048 && nout < kRxBaMaxBuf) {
            uint16_t hidx = r->headSn % kRxBaMaxBuf;
            if (r->buf[hidx]) {
                out[nout++] = r->buf[hidx];
                r->buf[hidx] = nullptr;
                r->stored--;
            }
            r->headSn = (uint16_t)((r->headSn + 1) & 0xFFF);
        }
    }

    uint16_t idx = sn % kRxBaMaxBuf;
    if (r->buf[idx]) {                 /* duplicate within the window */
        kfree_skb(skb);
    } else {
        r->buf[idx] = skb;
        r->stored++;
    }

    /* Release the in-order run starting at head. */
    while (r->stored > 0 && nout < kRxBaMaxBuf) {
        uint16_t hidx = r->headSn % kRxBaMaxBuf;
        if (!r->buf[hidx]) break;
        out[nout++] = r->buf[hidx];
        r->buf[hidx] = nullptr;
        r->stored--;
        r->headSn = (uint16_t)((r->headSn + 1) & 0xFFF);
    }
    needTimer = (r->stored > 0);
    IOLockUnlock(_rxBaLock);

    for (uint32_t i = 0; i < nout; i++)
        deliverDataFrame(out[i]);

    if (needTimer)
        rxReorderArmTimer();
}

void RTW88IEEE80211::rxReorderArmTimer()
{
    /* Preserve the outstanding deadline. Re-arming on every incoming frame
     * can starve delivery indefinitely while a sequence-number hole exists.
     * Both callers and the callback execute on the RX workloop. */
    if (_reorderTimer && !_reorderTimerArmed) {
        _reorderTimerArmed = true;
        _reorderTimer->setTimeoutMS(kReorderTimeoutMs);
    }
}

/* Timer: a hole has persisted past the reorder timeout (the missing frame is
 * not coming).  Force progress by releasing past the first hole on each TID. */
void RTW88IEEE80211::rxReorderFlushStale()
{
    struct sk_buff *out[kRxBaMaxBuf];
    uint32_t nout = 0;
    bool again = false;

    IOLockLock(_rxBaLock);
    for (uint8_t tid = 0; tid < kRxBaNumTid; tid++) {
        RxReorder *r = _rxBa[tid];
        if (!r || !r->active || r->stored == 0) continue;

        /* Skip leading holes (lost frames), then release the next run. */
        uint32_t guard = 0;
        while (r->stored > 0 && guard < 4096) {
            uint16_t hidx = r->headSn % kRxBaMaxBuf;
            if (r->buf[hidx]) break;
            r->headSn = (uint16_t)((r->headSn + 1) & 0xFFF);
            guard++;
        }
        while (r->stored > 0 && nout < kRxBaMaxBuf) {
            uint16_t hidx = r->headSn % kRxBaMaxBuf;
            if (!r->buf[hidx]) break;
            out[nout++] = r->buf[hidx];
            r->buf[hidx] = nullptr;
            r->stored--;
            r->headSn = (uint16_t)((r->headSn + 1) & 0xFFF);
        }
        if (r->stored > 0) again = true;
    }
    IOLockUnlock(_rxBaLock);

    for (uint32_t i = 0; i < nout; i++)
        deliverDataFrame(out[i]);

    if (again)
        rxReorderArmTimer();
}

void RTW88IEEE80211::reorderTimerFired(OSObject *owner, IOTimerEventSource *)
{
    RTW88IEEE80211 *self = OSDynamicCast(RTW88IEEE80211, owner);
    if (self) {
        self->_reorderTimerArmed = false;
        self->rxReorderFlushStale();
    }
}

/* ------------------------------------------------------------------ */
/*  TX status                                                           */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::txStatus(struct sk_buff *skb)
{
    /* Nothing to do — skb freed by caller */
}

/*
 * Pick the operating channel width for the connection and program it into
 * hw->conf.chandef (consumed by rtw_set_channel via connect_hw_setup).
 *
 * We parse the AP's HT Operation (EID 61) and VHT Operation (EID 192) from the
 * cached beacon IEs to learn the BSS width, then cap to what the chip supports
 * (from its band caps).  Without this the link is pinned to 20 MHz — e.g. an
 * 80 MHz VHT AP gives 173 Mbps (20 MHz MCS8) instead of 866 Mbps (80 MHz MCS9).
 * TKIP links stay 20 MHz (HT disallowed — see htAllowed()).
 */
void RTW88IEEE80211::setConnectedChandef(struct ieee80211_channel *chan)
{
    _hw->conf.chandef.chan         = chan;
    _hw->conf.chandef.width        = NL80211_CHAN_WIDTH_20_NOHT;
    _hw->conf.chandef.center_freq1 = chan->center_freq;
    _connChanWidth = 20;

    if (!htAllowed())
        return;

    enum nl80211_band bnd =
        (_targetBSS.channel > 14) ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ;
    struct ieee80211_supported_band *sb =
        (_hw->wiphy) ? _hw->wiphy->bands[bnd] : nullptr;
    if (!sb) return;

    bool chip40 = (sb->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40) != 0;
    bool chip80 = chip40 && bnd == NL80211_BAND_5GHZ && sb->vht_cap.vht_supported;

    /* Parse HT/VHT Operation from the cached beacon IEs. */
    int     sco        = 0;      /* HT secondary-channel offset: 1=above 3=below */
    bool    htStaWidth = false;  /* HT "STA Channel Width" (40 MHz allowed)      */
    int     vhtWidth   = 0;      /* VHT op channel width: >=1 means 80 MHz        */
    uint8_t vhtSeg0    = 0;      /* VHT center-frequency segment 0 (center chan)  */
    const uint8_t *ies = _targetBSS.ies;
    uint16_t ielen     = _targetBSS.ies_len;
    for (uint16_t i = 0; i + 2 <= ielen; ) {
        uint8_t id = ies[i], len = ies[i + 1];
        if ((uint32_t)i + 2 + len > ielen) break;
        const uint8_t *d = ies + i + 2;
        if (id == WLAN_EID_HT_OPERATION && len >= 2) {
            sco        = d[1] & 0x03;
            htStaWidth = (d[1] & 0x04) != 0;
        } else if (id == WLAN_EID_VHT_OPERATION && len >= 3) {
            vhtWidth = d[0];
            vhtSeg0  = d[1];
        }
        i += 2 + len;
    }

    /* 40 MHz (HT). */
    if (chip40 && htStaWidth && (sco == 1 || sco == 3)) {
        _connChanWidth = 40;
        _hw->conf.chandef.width = NL80211_CHAN_WIDTH_40;
        _hw->conf.chandef.center_freq1 =
            (uint32_t)((int)chan->center_freq + (sco == 1 ? 10 : -10));
    }

    /* 80 MHz (VHT) — overrides 40 when the AP runs an 80 MHz BSS.  width==1 is
     * the 80/160/80+80 indicator; seg0 is the 80 MHz center either way, so we
     * use it and cap to 80 (the chip's max).  Deprecated width 2/3 are ignored. */
    if (chip80 && vhtWidth == 1 && vhtSeg0 != 0) {
        _connChanWidth = 80;
        _hw->conf.chandef.width = NL80211_CHAN_WIDTH_80;
        _hw->conf.chandef.center_freq1 = (uint32_t)(5000 + 5 * vhtSeg0);
    }

    /* Channel telemetry stays in IORegistry; avoid per-transition console spam. */
}

/* ------------------------------------------------------------------ */
/*  Scan                                                                */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::restoreConnectedChannel()
{
    if (_scanReturnState != RTW88_STATE_CONNECTED || !_hw || !_vif)
        return;

    struct ieee80211_channel *chan = nullptr;
    for (int b = 0; b < NL80211_NUM_BANDS && !chan; b++) {
        struct ieee80211_supported_band *band =
            (_hw->wiphy) ? _hw->wiphy->bands[b] : nullptr;
        if (!band) continue;
        for (int j = 0; j < band->n_channels; j++) {
            if (band->channels[j].hw_value == _targetBSS.channel) {
                chan = &band->channels[j];
                chan->band = band->band;
                break;
            }
        }
    }

    if (chan) {
        setConnectedChandef(chan);
    } else {
        IOLog("rtw88: scan restore: ch=%u not in band table\n",
              _targetBSS.channel);
    }

    rtw88_restore_connected_hw(_hw, _vif, _targetBSS.bssid);

    struct ieee80211_bss_conf *bss = &_vif->bss_conf;
    bss->bssid = bss->bssid_buf;
    memcpy(bss->bssid_buf, _targetBSS.bssid, ETH_ALEN);
    bss->assoc = true;
    bss->aid   = _assocAID;
    _vif->cfg.assoc = true;
    _vif->cfg.aid   = _assocAID;
}

bool RTW88IEEE80211::abortActiveScan(bool waitForIdle)
{
    if (_state != RTW88_STATE_SCANNING)
        return true;

    RTW88State returnState = _scanReturnState;
    if (_manualScanChannelCount) {
        _manualScanAbort = true;
    } else if (_hw && _hw->ops && _hw->ops->cancel_hw_scan) {
        _hw->ops->cancel_hw_scan(_hw, _vif);
    }

    if (!waitForIdle)
        return true;

    for (int i = 0; i < 40 && _state == RTW88_STATE_SCANNING; i++)
        IOSleep(50);

    if (_state == RTW88_STATE_SCANNING) {
        if (_manualScanChannelCount)
            return false;
        if (returnState == RTW88_STATE_CONNECTED)
            restoreConnectedChannel();
        _state = (returnState == RTW88_STATE_IDLE) ?
            RTW88_STATE_IDLE : returnState;
        _scanReturnState = RTW88_STATE_IDLE;
        _manualScanChannelCount = 0;
        _manualScanOnHomeChannel = false;
    }

    return _state != RTW88_STATE_SCANNING;
}

void RTW88IEEE80211::scanDone(bool aborted)
{
    // A late firmware completion after timeout must not finish a new join.
    if (_state != RTW88_STATE_SCANNING) return;
    if (_timer) _timer->cancelTimeout();
    if (!aborted) {
        IOLockLock(_bssLock);
        RTW88BSS **link = &_bssList;
        while (*link) {
            RTW88BSS *b = *link;
            RTW88State effectiveState =
                (_state == RTW88_STATE_SCANNING) ? _scanReturnState : _state;
            bool isTarget = (effectiveState == RTW88_STATE_CONNECTED ||
                             effectiveState == RTW88_STATE_HANDSHAKING) &&
                            memcmp(b->bssid, _targetBSS.bssid, 6) == 0;
            uint32_t age = _scanGeneration - b->last_seen_scan;

            if (!isTarget && age > 3) {
                *link = b->next;
                IOFree(b, sizeof(*b));
                if (_bssCount)
                    _bssCount--;
                continue;
            }

            link = &b->next;
        }
        IOLockUnlock(_bssLock);
    }

    if (_state == RTW88_STATE_SCANNING) {
        RTW88State returnState = _scanReturnState;
        /* Restore the infrastructure channel for every connected scan, not
         * only the manual fallback.  This keeps STA coherent if hw_scan is
         * available on another RTL88xx backend later. */
        if (returnState == RTW88_STATE_CONNECTED) {
            restoreConnectedChannel();
            txNullFunc(false);
        }
        _state = (returnState == RTW88_STATE_IDLE) ?
            RTW88_STATE_IDLE : returnState;
        _scanReturnState = RTW88_STATE_IDLE;
        _manualScanOnHomeChannel = false;

        /* A background CoreWiFi scan can leave an unassociated radio on its
         * final scan channel. If AWDL owns the idle PHY, return it to the AWDL
         * home/master channel before notifying IO80211 that scan completed. */
        if (returnState == RTW88_STATE_IDLE && _awdlReceiveMode && _awdlChannel) {
            IOReturn awdlRestore = setAWDLChannel(_awdlChannel);
            if (awdlRestore != kIOReturnSuccess)
                IOLog("rtw88: AWDL channel restore after scan failed 0x%x\n", awdlRestore);
        }
    }

    if (_powered && _awdlReceiveMode)
        (void)setAWDLReceiveMode(true); // scan completion reinstates BSSID filtering

    IOLockLock(_bssLock);
    _lastScanCacheCount = _bssCount;
    IOLockUnlock(_bssLock);

    /* Complete the Apple scan transaction first.  If ASSOCIATE arrived while
     * scanning, start that already-accepted join immediately afterwards. */
    if (_delegate) {
        UInt32 result = aborted ? 1 : 0;
        _delegate->rtw88Event(kRTW88EventScanDone, &result);
    }

    if (_deferredConnectPending && _state == RTW88_STATE_IDLE && _powered) {
        char ssid[sizeof(_deferredSSID)] = {};
        char password[sizeof(_deferredPassword)] = {};
        uint8_t pmk[sizeof(_deferredPMK)] = {};
        uint8_t bssid[sizeof(_deferredBSSID)] = {};
        strlcpy(ssid, _deferredSSID, sizeof(ssid));
        strlcpy(password, _deferredPassword, sizeof(password));
        memcpy(pmk, _deferredPMK, sizeof(pmk));
        memcpy(bssid, _deferredBSSID, sizeof(bssid));
        const bool pmkValid = _deferredPMKValid;
        const bool bssidValid = _deferredBSSIDValid;
        const bool externalSupplicant = _deferredExternalSupplicant;

        _deferredConnectPending = false;
        _deferredPMKValid = false;
        _deferredBSSIDValid = false;
        bzero(_deferredSSID, sizeof(_deferredSSID));
        bzero(_deferredPassword, sizeof(_deferredPassword));
        bzero(_deferredPMK, sizeof(_deferredPMK));
        bzero(_deferredBSSID, sizeof(_deferredBSSID));

        IOReturn joinRet = cmdConnect(ssid, password[0] ? password : nullptr,
                                      pmkValid ? pmk : nullptr,
                                      bssidValid ? bssid : nullptr,
                                      externalSupplicant);
        IOLog("rtw88: deferred ASSOCIATE after scan ret=0x%x ssid=%s\n",
              joinRet, ssid);
        bzero(pmk, sizeof(pmk));
        bzero(password, sizeof(password));
        if (joinRet != kIOReturnSuccess) failJoin("deferred-connect");
    }
}

IOReturn RTW88IEEE80211::cmdScan()
{
    _awdlPreparedChannel = 0;
    /* Physical scans are only safe while unassociated until the manual
     * scanner can honor CoreWiFi's requested channel subset / availability
     * windows. AirportRTW88 handles CONNECTED background scans cache-only. */
    if (_state != RTW88_STATE_IDLE) {
        IOLog("rtw88: physical scan busy state=%u\n", (unsigned)_state);
        return kIOReturnBusy;
    }
    if (!_powered || !_hw || !_hw->ops) return kIOReturnNotReady;
    RTW88State returnState = _state;
    IOLog("rtw88: scan start returnState=%u connected=%u\n",
          (unsigned)returnState, returnState == RTW88_STATE_CONNECTED ? 1U : 0U);

    IOLockLock(_bssLock);
    _scanGeneration++;
    if (_scanGeneration == 0) {
        _scanGeneration = 1;
        for (RTW88BSS *b = _bssList; b; b = b->next)
            b->last_seen_scan = 1;
    }
    IOLockUnlock(_bssLock);

    _scanReturnState = (returnState == RTW88_STATE_CONNECTED) ?
        returnState : RTW88_STATE_IDLE;
    _state = RTW88_STATE_SCANNING;

    // Firmware retains the request until completion; neither object may live on the stack.
    if (!_scanRequest) _scanRequest = (ieee80211_scan_request *)IOMallocZero(sizeof(*_scanRequest));
    if (!_scanRequest) { _state = returnState; _scanReturnState = RTW88_STATE_IDLE; return kIOReturnNoMemory; }
    bzero(_scanRequest, sizeof(*_scanRequest));
    auto &req = *_scanRequest;
    auto *chans = _scanRequestChannels;
    int n_chans = 0;
    
    if (_hw->wiphy) {
        for (int i = 0; i < NL80211_NUM_BANDS; i++) {
            struct ieee80211_supported_band *band = _hw->wiphy->bands[i];
            if (!band) continue;
            for (int j = 0; j < band->n_channels; j++) {
                if (n_chans < 256) {
                    /* Only scan enabled channels */
                    if (!(band->channels[j].flags & IEEE80211_CHAN_DISABLED)) {
                        band->channels[j].band = band->band;
                        chans[n_chans++] = &band->channels[j];
                    }
                }
            }
        }
    }

    if (n_chans == 0) {
        IOLog("rtw88: scan has no enabled channels\n");
        _state = returnState;
        _scanReturnState = RTW88_STATE_IDLE;
        return kIOReturnNotReady;
    }

    const bool firmwareScan = _hw->ops->hw_scan && rtw88_hw_scan_supported(_hw);
    _scanEngine = firmwareScan ? "firmware" : "software";
    _lastScanChannelCount = (uint32_t)n_chans;
    _scanVisited2GHz = _scanVisited5GHz = 0;
    if (!firmwareScan) {
        if (!_manualScanTC) {
            _state = returnState;
            _scanReturnState = RTW88_STATE_IDLE;
            return kIOReturnNotReady;
        }

        _manualScanChannelCount = (uint32_t)n_chans;
        for (int i = 0; i < n_chans; i++)
            _manualScanChannels[i] = chans[i];
        _manualScanAbort = false;
        _manualScanOnHomeChannel = false;
        _rxFrameCount = 0;

        if (!_manualScanFallbackLogged) {
            IOLog("rtw88: scan offload unavailable, using passive channel scan (%d channels)\n",
                  n_chans);
            _manualScanFallbackLogged = true;
        }
        thread_call_enter(_manualScanTC);

        _timeoutMs = (_scanReturnState == RTW88_STATE_CONNECTED) ?
            30000 : 12000;
        uint64_t d;
        clock_interval_to_deadline(_timeoutMs, kMillisecondScale, &d);
        _timer->wakeAtTime(d);
        return kIOReturnSuccess;
    }
    
    req.req.channels = chans;
    req.req.n_channels = n_chans;

    _rxFrameCount = 0;  /* reset diagnostic counter at scan start */

    int hw_scan_ret = _hw->ops->hw_scan(_hw, _vif, &req);
    if (hw_scan_ret != 0) {
        IOLog("rtw88: hw_scan returned %d -- falling back to passive scan\n",
              hw_scan_ret);
        if (_manualScanTC) {
            _scanEngine = "software-after-fw-error";
            _manualScanChannelCount = (uint32_t)n_chans;
            for (int i = 0; i < n_chans; i++)
                _manualScanChannels[i] = chans[i];
            _manualScanAbort = false;
            _manualScanOnHomeChannel = false;
            _rxFrameCount = 0;
            thread_call_enter(_manualScanTC);

            _timeoutMs = (_scanReturnState == RTW88_STATE_CONNECTED) ?
                30000 : 12000;
            uint64_t d;
            clock_interval_to_deadline(_timeoutMs, kMillisecondScale, &d);
            _timer->wakeAtTime(d);
            return kIOReturnSuccess;
        }
        _state = returnState;
        _scanReturnState = RTW88_STATE_IDLE;
        return kIOReturnError;
    }
    /* Timeout: if scan doesn't complete in 10s */
    _timeoutMs = 10000;
    uint64_t d; clock_interval_to_deadline(_timeoutMs, kMillisecondScale, &d); _timer->wakeAtTime(d);
    return kIOReturnSuccess;
}

void RTW88IEEE80211::manualScanTCFn(thread_call_param_t self, thread_call_param_t)
{
    ((RTW88IEEE80211 *)self)->runManualScan();
}

void RTW88IEEE80211::runManualScan()
{
    if (!_hw || !_vif) {
        scanDone(true);
        _manualScanChannelCount = 0;
        return;
    }

    uint32_t count = _manualScanChannelCount;
    if (count > 256)
        count = 256;
    bool connectedScan = (_scanReturnState == RTW88_STATE_CONNECTED);

    rtw88_sw_scan_start(_hw, _vif);

    for (uint32_t i = 0; i < count && !_manualScanAbort; i++) {
        struct ieee80211_channel *chan = _manualScanChannels[i];
        if (!chan)
            continue;

        _manualScanOnHomeChannel = false;

        if (connectedScan) {
            restoreConnectedChannel();
            txNullFunc(true);
            IOSleep(10);
        }

        _hw->conf.chandef.chan = chan;
        _hw->conf.chandef.width = NL80211_CHAN_WIDTH_20_NOHT;
        _hw->conf.chandef.center_freq1 = chan->center_freq;

        rtw88_sw_scan_switch_channel(_hw);

        bool passiveOnly = (chan->flags &
            (IEEE80211_CHAN_NO_IR | IEEE80211_CHAN_RADAR)) != 0;
        if (!passiveOnly)
            txProbeRequest();

        /* Active channels can use a short probe dwell. Passive/DFS channels
         * still need a beacon-listen dwell. */
        IOSleep(passiveOnly ? 140 : 70);
        /* Software dwell completed; this is not proof of successful RF RX. */
        if (chan->band == NL80211_BAND_5GHZ) ++_scanVisited5GHz;
        else if (chan->band == NL80211_BAND_2GHZ) ++_scanVisited2GHz;

        if (connectedScan && !_manualScanAbort) {
            restoreConnectedChannel();
            txNullFunc(false);
            _manualScanOnHomeChannel = true;
            IOSleep(80);
        }
    }

    _manualScanOnHomeChannel = false;
    rtw88_sw_scan_complete(_hw, _vif);
    /* scanDone restores/calibrates the home channel and clears AP power-save
     * once, including an aborted scan, before notifying IO80211. */
    scanDone(_manualScanAbort);
    _manualScanAbort = false;
    _manualScanChannelCount = 0;
}

/* ------------------------------------------------------------------ */
/*  Connect                                                             */
/* ------------------------------------------------------------------ */

bool RTW88IEEE80211::matchesPendingJoin(const char *ssid, const uint8_t *bssid,
                                            const uint8_t *pmk) const
{
    return ssid && strcmp(ssid, _targetBSS.ssid) == 0 &&
        (!bssid || memcmp(bssid, _targetBSS.bssid, 6) == 0) &&
        (pmk != nullptr) == _pmkProvided &&
        (!pmk || memcmp(pmk, _pmk, sizeof(_pmk)) == 0);
}

/* May run on the authentication worker: never drain that worker here and
 * never touch RF registers when scan firmware failed to become idle. */
void RTW88IEEE80211::failJoin(const char *stage)
{
    IOLog("rtw88: join failed stage=%s attempts=%u\n", stage, _authAttempts);
    __atomic_store_n(&_connectCancelled, true, __ATOMIC_RELEASE);
    if (_timer) _timer->cancelTimeout();
    _associatedVisible = false;
    _state = RTW88_STATE_IDLE;
    if (_parent) _parent->setLinkStatus(kIONetworkLinkValid);
    if (_delegate) _delegate->rtw88Event(kRTW88EventDisconnected, nullptr);
}

IOReturn RTW88IEEE80211::cmdConnect(const char *ssid, const char *password, const uint8_t *pmk,
                                      const uint8_t *bssid, bool externalSupplicant)
{
    if (!ssid || !ssid[0] || strnlen(ssid, 33) > 32) return kIOReturnBadArgument;

    /* AirportItlwm accepts ASSOCIATE while net80211 is in SCAN.  Returning
     * EBUSY here made CoreWiFi abort the join and issue DISASSOCIATE while our
     * raw state was still SCANNING. Preserve the request and return success
     * immediately; scanDone() continues
     * the join into AUTH. */
    if (_state == RTW88_STATE_SCANNING) {
        strlcpy(_deferredSSID, ssid, sizeof(_deferredSSID));
        strlcpy(_deferredPassword, password ? password : "", sizeof(_deferredPassword));
        _deferredPMKValid = pmk != nullptr;
        if (pmk) memcpy(_deferredPMK, pmk, sizeof(_deferredPMK));
        else bzero(_deferredPMK, sizeof(_deferredPMK));
        _deferredBSSIDValid = bssid != nullptr;
        if (bssid) memcpy(_deferredBSSID, bssid, sizeof(_deferredBSSID));
        else bzero(_deferredBSSID, sizeof(_deferredBSSID));
        _deferredExternalSupplicant = externalSupplicant;
        _deferredConnectPending = true;

        if (_manualScanChannelCount) {
            /* Do not let autojoin to an early 2.4 GHz result truncate the
             * initial software scan before the later 5 GHz channels. The
             * existing scan deadline still bounds this deferred join.
             * Preserve cancellation for a scan returning to a live STA. */
            if (_scanReturnState != RTW88_STATE_IDLE)
                _manualScanAbort = true;
        }
        else if (_hw && _hw->ops && _hw->ops->cancel_hw_scan)
            _hw->ops->cancel_hw_scan(_hw, _vif);

        IOLog("rtw88: ASSOCIATE deferred until active scan completes ssid=%s\n", ssid);
        return kIOReturnSuccess;
    }

    if (_state != RTW88_STATE_IDLE) return kIOReturnBusy;
    if (!_powered || !_connectTC) return kIOReturnNotReady;
    clearKeys();
    releaseSta();
    /* A new join supersedes any previous externally-visible RUN state. */
    _associatedVisible = false;

    /* Find the SSID in our BSS list */
    IOLockLock(_bssLock);
    RTW88BSS *target = nullptr;
    const size_t wantedLen = strnlen(ssid, sizeof(_targetBSS.ssid));
    for (RTW88BSS *b = _bssList; b; b = b->next) {
        if (b->ssid_len == wantedLen &&
            memcmp(b->ssid, ssid, wantedLen) == 0 &&
            (!bssid || memcmp(b->bssid, bssid, 6) == 0)) {
            target = b;
            break;
        }
    }
    if (!target) {
        IOLockUnlock(_bssLock);
        return kIOReturnNotFound;
    }
    memcpy(&_targetBSS, target, sizeof(_targetBSS));
    _rssi = target->rssi < 0 && target->rssi >= -127 ? target->rssi : -100;
    IOLockUnlock(_bssLock);

    /* AirportItlwm drives association from the Apple80211 AUTH/RSN request,
     * not solely from security metadata recovered during scan.  Do the same:
     * if Apple's association RSN IE selects CCMP-PSK, make that authoritative
     * for this attempt.  This prevents a perfectly valid WPA2 network from
     * being rejected just because our lightweight beacon parser missed or
     * simplified its AKM/cipher list. */
    if ((externalSupplicant || pmk || (password && password[0])) && _assocRsnIELen >= 4 &&
        _assocRsnIE[0] == WLAN_EID_RSN &&
        (uint16_t)(_assocRsnIE[1] + 2) <= _assocRsnIELen) {
        uint32_t pairwise = 0, group = 0;
        if (rtw88RsnSelectCcmpPsk(_assocRsnIE + 2, _assocRsnIE[1],
                                  &pairwise, &group)) {
            _targetBSS.cipher = pairwise;
            _targetBSS.group_cipher = group;
            _targetBSS.akm = 0x000FAC02;
        }
    }

    if (externalSupplicant) {
        /* Current Apple-RSN backend supports WPA2/CCMP PSK.  Reject only after
         * considering the association RSN IE, rather than trusting scan-only
         * metadata. */
        if (_targetBSS.cipher != WLAN_CIPHER_SUITE_CCMP ||
            _targetBSS.akm != 0x000FAC02) {
            IOLog("rtw88: connect: requested protected BSS is not CCMP-PSK "
                  "cipher=0x%x akm=0x%x rsnLen=%u\n",
                  _targetBSS.cipher, _targetBSS.akm, _assocRsnIELen);
            return kIOReturnUnsupported;
        }
    } else if ((_targetBSS.capabilities & 0x10) && !pmk && !password) {
        return kIOReturnUnsupported;
    }

    _authAttempts = 0;
    _externalSupplicant = externalSupplicant;
    _pmkProvided = pmk != nullptr;
    if (pmk) memcpy(_pmk, pmk, sizeof(_pmk));
    strlcpy(_password, password ? password : "", sizeof(_password));
    _wpa2 = externalSupplicant || (_targetBSS.cipher == WLAN_CIPHER_SUITE_CCMP);
    _deauthReason = 0;
    __atomic_store_n(&_connectCancelled, false, __ATOMIC_RELEASE);
    _state = RTW88_STATE_AUTHENTICATING;

    /* Run doAuthenticate on a background thread_call so the IOUserClient
     * call returns immediately.  The connect machinery (channel change,
     * mutex acquisition, TX) must not block the MIG thread. */
    if (_connectTC)
        thread_call_enter(_connectTC);
    return kIOReturnSuccess;
}

void RTW88IEEE80211::connectTCFn(thread_call_param_t self, thread_call_param_t)
{
    ((RTW88IEEE80211 *)self)->doAuthenticate();
}

void RTW88IEEE80211::doAuthenticate()
{
    _awdlPreparedChannel = 0;
    if (authenticationCancelled() || !_powered || !_hw || !_vif) return;
    if (++_authAttempts > 3) { failJoin("auth-retry-limit"); return; }

    IOLog("rtw88: doAuthenticate entry — BSSID %02x:%02x:%02x:%02x:%02x:%02x ch=%u\n",
          _targetBSS.bssid[0], _targetBSS.bssid[1], _targetBSS.bssid[2],
          _targetBSS.bssid[3], _targetBSS.bssid[4], _targetBSS.bssid[5],
          _targetBSS.channel);

    /* Wait for RTW_FLAG_SCANNING to clear.
     * The flag is cleared inside rtw_core_scan_complete() which runs under
     * rtwdev->mutex in the c2h_work thread. */
    for (int i = 0; i < 100; i++) {
        if (authenticationCancelled()) return;
        if (!rtw88_is_scanning()) break;
        IOSleep(50);
    }
    if (authenticationCancelled()) return;
    if (rtw88_is_scanning()) { failJoin("scan-not-idle"); return; }
    IOLog("rtw88: doAuthenticate: scan flag clear\n");

    /* Firmware settle delay.
     *
     * After HW scan the firmware needs ~200-500 ms to fully exit its
     * internal scan-mode critical section before it can safely process
     * channel-switch register writes.  On Linux/FreeBSD this gap is filled
     * naturally by the wpa_supplicant userspace round-trip; we must add it
     * explicitly.  Without this, rtw_set_channel's BB/RF MMIO reads hit the
     * chip while the firmware is still transitioning → PCIe bus hang →
     * system freeze.  500 ms is comfortably below the watch-dog LPS timer
     * (~2 s), so the chip stays awake. */
    IOSleep(500);
    if (authenticationCancelled()) return;
    IOLog("rtw88: doAuthenticate: firmware settled\n");

    /* ----- 1. Channel switch + BSSID (single mutex section) ----- *
     *
     * We call rtw88_connect_hw_setup() instead of ops->config +
     * ops->bss_info_changed because both of those call rtw_leave_lps_deep()
     * → __rtw_fw_leave_lps_check_reg() → polling MMIO reads on REG_TCR.
     * If the chip is slow to respond, those reads stall the calling CPU core
     * indefinitely (PCIe timeout → system freeze).
     *
     * rtw88_connect_hw_setup() holds rtwdev->mutex, calls rtw_set_channel()
     * (MMIO writes) and rtw_vif_port_config(PORT_SET_BSSID) — no reads that
     * can stall, and no LPS wake sequence. */
    struct ieee80211_channel *chan = nullptr;
    for (int b = 0; b < NL80211_NUM_BANDS && !chan; b++) {
        struct ieee80211_supported_band *band =
            (_hw->wiphy) ? _hw->wiphy->bands[b] : nullptr;
        if (!band) continue;
        for (int j = 0; j < band->n_channels; j++) {
            if (band->channels[j].hw_value == _targetBSS.channel) {
                chan = &band->channels[j];
                /* The channel tables omit the per-channel .band field (Linux
                 * fills it in at wiphy_register, which we don't run), so 5GHz
                 * channels otherwise report band 0 == 2GHz.  Backfill it from
                 * the parent band so chandef.chan->band and rx_status->band
                 * are correct. */
                chan->band = band->band;
                break;
            }
        }
    }
    if (chan) {
        setConnectedChandef(chan);
        IOLog("rtw88: doAuthenticate: calling connect_hw_setup ch=%u\n",
              _targetBSS.channel);
        rtw88_connect_hw_setup(_hw, _vif, _targetBSS.bssid);
        IOLog("rtw88: doAuthenticate: connect_hw_setup done\n");
    } else {
        failJoin("channel-not-found");
        return;
    }

    if (authenticationCancelled()) return;

    /* Also update the vif bss_conf bssid so any driver-internal code
     * that reads it sees the right value. */
    struct ieee80211_bss_conf *bss = &_vif->bss_conf;
    bss->bssid = bss->bssid_buf;
    memcpy(bss->bssid_buf, _targetBSS.bssid, 6);
    bss->assoc = false;
    bss->aid   = 0;
    _vif->cfg.assoc = false;
    _vif->cfg.aid = 0;
    _assocAID = 0;

    /* ----- 2. Send Authentication frame ----- */
    IOLog("rtw88: doAuthenticate: building auth frame\n");
    uint8_t auth[30] = {};
    uint32_t authlen = 0;
    buildAuthReq(auth, &authlen);
    IOLog("rtw88: doAuthenticate: transmitting auth frame (%u bytes)\n", authlen);
    if (authenticationCancelled()) return;
    if (!txMgmtFrame(auth, authlen)) { failJoin("auth-tx"); return; }
    IOLog("rtw88: doAuthenticate: auth frame sent — waiting for response\n");

    // Do not overwrite a response-driven state transition after transmitting.
    if (authenticationCancelled() || _state != RTW88_STATE_AUTHENTICATING) return;
    uint64_t d; clock_interval_to_deadline(3000, kMillisecondScale, &d);
    _timer->wakeAtTime(d);
}

void RTW88IEEE80211::doAssociate()
{
    if (!_hw || !_vif) return;

    uint8_t assoc[256] = {};
    uint32_t assoclen  = 0;
    if (authenticationCancelled()) return;
    if (!buildAssocReq(assoc, &assoclen)) { failJoin("assoc-build"); return; }
    // Publish the state before TX: a fast response may arrive during submission.
    _state = RTW88_STATE_ASSOCIATING;
    if (!txMgmtFrame(assoc, assoclen)) {
        failJoin("assoc-tx");
        return;
    }
    if (authenticationCancelled() || _state != RTW88_STATE_ASSOCIATING) return;
    uint64_t d; clock_interval_to_deadline(3000, kMillisecondScale, &d); _timer->wakeAtTime(d);
}

void RTW88IEEE80211::processAssocResponse(struct sk_buff *skb)
{
    uint16_t status = 0, aid = 0;
    bool valid = rtw88AssocResponse(skb->data, skb->len, _macAddr,
                                    _targetBSS.bssid, &status, &aid);
    kfree_skb(skb);
    if (!valid || authenticationCancelled()) return;

    if (status != 0) {
        IOLog("rtw88: assoc failed status=%u\n", status);
        _associatedVisible = false;
        _state = RTW88_STATE_IDLE;
        return;
    }
    IOLog("rtw88: associated! AID=%u\n", aid);
    _assocAID = aid;
    /* From this point until a real deauth/disconnect, IO80211 must observe
     * the infrastructure interface as RUN.  AirportItlwm gets this persistence
     * from net80211's IEEE80211_S_RUN state; keep an explicit latch because
     * rtw88 has additional transient implementation states. */
    _associatedVisible = true;

    /* ----- 1. Allocate and register peer STA ----- */
    if (_sta == nullptr && _hw->ops && _hw->ops->sta_add) {
        size_t sta_sz = sizeof(struct ieee80211_sta) + _hw->sta_data_size;
        _sta = (struct ieee80211_sta *)IOMallocZero(sta_sz);
        if (_sta) {
            _staAllocSize = sta_sz;
            memcpy(_sta->addr, _targetBSS.bssid, ETH_ALEN);
            _sta->aid  = aid;
            _sta->wme  = true;

            /* Populate supported rates so rtw_update_sta_info() builds a
             * non-empty rate-adaptation mask. */
            _sta->deflink.supp_rates[NL80211_BAND_2GHZ] = 0xFFF; /* CCK+OFDM */
            _sta->deflink.supp_rates[NL80211_BAND_5GHZ] = 0xFF;  /* OFDM     */
            _sta->deflink.bandwidth =
                (_connChanWidth == 80) ? IEEE80211_STA_RX_BW_80 :
                (_connChanWidth == 40) ? IEEE80211_STA_RX_BW_40 :
                                         IEEE80211_STA_RX_BW_20;

            /* Mirror the chip's HT/VHT capabilities onto the peer STA so
             * rtw_update_sta_info() (invoked by sta_add) builds a firmware
             * rate-adaptation mask that includes HT/VHT MCS rates instead of
             * legacy-only.  This MUST match what buildAssocReq() advertised to
             * the AP.  Operation is held to 20 MHz (clear the 40 MHz HT bits)
             * to match the 20 MHz PHY. */
            enum nl80211_band sta_band =
                (_targetBSS.channel > 14) ? NL80211_BAND_5GHZ
                                          : NL80211_BAND_2GHZ;
            struct ieee80211_supported_band *sband =
                (_hw->wiphy) ? _hw->wiphy->bands[sta_band] : nullptr;
            if (htAllowed() && sband) {
                _sta->deflink.ht_cap = sband->ht_cap;
                if (_connChanWidth < 40)
                    _sta->deflink.ht_cap.cap &=
                        ~(uint16_t)(IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
                                    IEEE80211_HT_CAP_SGI_40 |
                                    IEEE80211_HT_CAP_DSSSCCK40);
                if (sta_band == NL80211_BAND_5GHZ)
                    _sta->deflink.vht_cap = sband->vht_cap;
            }

            _hw->ops->sta_add(_hw, _vif, _sta);
        }
    }

    /* ----- 2. Notify driver of full association ----- */
    if (_hw->ops && _hw->ops->bss_info_changed) {
        struct ieee80211_bss_conf *bss = &_vif->bss_conf;
        bss->assoc = true;
        bss->aid   = aid;
        bss->qos   = true;
        bss->bssid = bss->bssid_buf;
        memcpy(bss->bssid_buf, _targetBSS.bssid, ETH_ALEN);
        _vif->cfg.assoc = true;
        _vif->cfg.aid   = aid;
        _hw->ops->bss_info_changed(_hw, _vif, bss,
            BSS_CHANGED_ASSOC | BSS_CHANGED_QOS);
    }

    if (_wpa2 && !_externalSupplicant) {
        _state = RTW88_STATE_HANDSHAKING;
        IOLog("rtw88: WPA2 — waiting for EAPOL M1\n");
        /* Derive PMK from passphrase now */
        if (!_pmkProvided) derivePMK((uint8_t *)_password, (uint8_t *)_targetBSS.ssid,
                  _targetBSS.ssid_len, _pmk);
        uint64_t d;
        clock_interval_to_deadline(8000, kMillisecondScale, &d);
        _timer->wakeAtTime(d);
    } else {
        /* Apple RSN Supplicant needs the station in RUN so its EAPOL socket can
         * receive the AP's handshake frames.  The link becomes usable only after
         * CIPHER_KEY installs the PTK/GTK, but exposing RUN here mirrors
         * AirportItlwm's net80211 flow. */
        _state = RTW88_STATE_CONNECTED;
        /* Our Apple-RSN path injects EAPOL through the normal BSD Ethernet
         * interface, unlike AirportItlwm's net80211 path.  The interface must
         * therefore be LINK_UP for EAPOLController to receive/send 0x888e.
         * The TX gate below still drops ordinary traffic until PTK+GTK exist,
         * so exposing LINK_UP here does not leak unprotected IP traffic.  This
         * restores the association state that previously reached EAPOL on the
         * RTL8822BE while keeping controlled-port semantics in our own path. */
        if (_parent)
            _parent->setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid);
        if (!_wpa2)
            startTxAggregation();
        _timer->cancelTimeout();
        if (_externalSupplicant)
            IOLog("rtw88: associated — Apple RSN active; waiting for PTK/GTK before link-up\n");
    }

    /* External Apple-RSN mode needs ASSOC_DONE at raw 802.11 association so
     * EAPOLController can take over.  The proven RTL8822BE path uses the
     * internal rtw88 supplicant instead: there CoreWiFi is notified from
     * AirportRTW88::setLinkStatus() only after the controlled port is usable
     * (open immediately, WPA2 after the 4-way handshake). */
    if (_externalSupplicant && _delegate)
        _delegate->rtw88Event(kRTW88EventAssocDone, nullptr);
}

bool RTW88IEEE80211::buildAuthReq(uint8_t *buf, uint32_t *len)
{
    /* 802.11 Authentication frame (open system, seq 1) */
    struct ieee80211_hdr_3addr *hdr = (struct ieee80211_hdr_3addr *)buf;
    hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_AUTH);
    hdr->duration_id   = 0;
    memcpy(hdr->addr1, _targetBSS.bssid, 6);
    memcpy(hdr->addr2, _macAddr, 6);
    memcpy(hdr->addr3, _targetBSS.bssid, 6);
    hdr->seq_ctrl = cpu_to_le16((uint16_t)(_txSeq++ << 4));

    uint8_t *body = buf + sizeof(*hdr);
    /* Algorithm: 0 (Open), Transaction: 1, Status: 0 */
    body[0] = 0; body[1] = 0; /* algorithm */
    body[2] = 1; body[3] = 0; /* transaction seq */
    body[4] = 0; body[5] = 0; /* status code */
    *len = sizeof(*hdr) + 6;
    return true;
}

bool RTW88IEEE80211::buildAssocReq(uint8_t *buf, uint32_t *len)
{
    struct ieee80211_hdr_3addr *hdr = (struct ieee80211_hdr_3addr *)buf;
    hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ASSOC_REQ);
    hdr->duration_id   = 0;
    memcpy(hdr->addr1, _targetBSS.bssid, 6);
    memcpy(hdr->addr2, _macAddr, 6);
    memcpy(hdr->addr3, _targetBSS.bssid, 6);
    hdr->seq_ctrl = cpu_to_le16((uint16_t)(_txSeq++ << 4));

    uint8_t *body = buf + sizeof(*hdr);
    /* Capability: ESS, Short Preamble, and Privacy for encrypted APs. */
    uint16_t cap = 0x0431;
    if (_wpa2)
        cap |= 0x0010;
    body[0] = (uint8_t)(cap & 0xff);
    body[1] = (uint8_t)(cap >> 8);
    /* Listen interval: 10 */
    body[2] = 10; body[3] = 0;
    body += 4;

    /* SSID IE */
    body[0] = WLAN_EID_SSID;
    body[1] = (uint8_t)_targetBSS.ssid_len;
    memcpy(body + 2, _targetBSS.ssid, _targetBSS.ssid_len);
    body += 2 + _targetBSS.ssid_len;

    /* Supported rates. */
    static const uint8_t rates_2g[] = {
        0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24
    };
    static const uint8_t rates_5g[] = {
        0x8c, 0x12, 0x98, 0x24, 0x30, 0x48, 0x60, 0x6c
    };
    static const uint8_t ext_rates_2g[] = {
        0x30, 0x48, 0x60, 0x6c
    };
    const uint8_t *rates =
        (_targetBSS.channel > 14) ? rates_5g : rates_2g;
    uint8_t rates_len =
        (_targetBSS.channel > 14) ? sizeof(rates_5g) : sizeof(rates_2g);

    body[0] = WLAN_EID_SUPP_RATES;
    body[1] = rates_len;
    memcpy(body + 2, rates, rates_len);
    body += 2 + rates_len;

    if (_targetBSS.channel <= 14) {
        body[0] = WLAN_EID_EXT_SUPP_RATES;
        body[1] = sizeof(ext_rates_2g);
        memcpy(body + 2, ext_rates_2g, sizeof(ext_rates_2g));
        body += 2 + sizeof(ext_rates_2g);
    }

    /*
     * HT/VHT Capabilities — advertise 802.11n/ac so the AP associates us as a
     * high-throughput station (high MCS rates) and is willing to set up A-MPDU
     * BlockAck aggregation.  Without these IEs the AP treats us as legacy a/g
     * (<=54 Mbps, no aggregation), which is the root cause of the ~13 Mbps cap.
     *
     * We copy directly from the chip's own band capabilities (filled by
     * rtw_init_ht_cap/rtw_init_vht_cap during rtw_register_hw) so we never
     * claim more than the hardware supports.  The 40/80 MHz channel-width bits
     * are cleared because the PHY stays on a 20 MHz channel this pass — wider
     * operation needs HT/VHT Operation parsing + a re-tune (future work).
     */
    enum nl80211_band band =
        (_targetBSS.channel > 14) ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ;
    struct ieee80211_supported_band *sband =
        (_hw && _hw->wiphy) ? _hw->wiphy->bands[band] : nullptr;

    if (htAllowed() && sband && sband->ht_cap.ht_supported) {
        const struct ieee80211_sta_ht_cap *ht = &sband->ht_cap;
        body[0] = WLAN_EID_HT_CAPABILITY;
        body[1] = 26;
        uint8_t *p = body + 2;
        /* HT Capabilities Info (2 bytes, LE).  Advertise 40 MHz only when we
         * actually operate at >=40 MHz; otherwise clear the width bits to keep
         * the AP at 20 MHz. */
        uint16_t htcap = ht->cap;
        if (_connChanWidth < 40)
            htcap &= ~(uint16_t)(IEEE80211_HT_CAP_SUP_WIDTH_20_40 |
                                 IEEE80211_HT_CAP_SGI_40 |
                                 IEEE80211_HT_CAP_DSSSCCK40);
        p[0] = (uint8_t)(htcap & 0xff);
        p[1] = (uint8_t)(htcap >> 8);
        /* A-MPDU Parameters: max-length exponent (bits 1:0) | density (4:2). */
        p[2] = (uint8_t)((ht->ampdu_factor & 0x3) |
                         ((ht->ampdu_density & 0x7) << 2));
        /* Supported MCS Set (16 bytes): rx_mask[10], rx_highest(2),
         * tx_params(1), 3 reserved. */
        memcpy(p + 3, ht->mcs.rx_mask, 10);
        p[13] = (uint8_t)(ht->mcs.rx_highest & 0xff);
        p[14] = (uint8_t)((ht->mcs.rx_highest >> 8) & 0xff);
        p[15] = (uint8_t)(ht->mcs.tx_params & 0xff);
        /* p[16..25]: MCS-set reserved (3) + HT-ext (2) + TxBF (4) + ASEL (1). */
        memset(p + 16, 0, 10);
        body += 2 + 26;
    }

    if (htAllowed() && band == NL80211_BAND_5GHZ &&
        sband && sband->vht_cap.vht_supported) {
        const struct ieee80211_sta_vht_cap *vht = &sband->vht_cap;
        body[0] = WLAN_EID_VHT_CAPABILITY;
        body[1] = 12;
        uint8_t *p = body + 2;
        /* VHT Capabilities Info (4 bytes, LE).  The Supported Channel Width
         * Set subfield (bits 2:3) stays 0 (<=80 MHz capable); actual operation
         * is held to 20 MHz by the HT cap above. */
        uint32_t vcap = vht->cap;
        p[0] = (uint8_t)(vcap & 0xff);
        p[1] = (uint8_t)((vcap >> 8) & 0xff);
        p[2] = (uint8_t)((vcap >> 16) & 0xff);
        p[3] = (uint8_t)((vcap >> 24) & 0xff);
        /* Supported VHT-MCS and NSS Set (8 bytes). */
        uint16_t rxmap = (uint16_t)le32_to_cpu(vht->vht_mcs.rx_mcs_map);
        uint16_t rxhi  = le16_to_cpu(vht->vht_mcs.rx_highest);
        uint16_t txmap = (uint16_t)le32_to_cpu(vht->vht_mcs.tx_mcs_map);
        uint16_t txhi  = le16_to_cpu(vht->vht_mcs.tx_highest);
        p[4] = (uint8_t)(rxmap & 0xff); p[5] = (uint8_t)(rxmap >> 8);
        p[6] = (uint8_t)(rxhi & 0xff);  p[7] = (uint8_t)(rxhi >> 8);
        p[8] = (uint8_t)(txmap & 0xff); p[9] = (uint8_t)(txmap >> 8);
        p[10] = (uint8_t)(txhi & 0xff); p[11] = (uint8_t)(txhi >> 8);
        body += 2 + 12;
    }

    /* WME information element. We later notify rtw88 that QoS is enabled, so
     * advertise WME to the AP as well, especially for stricter 5GHz networks. */
    static const uint8_t wme_info[] = {
        0xdd, 0x07, 0x00, 0x50, 0xf2, 0x02, 0x00, 0x01, 0x00
    };
    memcpy(body, wme_info, sizeof(wme_info));
    body += sizeof(wme_info);

    /* Advertise the cipher choice we actually implement: pairwise CCMP/AES
     * with the AP's selected group cipher.  Mixed TKIP+AES APs often list
     * both pairwise ciphers; copying that raw IE can make the AP pick a path
     * we do not want. */
    if (_wpa2) {
        if (_externalSupplicant && _assocRsnIELen >= 2 && _assocRsnIELen <= sizeof(_assocRsnIE) &&
            _assocRsnIE[0] == 48 && (uint16_t)(_assocRsnIE[1] + 2) == _assocRsnIELen) {
            memcpy(body, _assocRsnIE, _assocRsnIELen);
            body += _assocRsnIELen;
        } else {
            uint16_t rsn_len = rtw88BuildSelectedRsnIe(body, _targetBSS.group_cipher);
            body += rsn_len;
        }
    }

    *len = (uint32_t)(body - buf);
    return true;
}

IOReturn RTW88IEEE80211::cmdSetAssocRsnIE(const uint8_t *ie, uint16_t len)
{
    if (!ie || len < 2 || len > sizeof(_assocRsnIE) || ie[0] != 48 ||
        (uint16_t)(ie[1] + 2) != len)
        return kIOReturnBadArgument;
    memcpy(_assocRsnIE, ie, len);
    if (len < sizeof(_assocRsnIE))
        bzero(_assocRsnIE + len, sizeof(_assocRsnIE) - len);
    _assocRsnIELen = len;
    return kIOReturnSuccess;
}

void RTW88IEEE80211::clearAssocRsnIE()
{
    bzero(_assocRsnIE, sizeof(_assocRsnIE));
    _assocRsnIELen = 0;
}

IOReturn RTW88IEEE80211::copyTargetIEs(uint8_t *out, uint32_t capacity, uint32_t *len) const
{
    if (!len) return kIOReturnBadArgument;
    uint32_t need = _targetBSS.ies_len > sizeof(_targetBSS.ies) ? sizeof(_targetBSS.ies) : _targetBSS.ies_len;
    if (!out || capacity < need) {
        *len = need;
        return need ? kIOReturnNoSpace : kIOReturnNotReady;
    }
    if (!need) return kIOReturnNotReady;
    memcpy(out, _targetBSS.ies, need);
    *len = need;
    return kIOReturnSuccess;
}

IOReturn RTW88IEEE80211::copyTargetRsnIE(uint8_t *out, uint16_t capacity, uint16_t *len) const
{
    if (!out || !len) return kIOReturnBadArgument;
    if (_assocRsnIELen >= 2) {
        if (capacity < _assocRsnIELen) return kIOReturnNoSpace;
        memcpy(out, _assocRsnIE, _assocRsnIELen);
        *len = _assocRsnIELen;
        return kIOReturnSuccess;
    }
    uint32_t total = _targetBSS.ies_len > sizeof(_targetBSS.ies) ? sizeof(_targetBSS.ies) : _targetBSS.ies_len;
    for (uint32_t pos = 0; pos + 2 <= total;) {
        uint32_t n = (uint32_t)_targetBSS.ies[pos + 1] + 2;
        if (n > total - pos) break;
        if (_targetBSS.ies[pos] == 48) {
            if (n > capacity) return kIOReturnNoSpace;
            memcpy(out, _targetBSS.ies + pos, n);
            *len = (uint16_t)n;
            return kIOReturnSuccess;
        }
        pos += n;
    }
    return kIOReturnNotFound;
}

IOReturn RTW88IEEE80211::cmdInstallExternalKey(bool pairwise, uint8_t keyidx, uint32_t cipher,
                                                const uint8_t *key, uint8_t key_len)
{
    if (!_externalSupplicant || !_wpa2 || !_associatedVisible)
        return kIOReturnNotReady;
    if (!key || !key_len) return kIOReturnBadArgument;
    if (pairwise && !_sta) return kIOReturnNotReady;

    struct ieee80211_key_conf **slot = pairwise ? &_ptkConf : &_gtkConf;
    if (!installKey(slot, pairwise, keyidx, cipher, key, key_len))
        return kIOReturnUnsupported;

    if (pairwise)
        _externalPTKInstalled = true;
    else
        _externalGTKInstalled = true;

    IOLog("rtw88: Apple RSN key installed type=%s ptk=%d gtk=%d\n",
          pairwise ? "PTK" : "GTK", _externalPTKInstalled, _externalGTKInstalled);

    /* The controlled port becomes valid only once both keys are present.
     * This mirrors AirportItlwm's setGTK()->ieee80211_set_link_state(UP)
     * sequencing while tolerating an unusual key callback order. */
    if (_externalPTKInstalled && _externalGTKInstalled) {
        if (_parent)
            _parent->setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid);
        startTxAggregation();
    }

    return kIOReturnSuccess;
}

/* ------------------------------------------------------------------ */
/*  Disconnect                                                          */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::clearDeferredJoin()
{
    _deferredConnectPending = false;
    _deferredPMKValid = false;
    _deferredBSSIDValid = false;
    _deferredExternalSupplicant = false;
    bzero(_deferredSSID, sizeof(_deferredSSID));
    bzero(_deferredPassword, sizeof(_deferredPassword));
    bzero(_deferredPMK, sizeof(_deferredPMK));
    bzero(_deferredBSSID, sizeof(_deferredBSSID));
}

void RTW88IEEE80211::cancelAuthentication()
{
    __atomic_store_n(&_connectCancelled, true, __ATOMIC_RELEASE);
    if (_timer) _timer->cancelTimeout();
    // All authentication retries use this call, so draining it also drains retries.
    if (_connectTC) thread_call_cancel_wait(_connectTC);
}

IOReturn RTW88IEEE80211::cmdDisconnect()
{
    clearDeferredJoin();
    /* A disconnected SCANNING state is not an association that can be torn
     * down.  AirportItlwm's net80211 stays in SCAN here; it does not emit a
     * spurious link-disconnected event.  Cancel any queued join but preserve
     * the scan transaction so CoreWiFi can receive SCAN_DONE cleanly. */
    if (_state == RTW88_STATE_SCANNING && !_associatedVisible &&
        _scanReturnState == RTW88_STATE_IDLE) {
        IOLog("rtw88: DISASSOCIATE acknowledged during disconnected scan (no link teardown)\n");
        return kIOReturnSuccess;
    }

    /* v52: a background scan deliberately changes the implementation state
     * from CONNECTED to SCANNING while _associatedVisible keeps IO80211 in
     * RUN.  Tearing down the station without first terminating that scan lets
     * scanDone() race the new association and can resurrect stale channel/BSS
     * state.  That is exactly the shape of the issue #3 network-transition
     * failure.  Drain the connected scan synchronously before disassociation. */
    if (_state == RTW88_STATE_SCANNING && _associatedVisible &&
        _scanReturnState == RTW88_STATE_CONNECTED) {
        IOLog("rtw88: DISASSOCIATE aborting connected background scan before teardown\n");
        if (!abortActiveScan(true))
            return kIOReturnBusy;
    }

    cancelAuthentication();
    /* Explicit disassociation ends the IO80211 RUN latch immediately. */
    _associatedVisible = false;
    if (_state == RTW88_STATE_IDLE) return kIOReturnSuccess;
    doDisconnect();
    return kIOReturnSuccess;
}

IOReturn RTW88IEEE80211::suspendForSystemSleep()
{
    /* System sleep/hibernate is stronger than an ordinary Wi-Fi toggle.  Do
     * not wait for a background scan timeout: stop its worker synchronously,
     * restore the infrastructure channel, and then reuse the normal power-off
     * cleanup.  The resumed kernel image must not carry a logical CONNECTED
     * state into freshly-reset firmware. */
    clearDeferredJoin();
    cancelAuthentication();

    if (_state == RTW88_STATE_SCANNING) {
        const RTW88State returnState = _scanReturnState;
        (void)abortActiveScan(false);
        _manualScanAbort = true;
        if (_manualScanTC)
            thread_call_cancel_wait(_manualScanTC);
        if (returnState == RTW88_STATE_CONNECTED)
            restoreConnectedChannel();
        _state = returnState == RTW88_STATE_IDLE ? RTW88_STATE_IDLE : returnState;
        _scanReturnState = RTW88_STATE_IDLE;
        _manualScanChannelCount = 0;
        _manualScanOnHomeChannel = false;
    }

    if (_awdlOffChannel)
        restoreSTAChannelAfterAWDL();

    return cmdPowerOff();
}

IOReturn RTW88IEEE80211::cmdPowerOn()
{
    IOReturn ret = powerOn();
    if (ret == kIOReturnSuccess && _state == RTW88_STATE_DISCONNECTING)
        _state = RTW88_STATE_IDLE;
    return ret;
}

IOReturn RTW88IEEE80211::cmdPowerOff()
{
    /* abortActiveScan may synchronously deliver scanDone while still powered.
     * Remove its queued association before draining either worker. */
    clearDeferredJoin();
    cancelAuthentication();
    if (_state == RTW88_STATE_SCANNING && !abortActiveScan(true))
        return kIOReturnBusy;

    if (_state == RTW88_STATE_CONNECTED ||
        _state == RTW88_STATE_AUTHENTICATING ||
        _state == RTW88_STATE_ASSOCIATING ||
        _state == RTW88_STATE_HANDSHAKING)
        doDisconnect();
    else {
        clearKeys();
        releaseSta();
    }

    powerOff();
    clearAssocRsnIE();
    _state = RTW88_STATE_IDLE;
    _associatedVisible = false;
    _scanReturnState = RTW88_STATE_IDLE;
    if (_parent)
        _parent->setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

void RTW88IEEE80211::doDisconnect()
{
    /* A deauth must be transmitted on the infrastructure channel.  AWDL may
     * currently own the single PHY, so hand it back before tearing down the
     * association or clearing the visibility latch used by the restore path. */
    if (_awdlOffChannel)
        restoreSTAChannelAfterAWDL();

    if (!_hw || !_vif) {
        _state = RTW88_STATE_IDLE;
        _associatedVisible = false;
        _scanReturnState = RTW88_STATE_IDLE;
        return;
    }
    clearKeys();
    clearAssocRsnIE();

    /* Send deauth frame */
    uint8_t deauth[28] = {};
    struct ieee80211_hdr_3addr *hdr = (struct ieee80211_hdr_3addr *)deauth;
    hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_DEAUTH);
    memcpy(hdr->addr1, _targetBSS.bssid, 6);
    memcpy(hdr->addr2, _macAddr, 6);
    memcpy(hdr->addr3, _targetBSS.bssid, 6);
    uint8_t *body = deauth + sizeof(*hdr);
    body[0] = WLAN_REASON_DEAUTH_LEAVING; body[1] = 0;
    txMgmtFrame(deauth, sizeof(*hdr) + 2);

    /* Notify driver */
    struct ieee80211_bss_conf *bss = &_vif->bss_conf;
    bss->assoc = false;
    bss->aid = 0;
    /* rtw_ops_bss_info_changed uses cfg.assoc, not the legacy bss field.
     * Clear both BEFORE the callback, otherwise disconnect re-enters the
     * firmware association path with the previous AP's AID. */
    _vif->cfg.assoc = false;
    _vif->cfg.aid = 0;
    _assocAID = 0;
    if (_hw->ops && _hw->ops->bss_info_changed)
        _hw->ops->bss_info_changed(_hw, _vif, bss, BSS_CHANGED_ASSOC);
    releaseSta();

    _state = RTW88_STATE_IDLE;
    _associatedVisible = false;
    _scanReturnState = RTW88_STATE_IDLE;
    _timer->cancelTimeout();
    if (_parent)
        _parent->setLinkStatus(kIONetworkLinkValid);
    if (_delegate)
        _delegate->rtw88Event(kRTW88EventDisconnected, nullptr);
}

/* ------------------------------------------------------------------ */
/*  WPA2 4-way handshake                                                */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::handleEAPOL(const uint8_t *data, uint32_t len)
{
    if (len < 99 || data[1] != 3)
        return;

    uint16_t eapol_body_len = (uint16_t)((data[2] << 8) | data[3]);
    uint32_t eapol_len = 4 + eapol_body_len;
    if (eapol_len > len || eapol_len < 99)
        return;

    uint16_t key_info = (uint16_t)((data[5] << 8) | data[6]);
    bool is_m1 = (key_info & 0x0088) == 0x0088 && !(key_info & 0x0100);
    bool is_m3 = (key_info & 0x01c8) == 0x01c8;
    uint16_t key_data_len = (uint16_t)((data[97] << 8) | data[98]);

    IOLog("rtw88: EAPOL key_info=0x%04x key_data_len=%u M1=%d M3=%d\n",
          key_info, key_data_len, is_m1, is_m3);

    if (is_m1) {
        memcpy(_anonce, data + 17, 32);
        read_random(_snonce, 32);
        derivePTK(_pmk, _anonce, _snonce, _macAddr, _targetBSS.bssid, _ptk);
        memcpy(_replayCtr, data + 9, 8);
        sendEAPOLKey(2, _replayCtr, false, false, true);
        uint64_t d;
        clock_interval_to_deadline(5000, kMillisecondScale, &d);
        _timer->wakeAtTime(d);
    } else if (is_m3) {
        if (!eapol_mic_ok(_ptk, data, eapol_len)) {
            IOLog("rtw88: EAPOL M3 MIC check failed\n");
            return;
        }

        if (99 + key_data_len > eapol_len) {
            IOLog("rtw88: EAPOL M3 key data truncated\n");
            return;
        }

        uint8_t gtk[32] = {};
        uint8_t gtk_len = 0;
        uint8_t gtk_idx = 0;
        const uint8_t *key_data = data + 99;
        uint8_t unwrapped[256] = {};
        uint16_t unwrapped_len = 0;

        if (key_data_len) {
            if (key_info & 0x1000) {
                if (!aes_unwrap_128(_ptk + 16, key_data, key_data_len,
                                    unwrapped, &unwrapped_len)) {
                    IOLog("rtw88: failed to unwrap GTK key data\n");
                    return;
                }
                key_data = unwrapped;
                key_data_len = unwrapped_len;
            }

            if (!extract_gtk_from_kde(key_data, key_data_len,
                                      gtk, &gtk_len, &gtk_idx)) {
                IOLog("rtw88: no GTK KDE found in M3 key data\n");
            }
        }

        memcpy(_replayCtr, data + 9, 8);

        if (!installKey(&_ptkConf, true, 0, WLAN_CIPHER_SUITE_CCMP,
                        _ptk + 32, 16))
            return;
        uint32_t groupCipher = (_targetBSS.group_cipher == WLAN_CIPHER_SUITE_TKIP) ?
            WLAN_CIPHER_SUITE_TKIP : WLAN_CIPHER_SUITE_CCMP;
        if (gtk_len && !installKey(&_gtkConf, false, gtk_idx, groupCipher,
                                   gtk, gtk_len))
            return;

        sendEAPOLKey(4, _replayCtr, false, false, true);
        _associatedVisible = true;
        _state = RTW88_STATE_CONNECTED;
        _timer->cancelTimeout();
        if (_parent)
            _parent->setLinkStatus(kIONetworkLinkActive | kIONetworkLinkValid);
        startTxAggregation();   /* keys are installed — negotiate uplink A-MPDU */
        IOLog("rtw88: WPA2 connected! gtk_len=%u gtk_idx=%u\n", gtk_len, gtk_idx);
    }
}

void RTW88IEEE80211::sendEAPOLKey(int step, const uint8_t *replay_counter,
                                    bool install, bool ack, bool mic)
{
    uint8_t frame[512] = {};
    uint8_t *eth = frame;
    memcpy(eth,     _targetBSS.bssid, 6); /* DA = AP */
    memcpy(eth + 6, _macAddr, 6);          /* SA = us */
    eth[12] = 0x88; eth[13] = 0x8e;        /* EAPOL ethertype */

    uint8_t *eapol = eth + 14;
    eapol[0] = 2;  /* version 2 */
    eapol[1] = 3;  /* EAPOL-Key */

    uint8_t *key = eapol + 4;
    key[0] = 2;  /* key descriptor = RSN */
    uint16_t ki = 0x000A; /* version=2 (HMAC-SHA1/AES), pairwise */
    if (mic)     ki |= 0x0100; /* MIC */
    if (install) ki |= 0x0040; /* Install */
    if (ack)     ki |= 0x0080; /* ACK */
    if (step == 4) ki |= 0x0200; /* Secure (bit 9) */
    key[1] = (uint8_t)(ki >> 8);
    key[2] = (uint8_t)(ki & 0xff);
    key[3] = 0; key[4] = 16; /* key length = 16 (AES-128) */
    memcpy(key + 5, replay_counter, 8);  /* key[5..12]  = Replay Counter */
    memcpy(key + 13, _snonce, 32);       /* key[13..44] = SNonce */
    /* key[45..60] = Key IV (zeros), key[61..68] = RSC (zeros) */
    /* key[69..76] = Reserved (zeros), key[77..92] = MIC (below) */

    uint16_t key_data_len = 0;
    if (step == 2) {
        key_data_len = rtw88BuildSelectedRsnIe(eapol + 99, _targetBSS.group_cipher);
        if (99 + key_data_len > sizeof(frame) - 14)
            key_data_len = 0;
    }

    key[93] = (uint8_t)(key_data_len >> 8);
    key[94] = (uint8_t)(key_data_len & 0xff);

    uint16_t eapol_key_len = (uint16_t)(95 + key_data_len);
    uint32_t eapol_total = 4 + eapol_key_len;
    eapol[2] = (uint8_t)(eapol_key_len >> 8);
    eapol[3] = (uint8_t)(eapol_key_len & 0xff);

    if (mic) {
        /* MIC = first 16 bytes of HMAC-SHA1(KCK, EAPOL frame with MIC zeroed)
         * KCK = _ptk[0..15]. */
        uint8_t mic_buf[20];
        kern_hmac_sha1(_ptk, 16, eapol, eapol_total, mic_buf);
        memcpy(key + 77, mic_buf, 16);
    }

    uint32_t ethlen = 14 + eapol_total;
    mbuf_t m = rtw88_make_packet_mbuf(frame, ethlen);
    if (!m) return;
    txDataFrame(m);
}

/* ------------------------------------------------------------------ */
/*  Frame TX helpers                                                    */
/* ------------------------------------------------------------------ */

bool RTW88IEEE80211::txRawManagementFrame(const uint8_t *frame, uint32_t len)
{
    if (!frame || len < 24 || len > 4096)
        return false;

    /* Accept management frames only. AWDL uses vendor/public action frames;
     * keeping data/control frames out of this path protects the normal STA
     * datapath and rtw88 queue assumptions. */
    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    if ((fc & 0x000c) != 0x0000)
        return false;

    if (len >= 40 && RTW88AWDL::actionHeader(frame + 24, len - 24) && !canTransmitAWDL())
        return false;
    return txMgmtFrame(frame, len);
}

bool RTW88IEEE80211::txMgmtFrame(const uint8_t *frame, uint32_t len)
{
    if (!_powered || !frame || len < 24 || len > 4096 ||
        !_hw || !_hw->ops || !_hw->ops->tx || !_vif) return false;

    struct sk_buff *skb = alloc_skb(len + 128, GFP_ATOMIC);
    if (!skb) return false;
    skb_reserve(skb, 128); /* headroom for TX descriptor (48 B) + pkt_offset padding */
    skb_put_data(skb, frame, len);

    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    memset(info, 0, sizeof(*info));
    info->flags = IEEE80211_TX_CTL_FIRST_FRAGMENT;
    if (frame[4] & 1) info->flags |= IEEE80211_TX_CTL_NO_ACK;
    info->control.vif = _vif;

    struct ieee80211_tx_control ctrl = { .sta = nullptr };
    _hw->ops->tx(_hw, &ctrl, skb);
    return true;
}

/* ------------------------------------------------------------------ */
/*  A-MPDU BlockAck negotiation                                          */
/*                                                                      */
/*  802.11n/ac throughput depends on A-MPDU aggregation, which requires */
/*  a per-TID BlockAck agreement negotiated over the air with ADDBA     */
/*  action frames (category 3 = BACK).  mac80211 normally does this; we  */
/*  bypass mac80211, so the MLME drives it here:                         */
/*    - TX (uplink) agg: we send an ADDBA Request and, on a successful   */
/*      ADDBA Response, tag our data frames with IEEE80211_TX_CTL_AMPDU. */
/*    - RX (downlink) agg: we answer the AP's ADDBA Request; Realtek HW  */
/*      then auto-generates the RX BlockAck and de-aggregates for us     */
/*      (rtw88's ampdu_action is a no-op for RX_START/STOP).             */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::sendAddbaRequest(uint8_t tid)
{
    uint8_t f[24 + 9] = {};
    struct ieee80211_hdr_3addr *h = (struct ieee80211_hdr_3addr *)f;
    h->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION);
    h->duration_id   = 0;
    memcpy(h->addr1, _targetBSS.bssid, 6);
    memcpy(h->addr2, _macAddr, 6);
    memcpy(h->addr3, _targetBSS.bssid, 6);
    h->seq_ctrl = cpu_to_le16((uint16_t)(_txSeq++ & 0xFFF) << 4);

    if (++_baDialog == 0) _baDialog = 1;   /* dialog token must be non-zero */

    uint8_t *b = f + 24;
    b[0] = WLAN_CATEGORY_BACK;
    b[1] = WLAN_ACTION_ADDBA_REQ;
    b[2] = _baDialog;
    /* Block Ack Parameter Set: A-MSDU(bit0)=0, policy(bit1)=1 (immediate),
     * TID(bits 2-5), Buffer Size(bits 6-15). */
    uint16_t param = (uint16_t)((1u << 1) |
                                ((unsigned)(tid & 0xf) << 2) |
                                ((unsigned)(_baBufSize & 0x3ff) << 6));
    b[3] = (uint8_t)(param & 0xff);
    b[4] = (uint8_t)(param >> 8);
    b[5] = 0; b[6] = 0;   /* Block Ack Timeout = 0 (no timeout) */
    /* Block Ack Starting Sequence Control: SSN (bits 4-15) = next data SN, so
     * the AP's reorder window aligns with the TID-0 data stream. */
    uint16_t ssc = (uint16_t)((uint16_t)(_dataSeq & 0xFFF) << 4);
    b[7] = (uint8_t)(ssc & 0xff);
    b[8] = (uint8_t)(ssc >> 8);

    _txBaPending = txMgmtFrame(f, sizeof(f));
}

void RTW88IEEE80211::sendAddbaResponse(uint8_t tid, uint8_t dialog,
                                       uint16_t req_param, uint16_t ba_timeout)
{
    uint8_t f[24 + 9] = {};
    struct ieee80211_hdr_3addr *h = (struct ieee80211_hdr_3addr *)f;
    h->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ACTION);
    h->duration_id   = 0;
    memcpy(h->addr1, _targetBSS.bssid, 6);
    memcpy(h->addr2, _macAddr, 6);
    memcpy(h->addr3, _targetBSS.bssid, 6);
    h->seq_ctrl = cpu_to_le16((uint16_t)(_txSeq++ & 0xFFF) << 4);

    uint8_t *b = f + 24;
    b[0] = WLAN_CATEGORY_BACK;
    b[1] = WLAN_ACTION_ADDBA_RESP;
    b[2] = dialog;
    b[3] = 0; b[4] = 0;   /* Status Code = 0 (success) */
    /* Echo the requester's A-MSDU bit; force immediate policy and our TID. */
    uint16_t param = (uint16_t)((unsigned)(req_param & 0x0001u) |
                                (1u << 1) |
                                ((unsigned)(tid & 0xf) << 2) |
                                ((unsigned)(_baBufSize & 0x3ff) << 6));
    b[5] = (uint8_t)(param & 0xff);
    b[6] = (uint8_t)(param >> 8);
    b[7] = (uint8_t)(ba_timeout & 0xff);
    b[8] = (uint8_t)(ba_timeout >> 8);

    txMgmtFrame(f, sizeof(f));
}

/* The HT restriction applies to the pairwise cipher. A mixed WPA/WPA2 AP
 * may use TKIP for group traffic while this station negotiates CCMP for
 * unicast. Do not disable HT/VHT and unicast A-MPDU for that combination. */
bool RTW88IEEE80211::htAllowed() const
{
    /* Only the supported open/CCMP pairwise paths may use HT. Unknown and
     * legacy pairwise ciphers fail closed; the group key remains unchanged. */
    return _targetBSS.cipher == 0 ||
           _targetBSS.cipher == WLAN_CIPHER_SUITE_CCMP;
}

void RTW88IEEE80211::startTxAggregation()
{
    if (_txBaActive) return;
    if (!htAllowed()) return;
    if (!_sta || !_sta->deflink.ht_cap.ht_supported) return;
    IOLog("rtw88: starting TX A-MPDU — sending ADDBA request (tid=%u)\n", _baTid);
    sendAddbaRequest(_baTid);
}

void RTW88IEEE80211::handleBackAction(const uint8_t *b, uint32_t len)
{
    if (len < 2) return;
    switch (b[1]) {   /* BlockAck action field */
    case WLAN_ACTION_ADDBA_REQ: {
        /* AP wants to aggregate downlink traffic to us — accept it and stand
         * up an RX reorder buffer for the TID so we deliver in order. */
        if (len < 9) return;
        uint8_t  dialog    = b[2];
        uint16_t req_param = (uint16_t)(b[3] | (b[4] << 8));
        uint16_t ba_to     = (uint16_t)(b[5] | (b[6] << 8));
        uint16_t ssc       = (uint16_t)(b[7] | (b[8] << 8));
        uint8_t  tid       = (uint8_t)((req_param >> 2) & 0xf);
        uint16_t bufsz     = (uint16_t)((req_param >> 6) & 0x3ff);
        uint16_t ssn       = (uint16_t)(ssc >> 4);
        rxBaSetup(tid, ssn, bufsz);
        sendAddbaResponse(tid, dialog, req_param, ba_to);
        IOLog("rtw88: RX ADDBA request (tid=%u ssn=%u buf=%u) — accepted, "
              "downlink A-MPDU on\n", tid, ssn, bufsz);
        break;
    }
    case WLAN_ACTION_ADDBA_RESP: {
        /* Response to our TX ADDBA request. */
        if (len < 9) return;
        uint16_t status = (uint16_t)(b[3] | (b[4] << 8));
        uint16_t param  = (uint16_t)(b[5] | (b[6] << 8));
        uint8_t  tid    = (uint8_t)((param >> 2) & 0xf);
        if (!_txBaPending || b[2] != _baDialog || tid != _baTid) break;
        _txBaPending = false;
        if (status == 0 && (param & (1u << 1))) {
            _txBaActive = true;
            IOLog("rtw88: TX ADDBA accepted (tid=%u) — uplink A-MPDU on\n", tid);
        } else {
            IOLog("rtw88: TX ADDBA rejected status=%u tid=%u\n", status, tid);
        }
        break;
    }
    case WLAN_ACTION_DELBA: {
        if (len < 6) return;
        uint16_t del_param = (uint16_t)(b[2] | (b[3] << 8));
        uint8_t  tid       = (uint8_t)((del_param >> 12) & 0xf);
        bool     initiator = (del_param & (1u << 11)) != 0;
        /* initiator=0: AP is the recipient of the agreement it is tearing down
         * — our uplink TX BA, so stop aggregating.  initiator=1: AP is the
         * originator — its downlink BA, so drop our RX reorder buffer. */
        if (!initiator && tid == _baTid) {
            _txBaActive = false;
            _txBaPending = false;
        }
        if (initiator)
            rxBaTeardown(tid);
        IOLog("rtw88: RX DELBA tid=%u initiator=%d\n", tid, initiator);
        break;
    }
    default:
        break;
    }
}

bool RTW88IEEE80211::txNullFunc(bool powerSave)
{
    if (!_hw || !_hw->ops || !_hw->ops->tx || !_vif || !_sta)
        return false;

    static const uint16_t IEEE80211_STYPE_NULLFUNC = 0x0040;
    struct sk_buff *skb = alloc_skb(24 + 128, GFP_ATOMIC);
    if (!skb) return false;
    skb_reserve(skb, 128);

    struct ieee80211_hdr_3addr *h =
        (struct ieee80211_hdr_3addr *)skb_put(skb, 24);
    uint16_t fc = IEEE80211_FTYPE_DATA | IEEE80211_STYPE_NULLFUNC |
                  IEEE80211_FCTL_TODS;
    if (powerSave)
        fc |= IEEE80211_FCTL_PM;
    h->frame_control = cpu_to_le16(fc);
    h->duration_id   = 0;
    memcpy(h->addr1, _targetBSS.bssid, 6);
    memcpy(h->addr2, _macAddr, 6);
    memcpy(h->addr3, _targetBSS.bssid, 6);
    h->seq_ctrl = cpu_to_le16((uint16_t)(_txSeq++ & 0xFFF) << 4);

    skb_set_queue_mapping(skb, IEEE80211_AC_BE);
    skb->priority = 0;

    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    memset(info, 0, sizeof(*info));
    info->flags = IEEE80211_TX_CTL_FIRST_FRAGMENT;
    info->band  = (_targetBSS.channel > 14) ? NL80211_BAND_5GHZ
                                            : NL80211_BAND_2GHZ;
    info->control.vif = _vif;
    info->control.sta = _sta;

    struct ieee80211_tx_control ctrl = { .sta = _sta };
    _hw->ops->tx(_hw, &ctrl, skb);
    return true;
}

bool RTW88IEEE80211::txProbeRequest()
{
    if (!_hw || !_hw->ops || !_hw->ops->tx)
        return false;

    uint8_t frame[128] = {};
    struct ieee80211_hdr_3addr *hdr =
        (struct ieee80211_hdr_3addr *)frame;
    hdr->frame_control =
        cpu_to_le16(IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_PROBE_REQ);
    memset(hdr->addr1, 0xff, 6);
    memcpy(hdr->addr2, _macAddr, 6);
    memset(hdr->addr3, 0xff, 6);
    hdr->seq_ctrl = cpu_to_le16((uint16_t)(_txSeq++ & 0xFFF) << 4);

    uint8_t *body = frame + sizeof(*hdr);
    body[0] = WLAN_EID_SSID;
    body[1] = 0;
    body += 2;

    static const uint8_t rates_2g[] = {
        0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24
    };
    static const uint8_t rates_5g[] = {
        0x8c, 0x12, 0x98, 0x24, 0x30, 0x48, 0x60, 0x6c
    };
    bool is5g = _hw->conf.chandef.chan &&
                _hw->conf.chandef.chan->band == NL80211_BAND_5GHZ;
    const uint8_t *rates = is5g ? rates_5g : rates_2g;
    uint8_t ratesLen = is5g ? sizeof(rates_5g) : sizeof(rates_2g);

    body[0] = WLAN_EID_SUPP_RATES;
    body[1] = ratesLen;
    memcpy(body + 2, rates, ratesLen);
    body += 2 + ratesLen;

    return txMgmtFrame(frame, (uint32_t)(body - frame));
}

/* AWDL uses a direct 802.11 data frame, not the infrastructure ToDS path.
 * The payload format is:
 *   802.11(24) | SNAP(00:17:f2, PID 0x0800) | AWDL data(8) | L3 payload
 * The Realtek tx core explicitly supports data frames with sta == nullptr,
 * selecting the vif MAC-ID and conservative 6 Mbps/20 MHz defaults. */
bool RTW88IEEE80211::txAWDLDataFrame(mbuf_t m)
{
    if (!m || !_powered || !_awdlReceiveMode ||
        (_state != RTW88_STATE_IDLE && _state != RTW88_STATE_CONNECTED) ||
        !_hw || !_hw->ops || !_hw->ops->tx || !_vif ||
        !_hw->conf.chandef.chan || !_awdlChannel ||
        _hw->conf.chandef.chan->hw_value != _awdlChannel) {
        if (m) mbuf_freem(m);
        return false;
    }

    size_t total = mbuf_pkthdr_len(m);
    if (total < 14 || total > 4096) {
        mbuf_freem(m);
        return false;
    }

    uint8_t eh[14] = {};
    if (mbuf_copydata(m, 0, sizeof(eh), eh) != 0) {
        mbuf_freem(m);
        return false;
    }
    const uint16_t ethertype = (uint16_t)((eh[12] << 8) | eh[13]);
    const uint32_t paylen = (uint32_t)total - 14;

    /* 24-byte 802.11 header + 8-byte AWDL SNAP + 8-byte AWDL data header. */
    const uint32_t framelen = 24 + 8 + 8 + paylen;
    struct sk_buff *skb = alloc_skb(framelen + 128, GFP_ATOMIC);
    if (!skb) {
        mbuf_freem(m);
        return false;
    }
    skb_reserve(skb, 128);

    struct ieee80211_hdr_3addr *h =
        (struct ieee80211_hdr_3addr *)skb_put(skb, 24);
    bzero(h, sizeof(*h));
    h->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA);
    memcpy(h->addr1, eh, 6);       /* peer / multicast destination */
    memcpy(h->addr2, _awdlAddress, 6); /* Same virtual identity as action frames. */
    static const uint8_t awdlBssid[6] = {0x00,0x25,0x00,0xff,0x94,0x73};
    memcpy(h->addr3, awdlBssid, sizeof(awdlBssid));
    h->seq_ctrl = cpu_to_le16((uint16_t)(_txSeq++ & 0x0fff) << 4);

    /* LLC/SNAP with Apple's AWDL OUI and AWDL protocol ID 0x0800. */
    uint8_t *snap = skb_put(skb, 8);
    snap[0] = 0xaa; snap[1] = 0xaa; snap[2] = 0x03;
    snap[3] = 0x00; snap[4] = 0x17; snap[5] = 0xf2;
    snap[6] = 0x08; snap[7] = 0x00;

    /* AWDL data header: LE magic 0x0403, LE sequence, zero pad, BE EtherType. */
    uint8_t *aw = skb_put(skb, 8);
    aw[0] = 0x03; aw[1] = 0x04;
    uint16_t seq = _awdlDataSeq++;
    aw[2] = (uint8_t)(seq & 0xff); aw[3] = (uint8_t)(seq >> 8);
    aw[4] = 0; aw[5] = 0;
    aw[6] = (uint8_t)(ethertype >> 8); aw[7] = (uint8_t)ethertype;

    if (paylen) {
        uint8_t *payload = skb_put(skb, paylen);
        if (mbuf_copydata(m, 14, paylen, payload) != 0) {
            kfree_skb(skb);
            mbuf_freem(m);
            return false;
        }
    }

    skb_set_queue_mapping(skb, IEEE80211_AC_BE);
    skb->priority = 0;
    skb->protocol = cpu_to_be16(ethertype);

    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    memset(info, 0, sizeof(*info));
    info->flags = IEEE80211_TX_CTL_FIRST_FRAGMENT;
    if (eh[0] & 1) info->flags |= IEEE80211_TX_CTL_NO_ACK;
    info->band = (_hw->conf.chandef.chan &&
                  _hw->conf.chandef.chan->band == NL80211_BAND_5GHZ)
                   ? NL80211_BAND_5GHZ : NL80211_BAND_2GHZ;
    info->control.vif = _vif;
    info->control.sta = nullptr;

    struct ieee80211_tx_control ctrl = { .sta = nullptr };
    _hw->ops->tx(_hw, &ctrl, skb);
    mbuf_freem(m);
    return true;
}

/* Detect the AWDL data envelope before applying infrastructure association
 * rules. Returns true when ownership of skb was consumed. */
bool RTW88IEEE80211::tryDeliverAWDLDataFrame(struct sk_buff *skb)
{
    if (!_awdlReceiveMode || !skb || skb->len < 24 + 8 + 8)
        return false;

    struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
    uint16_t fc = le16_to_cpu(hdr->frame_control);
    if ((fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) != 0)
        return false;

    static const uint8_t awdlBssid[6] = {0x00,0x25,0x00,0xff,0x94,0x73};
    if (memcmp(hdr->addr3, awdlBssid, sizeof(awdlBssid)) != 0)
        return false;

    uint16_t hdrlen = ieee80211_get_hdrlen_from_skb(skb);
    if (hdrlen < 24 || skb->len < (uint32_t)hdrlen + 16)
        return false;

    const uint8_t *p = skb->data + hdrlen;
    // QoS A-MSDU starts with a subframe Ethernet header, not LLC/SNAP.
    // Reject encrypted/fragmented envelopes: this path has no reassembly or
    // decryption contract. Consume recognized AWDL aggregates on all paths.
    if (fc & (0x4000 | 0x0400) || (le16_to_cpu(hdr->seq_ctrl) & 15)) {
        kfree_skb(skb);
        return true;
    }
    if ((fc & 0x0080) && (skb->data[24] & 0x80)) {
        if (!(hdr->addr1[0] & 1) && memcmp(hdr->addr1, _awdlAddress, 6)) {
            kfree_skb(skb);
            return true;
        }
        RTW88AWDL::receiveAggregate(p, skb->len - hdrlen, _awdlAddress,
            [&](const uint8_t *da, const uint8_t *sa, uint16_t type,
                const uint8_t *payload, size_t length) {
                deliverAWDLEthernet(da, sa, type, payload, (uint32_t)length);
            });
        kfree_skb(skb);
        return true;
    }
    /* SNAP AA AA 03 00 17 F2 08 00 */
    if (p[0] != 0xaa || p[1] != 0xaa || p[2] != 0x03 ||
        p[3] != 0x00 || p[4] != 0x17 || p[5] != 0xf2 ||
        p[6] != 0x08 || p[7] != 0x00)
        return false;
    p += 8;

    if (p[0] != 0x03 || p[1] != 0x04) /* LE 0x0403 */
        return false;
    uint16_t ethertype = (uint16_t)((p[6] << 8) | p[7]);
    p += 8;
    uint32_t paylen = (uint32_t)(skb->data + skb->len - p);

    // Promiscuous reception also hears other AWDL peers' unicast traffic.
    // Consume that envelope without injecting it into this host's awdl0.
    if ((hdr->addr1[0] & 1) || memcmp(hdr->addr1, _awdlAddress, 6) == 0)
        deliverAWDLEthernet(hdr->addr1, hdr->addr2, ethertype, p, paylen);
    kfree_skb(skb);
    return true;
}

void RTW88IEEE80211::deliverAWDLEthernet(const uint8_t *da, const uint8_t *sa,
                                         uint16_t ethertype,
                                         const uint8_t *payload, uint32_t paylen)
{
    if (!_parent || !da || !sa || (!payload && paylen))
        return;
    mbuf_t m = _parent->allocateInputPacket(14 + paylen);
    if (!m)
        return;

    uint8_t eh[14];
    memcpy(eh, da, 6);
    memcpy(eh + 6, sa, 6);
    eh[12] = (uint8_t)(ethertype >> 8);
    eh[13] = (uint8_t)ethertype;
    if (mbuf_copyback(m, 0, sizeof(eh), eh, MBUF_WAITOK) != 0 ||
        (paylen && mbuf_copyback(m, 14, paylen, payload, MBUF_WAITOK) != 0)) {
        mbuf_freem(m);
        return;
    }
    _parent->injectRxAWDLFrame(m);
}

bool RTW88IEEE80211::txDataFrame(mbuf_t m)
{
    if (!m || !_powered || !_hw || !_hw->ops || !_hw->ops->tx || !_vif || !_sta) {
        if (m) mbuf_freem(m);
        return false;
    }

    /* The mbuf is an Ethernet frame: [DA(6)][SA(6)][ethertype(2)][payload].
     * The rtw88 driver's tx op expects an 802.11 frame, so we must
     * encapsulate: 802.11 data header (24) + LLC/SNAP (8) + IP payload.
     * mac80211 normally does this; we are bypassing mac80211's tx path. */
    size_t total = mbuf_pkthdr_len(m);
    if (total < 14 || total > 2048) { mbuf_freem(m); return false; }
    uint32_t paylen = (uint32_t)total - 14;

    uint8_t eh[14];
    if (mbuf_copydata(m, 0, 14, eh) != 0) { mbuf_freem(m); return false; }
    uint16_t ethertype = (uint16_t)((eh[12] << 8) | eh[13]);

    if (ethertype == ETH_P_PAE) {
        IOLog("rtw88: EAPOL TX external=%d state=%d ptk=%p len=%u\\n",
              _externalSupplicant, _state, _ptkConf, paylen);
    }

    bool protected_frame = _wpa2 && _ptkConf && ethertype != ETH_P_PAE;

    /* Use a QoS Data frame (24-byte header + 2-byte QoS Control) when HT is in
     * use — A-MPDU/BlockAck is strictly per-TID and a plain Data frame carries
     * no TID.  TKIP links (where HT is disallowed) fall back to a plain non-QoS
     * Data frame, the proven legacy path. */
    bool qos = htAllowed();
    uint32_t hlen = qos ? 26 : 24;
    uint32_t framelen = hlen + (protected_frame ? 8 : 0) + 8 + paylen;
    struct sk_buff *skb = alloc_skb(framelen + 128, GFP_ATOMIC);
    if (!skb) { mbuf_freem(m); return false; }
    skb_reserve(skb, 128);  /* TX descriptor headroom */

    /* 802.11 data header (ToDS: station -> AP) */
    struct ieee80211_hdr_3addr *h =
        (struct ieee80211_hdr_3addr *)skb_put(skb, 24);
    uint16_t fc = IEEE80211_FTYPE_DATA | IEEE80211_FCTL_TODS;
    if (qos)
        fc |= IEEE80211_STYPE_QOS_DATA;
    if (protected_frame)
        fc |= IEEE80211_FCTL_PROTECTED;
    h->frame_control = cpu_to_le16(fc);
    h->duration_id   = 0;
    memcpy(h->addr1, _targetBSS.bssid, 6); /* RA = BSSID (the AP)        */
    memcpy(h->addr2, _macAddr, 6);         /* TA = SA  (us)              */
    memcpy(h->addr3, eh, 6);               /* DA = Ethernet destination  */
    /* Each data frame needs a unique sequence number.  QoS data uses a
     * dedicated per-TID space so the BlockAck window stays gap-free; non-QoS
     * shares the mgmt counter (legacy behaviour).  rtw88 uses this header SN
     * for data frames (no hw-assigned SN on the data path). */
    h->seq_ctrl = cpu_to_le16((uint16_t)((qos ? _dataSeq++ : _txSeq++) & 0xFFF) << 4);

    /* QoS Control (2 bytes, LE): TID in bits 0-3 (BE = 0), Normal-Ack, no
     * A-MSDU. */
    if (qos) {
        uint8_t *qosc = skb_put(skb, 2);
        qosc[0] = (uint8_t)(_baTid & IEEE80211_QOS_CTL_TID_MASK);
        qosc[1] = 0;
    }

    if (protected_frame) {
        for (int i = 0; i < 6; i++) {
            if (++_ccmpTxPn[i] != 0)
                break;
        }

        uint8_t *ccmp = skb_put(skb, 8);
        ccmp[0] = _ccmpTxPn[0];
        ccmp[1] = _ccmpTxPn[1];
        ccmp[2] = 0x00;
        ccmp[3] = (uint8_t)(0x20 | (((uint8_t)_ptkConf->keyidx & 0x3) << 6));
        ccmp[4] = _ccmpTxPn[2];
        ccmp[5] = _ccmpTxPn[3];
        ccmp[6] = _ccmpTxPn[4];
        ccmp[7] = _ccmpTxPn[5];
    }

    /* RFC 1042 LLC/SNAP header */
    uint8_t *snap = skb_put(skb, 8);
    snap[0] = 0xAA; snap[1] = 0xAA; snap[2] = 0x03;
    snap[3] = 0x00; snap[4] = 0x00; snap[5] = 0x00;
    snap[6] = (uint8_t)(ethertype >> 8);
    snap[7] = (uint8_t)(ethertype & 0xff);

    /* Payload (IP packet) copied straight from the Ethernet mbuf */
    if (paylen) {
        uint8_t *pay = (uint8_t *)skb_put(skb, paylen);
        if (mbuf_copydata(m, 14, paylen, pay) != 0) {
            kfree_skb(skb);
            mbuf_freem(m);
            return false;
        }
    }

    /* Route to the Best-Effort hardware ring with a BE qsel.  queue_mapping
     * picks the ring (ac_to_hwq[AC_BE] = RTW_TX_QUEUE_BE); priority becomes
     * the TX-desc qsel (TID 0 = BE).  Without this the frame defaults to
     * queue_mapping 0 (AC_VO) and lands in the wrong ring. */
    skb_set_queue_mapping(skb, IEEE80211_AC_BE);
    skb->priority = 0;
    skb->protocol = cpu_to_be16(ethertype);

    struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
    memset(info, 0, sizeof(*info));
    info->flags = IEEE80211_TX_CTL_FIRST_FRAGMENT;
    /* Once the uplink BlockAck agreement is up, mark BE-TID frames for
     * aggregation: rtw88 then sets the descriptor's AGG_EN bit and the hardware
     * builds A-MPDUs. (rtw_tx reads this flag directly; the txq/RTW_TXQ_AMPDU
     * path is unused by this port.) */
    if (qos && _txBaActive)
        info->flags |= IEEE80211_TX_CTL_AMPDU;
    info->band  = (_targetBSS.channel > 14) ? NL80211_BAND_5GHZ
                                            : NL80211_BAND_2GHZ;
    info->control.vif = _vif;
    info->control.sta = _sta;
    if (protected_frame)
        info->control.hw_key = _ptkConf;

    struct ieee80211_tx_control ctrl = { .sta = _sta };
    _hw->ops->tx(_hw, &ctrl, skb);
    mbuf_freem(m);
    return true;
}

/* ------------------------------------------------------------------ */
/*  mbuf ↔ sk_buff conversion                                           */
/* ------------------------------------------------------------------ */

/*
 * Allocate a packet-header mbuf and copy `len` bytes from `src` into it.
 *
 * IMPORTANT: mbuf_allocpacket() may return a *chain* of mbufs (multiple
 * ~2 KB clusters) for packets larger than one cluster — e.g. A-MSDU
 * aggregated 802.11 data frames, which can be several KB.  In that case
 * mbuf_data(m) points only at the FIRST segment, and writing the whole
 * packet there overflows into adjacent kernel/mbuf-zone memory, corrupting
 * the heap (later manifesting as a GP fault in an unrelated mbuf walk such
 * as sbconcat_mbufs).
 *
 * mbuf_copyback() correctly distributes the data across every segment of
 * the chain and never overflows, so we use it instead of a raw memcpy.
 * mbuf_allocpacket() already sets each segment's length and pkthdr.len, so
 * we must NOT call mbuf_setlen() (which would wrongly set the first
 * segment's length to the whole-packet length).
 */
static mbuf_t rtw88_make_packet_mbuf(const void *src, uint32_t len)
{
    mbuf_t m = nullptr;
    if (mbuf_allocpacket(MBUF_WAITOK, len, nullptr, &m) != 0)
        return nullptr;
    if (mbuf_copyback(m, 0, len, src, MBUF_WAITOK) != 0) {
        mbuf_freem(m);
        return nullptr;
    }
    /* mbuf_allocpacket does not reliably set pkthdr.len, and depending on the
     * kernel, mbuf_copyback may not either.  Set it explicitly — otherwise
     * ether_input() strips the 14-byte header from a pkthdr.len of 0 and
     * panics with "Failed mbuf validity check: len -14". */
    mbuf_pkthdr_setlen(m, len);
    return m;
}

struct sk_buff *RTW88IEEE80211::mbufToSkb(mbuf_t m)
{
    size_t total = mbuf_pkthdr_len(m);
    struct sk_buff *skb = alloc_skb((uint32_t)(total + 64), GFP_ATOMIC);
    if (!skb) return nullptr;
    skb_reserve(skb, 128); /* headroom for TX descriptor (48 B) + pkt_offset padding */

    /* Copy contiguous mbuf chain data */
    mbuf_t cur = m;
    while (cur) {
        size_t chunk = mbuf_len(cur);
        if (chunk > 0) {
            memcpy(skb_put(skb, (uint32_t)chunk), mbuf_data(cur), chunk);
        }
        cur = mbuf_next(cur);
    }
    return skb;
}

mbuf_t RTW88IEEE80211::skbToMbuf(struct sk_buff *skb)
{
    return rtw88_make_packet_mbuf(skb->data, skb->len);
}

/* ------------------------------------------------------------------ */
/*  Timer (state machine timeout)                                       */
/* ------------------------------------------------------------------ */

void RTW88IEEE80211::timerFired(OSObject *owner, IOTimerEventSource *timer)
{
    RTW88IEEE80211 *self = OSDynamicCast(RTW88IEEE80211, owner);
    if (self) self->onTimer();
}

void RTW88IEEE80211::onTimer()
{
    switch (_state) {
    case RTW88_STATE_SCANNING:
        IOLog("rtw88: scan timeout\n");
        if (_manualScanChannelCount) {
            _manualScanAbort = true;
            break;
        } else if (_hw && _hw->ops && _hw->ops->cancel_hw_scan) {
            _hw->ops->cancel_hw_scan(_hw, _vif);
        }
        // Use the same completion path as firmware: notify SCAN_DONE and
        // resume an accepted deferred ASSOCIATE even if cancel has no callback.
        scanDone(true);
        break;

    case RTW88_STATE_AUTHENTICATING:
        IOLog("rtw88: auth timeout, retrying\n");
        if (!authenticationCancelled() && _connectTC) thread_call_enter(_connectTC);
        break;

    case RTW88_STATE_ASSOCIATING:
        failJoin("assoc-timeout");
        break;

    case RTW88_STATE_HANDSHAKING:
        IOLog("rtw88: 4-way handshake timeout\n");
        doDisconnect();
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Status queries                                                       */
/* ------------------------------------------------------------------ */

IOReturn RTW88IEEE80211::cmdGetState(struct RTW88StateResult *result)
{
    if (!result) return kIOReturnBadArgument;

    /* Expose the persistent infrastructure RUN state, not a transient rtw88
     * implementation state.  CoreWiFi immediately asks SSID/BSSID/CHANNEL
     * after ASSOC_DONE; returning anything except RUN there makes it abort a
     * link that may already have installed an RSN key. */
    RTW88State reportedState = _associatedVisible ? RTW88_STATE_CONNECTED : _state;
    result->state = reportedState;
    result->rssi = _rssi;
    memcpy(result->ssid, _targetBSS.ssid, sizeof(result->ssid));
    memcpy(result->bssid, _targetBSS.bssid, sizeof(result->bssid));
    result->channel = _targetBSS.channel;
    
    memcpy(result->mac_addr, _macAddr, 6);

    rtw88_get_fw_version(_rtwdev, &result->fw_version, &result->fw_sub_version);
    rtw88_get_chip_name(_rtwdev, result->chip_name, sizeof(result->chip_name));
    rtw88_get_stats(_rtwdev, &result->tx_byte_count, &result->rx_byte_count);
    result->scan_offload_supported =
        (_hw && _hw->ops && _hw->ops->hw_scan &&
         rtw88_hw_scan_supported(_hw)) ? 1 : 0;
    result->powered = _powered ? 1 : 0;

    return kIOReturnSuccess;
}

IOReturn RTW88IEEE80211::cmdGetRSSI(int *rssi)
{
    if (!rssi) return kIOReturnBadArgument;
    *rssi = _rssi;
    return kIOReturnSuccess;
}

uint8_t RTW88IEEE80211::txNSS() const
{
    return rtw88_get_tx_nss(_rtwdev);
}

IOReturn RTW88IEEE80211::cmdGetBSSList(uint8_t *buf, uint32_t *len)
{
    if (!buf || !len) return kIOReturnBadArgument;

    uint32_t max     = *len;
    if (max > 4095) max = 4095;

    if (max < 4) {
        *len = 0;
        return kIOReturnSuccess;
    }
    
    uint32_t written = 4; // reserve first 4 bytes for total length

    IOLockLock(_bssLock);
    for (RTW88BSS *b = _bssList; b; b = b->next) {
        /* Each entry: ssid_len(1), ssid(ssid_len), bssid(6), rssi(2),
         *             channel(1), cipher(4) */
        uint32_t entry_sz = 1 + b->ssid_len + 6 + 2 + 1 + 4;
        if (written + entry_sz > max) {
            IOLog("rtw88: BSS entry skipped (buffer full: written=%u max=%u)\n", written, max);
            break;
        }

        buf[written++] = b->ssid_len;
        memcpy(buf + written, b->ssid, b->ssid_len); written += b->ssid_len;
        memcpy(buf + written, b->bssid, 6);           written += 6;
        buf[written++] = (uint8_t)((b->rssi >> 8) & 0xff);
        buf[written++] = (uint8_t)(b->rssi & 0xff);
        buf[written++] = b->channel;
        memcpy(buf + written, &b->cipher, 4);          written += 4;
    }
    IOLockUnlock(_bssLock);

    /* Write total written bytes into the first 4 bytes */
    uint32_t total = written;
    memcpy(buf, &total, sizeof(total));

    *len = written;
    return kIOReturnSuccess;
}

void RTW88IEEE80211::getMACAddress(uint8_t *mac)
{
    memcpy(mac, _macAddr, 6);
}

IOReturn RTW88IEEE80211::setMACAddress(const uint8_t *mac)
{
    if (!mac || !_rtwdev || !_vif)
        return kIOReturnNotReady;

    const bool allZero = !(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
    if (allZero || (mac[0] & 0x01))
        return kIOReturnBadArgument;

    /* A station address must not change midway through auth/association. */
    if (_state == RTW88_STATE_CONNECTED || _state == RTW88_STATE_ASSOCIATING ||
        _state == RTW88_STATE_AUTHENTICATING)
        return kIOReturnBusy;

    memcpy(_macAddr, mac, ETH_ALEN);
    if (rtw88_set_station_mac(_hw, _vif, mac) != 0)
        return kIOReturnError;

    IOLog("rtw88: STA MAC programmed %02x:%02x:%02x:%02x:%02x:%02x\n",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return kIOReturnSuccess;
}

IOReturn RTW88IEEE80211::copyScanBSS(uint32_t index, RTW88BSS *out)
{
    if (!out || !_bssLock) return kIOReturnBadArgument;
    IOLockLock(_bssLock);
    RTW88BSS *b = _bssList;
    while (b && index--) b = b->next;
    if (b) { *out = *b; out->next = nullptr; }
    IOLockUnlock(_bssLock);
    return b ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn RTW88IEEE80211::copyCurrentBSS(RTW88BSS *out)
{
    if (!out) return kIOReturnBadArgument;

    /* v52: use the same persistent RUN latch as cmdGetState().  A connected
     * background scan has _state == SCANNING by design, but it is still the
     * current infrastructure network from CoreWiFi's point of view.  Returning
     * NotReady here made APPLE80211_IOC_CURRENT_NETWORK fail with -3903 while
     * the menu still showed an associated link (issue #3). */
    if (!_powered || !_associatedVisible)
        return kIOReturnNotReady;

    /* CURRENT_NETWORK is queried aggressively by CoreWiFi/locationd.
     * _targetBSS remains the association target, but some association paths
     * preserve the SSID bytes without preserving ssid_len. Prefer the scan
     * cache entry matching the currently-associated BSSID because it contains
     * the complete channel/capability/IE record; fall back to _targetBSS. */
    bool found = false;
    if (_bssLock) {
        IOLockLock(_bssLock);
        for (RTW88BSS *b = _bssList; b; b = b->next) {
            if (memcmp(b->bssid, _targetBSS.bssid, 6) == 0) {
                *out = *b;
                found = true;
                break;
            }
        }
        IOLockUnlock(_bssLock);
    }

    if (!found)
        *out = _targetBSS;

    out->next = nullptr;
    out->rssi = (int16_t)_rssi;

    /* Ventura's selector 103 payload is exactly apple80211_scan_result
     * (1164 bytes in the pinned SDK). Do not fail merely because the backend
     * forgot the cached SSID length when the NUL-terminated SSID is present. */
    if (out->ssid_len == 0 && out->ssid[0] != '\0')
        out->ssid_len = (uint8_t)strnlen(out->ssid, 32);

    if (out->ssid_len == 0 || out->channel == 0)
        return kIOReturnNotReady;

    return kIOReturnSuccess;
}

IOReturn RTW88IEEE80211::copyChannels(RTW88Channel *out, uint32_t capacity, uint32_t *count)
{
    if (!out || !count) return kIOReturnBadArgument;
    *count = 0;
    if (!_hw || !_hw->wiphy) return kIOReturnNotReady;
    for (unsigned band = 0; band < NL80211_NUM_BANDS; band++) {
        auto *supported = _hw->wiphy->bands[band];
        if (!supported) continue;
        for (int i = 0; i < supported->n_channels; i++) {
            auto *channel = &supported->channels[i];
            if (channel->flags & IEEE80211_CHAN_DISABLED) continue;
            if (*count == capacity) return kIOReturnNoSpace;
            int f = channel->center_freq;
            int number = f == 2484 ? 14 : f < 2500 ? (f - 2407) / 5 : (f - 5000) / 5;
            if (number <= 0 || number > 196) continue;
            out[*count].number = static_cast<uint16_t>(number);
            out[*count].passive = (channel->flags & IEEE80211_CHAN_NO_IR) != 0;
            out[*count].radar = (channel->flags & IEEE80211_CHAN_RADAR) != 0;
            (*count)++;
        }
    }
    return kIOReturnSuccess;
}
