/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The join request as macOS sends it to the driver in apple80211's ASSOCIATE:
 * 856 bytes on Sonoma, 900 on Sequoia, 908 on Tahoe. The family's own
 * translation does not take the first two, so the driver reads the request
 * itself (src/kext/AirPortRTW89Native.cpp). Plain C with no kernel or Apple
 * header, so tools/hosttest/assoc_test.c can test the decoding in userspace.
 *
 * The first 140 bytes are the same in all three sizes; the rest of the request
 * is not interpreted. Words are little-endian 32-bit, offsets in bytes:
 *   0 version, 4 mode, 8 lower auth, 12 upper auth, 16 SSID length,
 *   20 SSID (32), 52 BSSID (6), 60 key version, 64 key length, 68 key cipher,
 *   76 key (64)
 */
#ifndef RTW89_HOST_ASSOC_H
#define RTW89_HOST_ASSOC_H

#include <stdint.h>

/* apple80211's values as MacKernelSDK has them; AirPortRTW89Native.cpp checks
 * them against the SDK at compile time. */
#define RTW89_HOST_AP_MODE_INFRA      2u
#define RTW89_HOST_AUTH_NONE          0u
#define RTW89_HOST_AUTH_OPEN          1u
#define RTW89_HOST_AUTH_WPA           (1u << 0)
#define RTW89_HOST_AUTH_WPA_PSK       (1u << 1)
#define RTW89_HOST_AUTH_WPA2          (1u << 2)
#define RTW89_HOST_AUTH_WPA2_PSK      (1u << 3)
#define RTW89_HOST_AUTH_8021X         (1u << 5)
#define RTW89_HOST_AUTH_SHA256_PSK    (1u << 7)
#define RTW89_HOST_AUTH_SHA256_8021X  (1u << 8)

/* The key type when the key is the passphrase itself: what macOS sends once
 * the card says it does SAE. Seen with WPA2-PSK. */
#define RTW89_HOST_CIPHER_PASSPHRASE  10u

/* Bytes of the request that are read: up to the end of a 64-byte key at 76. */
#define RTW89_HOST_ASSOC_PREFIX       140u

struct rtw89_host_assoc {
    uint32_t upper, ssid_len, key_len, cipher;
    uint8_t ssid[32], bssid[6], key[64];
};

/* The request sizes macOS sends. */
static inline int rtw89_host_assoc_size_ok(uint32_t len)
{
    return len == 856 || len == 900 || len == 908;
}

static inline uint32_t rtw89_host_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* WPA or WPA2 signed in by 802.1X, and not also a PSK. */
static inline int rtw89_host_is_enterprise(uint32_t upper)
{
    const uint32_t enterprise = RTW89_HOST_AUTH_WPA | RTW89_HOST_AUTH_WPA2 |
                                RTW89_HOST_AUTH_8021X | RTW89_HOST_AUTH_SHA256_8021X;
    const uint32_t personal = RTW89_HOST_AUTH_WPA_PSK | RTW89_HOST_AUTH_WPA2_PSK |
                              RTW89_HOST_AUTH_SHA256_PSK;

    return (upper & enterprise) && !(upper & personal);
}

/*
 * Decodes the first RTW89_HOST_ASSOC_PREFIX bytes of a request. Returns 0 for
 * anything but an infrastructure join that is open, WPA2-PSK, or signed in by
 * 802.1X, and only with the key length that type takes.
 */
static inline int rtw89_host_assoc_decode(const uint8_t *p, struct rtw89_host_assoc *out)
{
    const uint32_t upper = rtw89_host_le32(p + 12), ssid_len = rtw89_host_le32(p + 16);
    const uint32_t key_len = rtw89_host_le32(p + 64), cipher = rtw89_host_le32(p + 68);
    const int passphrase = upper == RTW89_HOST_AUTH_WPA2_PSK &&
                           cipher == RTW89_HOST_CIPHER_PASSPHRASE;
    uint32_t i;

    if (rtw89_host_le32(p) != 1 || rtw89_host_le32(p + 4) != RTW89_HOST_AP_MODE_INFRA ||
        rtw89_host_le32(p + 8) != RTW89_HOST_AUTH_OPEN || rtw89_host_le32(p + 60) != 1)
        return 0;
    if ((upper != RTW89_HOST_AUTH_NONE && upper != RTW89_HOST_AUTH_WPA2_PSK &&
         !rtw89_host_is_enterprise(upper)) || !ssid_len || ssid_len > 32)
        return 0;
    if (passphrase ? key_len < 8 || key_len > 64 :
        upper == RTW89_HOST_AUTH_WPA2_PSK ? key_len != 32 : key_len != 0)
        return 0;
    for (i = 0; i < ssid_len; i++)
        if (!p[20 + i])
            return 0;
    if (p[52] & 1)
        return 0;

    for (i = 0; i < sizeof(*out); i++)
        ((uint8_t *)out)[i] = 0;
    out->upper = upper;
    out->ssid_len = ssid_len;
    out->key_len = key_len;
    out->cipher = cipher;
    for (i = 0; i < ssid_len; i++)
        out->ssid[i] = p[20 + i];
    for (i = 0; i < 6; i++)
        out->bssid[i] = p[52 + i];
    for (i = 0; i < key_len; i++)
        out->key[i] = p[76 + i];
    return 1;
}

#endif /* RTW89_HOST_ASSOC_H */
