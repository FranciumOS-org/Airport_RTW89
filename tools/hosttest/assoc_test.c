// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace test of the join request decoding in src/kext/rtw89_host_assoc.h:
 * the three sizes macOS sends with the same fields, what is read from them,
 * and the requests that must not be taken as a join. Built and run by
 * "make hosttest"; the kext decodes with the same header.
 */
#include <stdio.h>
#include <string.h>
#include "rtw89_host_assoc.h"

static int failures;

#define CHECK(c) do { if (!(c)) { failures++; printf("== FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static const uint8_t kBssid[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

/*
 * A join as macOS lays out its first 140 bytes. The rest of the request is
 * 0xa5, which the decoding must not read.
 */
static void make_request(uint8_t *req, uint32_t len, uint32_t upper, const char *ssid,
                         uint32_t cipher, const uint8_t *key, uint32_t key_len)
{
    memset(req, 0xa5, len);
    put32(req, 1);
    put32(req + 4, 2);
    put32(req + 8, 1);
    put32(req + 12, upper);
    put32(req + 16, strlen(ssid));
    memset(req + 20, 0, 32);
    memcpy(req + 20, ssid, strlen(ssid));
    memcpy(req + 52, kBssid, sizeof(kBssid));
    put32(req + 60, 1);
    put32(req + 64, key_len);
    put32(req + 68, cipher);
    memset(req + 76, 0, 64);
    memcpy(req + 76, key, key_len);
}

static void test_sizes(void)
{
    CHECK(rtw89_host_assoc_size_ok(856));
    CHECK(rtw89_host_assoc_size_ok(900));
    CHECK(rtw89_host_assoc_size_ok(908));
    CHECK(!rtw89_host_assoc_size_ok(0));
    CHECK(!rtw89_host_assoc_size_ok(855));
    CHECK(!rtw89_host_assoc_size_ok(857));
    CHECK(!rtw89_host_assoc_size_ok(899));
    CHECK(!rtw89_host_assoc_size_ok(909));
    CHECK(!rtw89_host_assoc_size_ok(276));
    CHECK(!rtw89_host_assoc_size_ok(1024));
}

static void test_wpa2_psk_every_size(void)
{
    static const uint32_t sizes[] = { 856, 900, 908 };
    static const uint8_t key[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                     17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
    uint8_t req[908];
    struct rtw89_host_assoc h;
    unsigned int i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        make_request(req, sizes[i], RTW89_HOST_AUTH_WPA2_PSK, "Golestan-5G", 0, key, sizeof(key));
        CHECK(rtw89_host_assoc_size_ok(sizes[i]));
        CHECK(rtw89_host_assoc_decode(req, &h));
        CHECK(h.upper == RTW89_HOST_AUTH_WPA2_PSK);
        CHECK(h.ssid_len == 11);
        CHECK(memcmp(h.ssid, "Golestan-5G", 11) == 0);
        CHECK(memcmp(h.bssid, kBssid, 6) == 0);
        CHECK(h.key_len == 32);
        CHECK(memcmp(h.key, key, 32) == 0);
        CHECK(h.cipher == 0);
    }
}

static void test_passphrase(void)
{
    static const uint8_t pass[] = "passphrase";
    uint8_t req[856];
    struct rtw89_host_assoc h;

    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Cafe", RTW89_HOST_CIPHER_PASSPHRASE,
                 pass, 10);
    CHECK(rtw89_host_assoc_decode(req, &h));
    CHECK(h.cipher == RTW89_HOST_CIPHER_PASSPHRASE);
    CHECK(h.key_len == 10);
    CHECK(memcmp(h.key, pass, 10) == 0);

    /* 8 to 64 characters, as WPA2 allows */
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Cafe", RTW89_HOST_CIPHER_PASSPHRASE,
                 pass, 7);
    CHECK(!rtw89_host_assoc_decode(req, &h));
    {
        uint8_t long_pass[65];

        memset(long_pass, 'x', sizeof(long_pass));
        make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Cafe",
                     RTW89_HOST_CIPHER_PASSPHRASE, long_pass, 65);
        CHECK(!rtw89_host_assoc_decode(req, &h));
        make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Cafe",
                     RTW89_HOST_CIPHER_PASSPHRASE, long_pass, 64);
        CHECK(rtw89_host_assoc_decode(req, &h));
        CHECK(h.key_len == 64);
    }
}

static void test_open_and_enterprise(void)
{
    static const uint8_t none[1] = { 0 };
    static const uint8_t key32[32] = { 0 };
    uint8_t req[900];
    struct rtw89_host_assoc h;

    make_request(req, sizeof(req), RTW89_HOST_AUTH_NONE, "Open", 0, none, 0);
    CHECK(rtw89_host_assoc_decode(req, &h));
    CHECK(h.upper == RTW89_HOST_AUTH_NONE && h.key_len == 0);
    make_request(req, sizeof(req), RTW89_HOST_AUTH_NONE, "Open", 0, key32, 32);
    CHECK(!rtw89_host_assoc_decode(req, &h));

    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2, "Corp", 0, none, 0);
    CHECK(rtw89_host_assoc_decode(req, &h));
    CHECK(h.upper == RTW89_HOST_AUTH_WPA2 && h.key_len == 0);
    make_request(req, sizeof(req), RTW89_HOST_AUTH_8021X, "Corp", 0, none, 0);
    CHECK(rtw89_host_assoc_decode(req, &h));
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2, "Corp", 0, key32, 32);
    CHECK(!rtw89_host_assoc_decode(req, &h));
}

static void test_refused(void)
{
    static const uint8_t key32[32] = { 0 };
    uint8_t req[856];
    struct rtw89_host_assoc h;

    /* the other auth types: a mix of PSK and 802.1X, SHA256-PSK alone */
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2 | RTW89_HOST_AUTH_WPA2_PSK, "Mix", 0, key32, 32);
    CHECK(!rtw89_host_assoc_decode(req, &h));
    make_request(req, sizeof(req), RTW89_HOST_AUTH_SHA256_PSK, "Mix", 0, key32, 32);
    CHECK(!rtw89_host_assoc_decode(req, &h));

    /* a key length the type does not take */
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Key", 0, key32, 16);
    CHECK(!rtw89_host_assoc_decode(req, &h));

    /* the layout: version, mode, lower auth, key version */
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Head", 0, key32, 32);
    put32(req, 2);
    CHECK(!rtw89_host_assoc_decode(req, &h));
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Head", 0, key32, 32);
    put32(req + 4, 3);
    CHECK(!rtw89_host_assoc_decode(req, &h));
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Head", 0, key32, 32);
    put32(req + 8, 0);
    CHECK(!rtw89_host_assoc_decode(req, &h));
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Head", 0, key32, 32);
    put32(req + 60, 2);
    CHECK(!rtw89_host_assoc_decode(req, &h));

    /* the SSID: length 1 to 32, no NUL byte */
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "", 0, key32, 32);
    put32(req + 16, 0);
    CHECK(!rtw89_host_assoc_decode(req, &h));
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Head", 0, key32, 32);
    put32(req + 16, 33);
    CHECK(!rtw89_host_assoc_decode(req, &h));
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Head", 0, key32, 32);
    req[23] = 0;
    CHECK(!rtw89_host_assoc_decode(req, &h));

    /* a multicast BSSID */
    make_request(req, sizeof(req), RTW89_HOST_AUTH_WPA2_PSK, "Head", 0, key32, 32);
    req[52] |= 1;
    CHECK(!rtw89_host_assoc_decode(req, &h));
}

static void test_helpers(void)
{
    const uint8_t word[4] = { 0x01, 0x02, 0x03, 0x80 };

    CHECK(rtw89_host_le32(word) == 0x80030201u);
    CHECK(rtw89_host_is_enterprise(RTW89_HOST_AUTH_WPA2));
    CHECK(rtw89_host_is_enterprise(RTW89_HOST_AUTH_8021X));
    CHECK(!rtw89_host_is_enterprise(RTW89_HOST_AUTH_WPA2 | RTW89_HOST_AUTH_WPA2_PSK));
    CHECK(!rtw89_host_is_enterprise(RTW89_HOST_AUTH_WPA2_PSK));
    CHECK(!rtw89_host_is_enterprise(RTW89_HOST_AUTH_NONE));
}

int main(void)
{
    test_sizes();
    test_wpa2_psk_every_size();
    test_passphrase();
    test_open_and_enterprise();
    test_refused();
    test_helpers();

    printf("== assoc test: %d failure(s)\n", failures);
    return failures != 0;
}
