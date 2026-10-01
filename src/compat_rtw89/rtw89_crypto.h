/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The cryptography WPA2-PSK key management needs (IEEE 802.11 12.7): SHA-1,
 * HMAC-SHA1, PBKDF2 for the passphrase, the 802.11 PRF for the pairwise keys,
 * and AES key wrap for the group key. Plain C with no Linux or kernel types,
 * so the userspace smoke test can use the same code to play the authenticator.
 *
 * Frame encryption itself (CCMP) is done by the hardware.
 */
#ifndef _RTW89_CRYPTO_H
#define _RTW89_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTW89_SHA1_LEN 20

void rtw89_sha1(const uint8_t *data, size_t len, uint8_t out[RTW89_SHA1_LEN]);

/* HMAC-SHA1 over the concatenation of @num pieces. */
void rtw89_hmac_sha1_vector(const uint8_t *key, size_t key_len, size_t num,
                            const uint8_t *const pieces[], const size_t lens[],
                            uint8_t out[RTW89_SHA1_LEN]);
void rtw89_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                     uint8_t out[RTW89_SHA1_LEN]);

/* PBKDF2-HMAC-SHA1 (RFC 2898). WPA2: 4096 iterations, salt = SSID, 32 bytes out. */
void rtw89_pbkdf2_sha1(const uint8_t *password, size_t password_len,
                       const uint8_t *salt, size_t salt_len, unsigned int iterations,
                       uint8_t *out, size_t out_len);

/* IEEE 802.11 PRF: HMAC-SHA1(key, label || 0 || data || counter), concatenated. */
void rtw89_sha1_prf(const uint8_t *key, size_t key_len, const char *label,
                    const uint8_t *data, size_t data_len, uint8_t *out, size_t out_len);

void rtw89_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void rtw89_aes128_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* AES key wrap (RFC 3394) with a 128-bit KEK. @plain_len is a multiple of 8,
 * at least 16; the wrapped form is 8 bytes longer. unwrap returns false if the
 * integrity check fails. */
void rtw89_aes_wrap(const uint8_t kek[16], const uint8_t *plain, size_t plain_len,
                    uint8_t *wrapped);
bool rtw89_aes_unwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t wrapped_len,
                      uint8_t *plain);

/* Compare without leaking where the difference is. */
bool rtw89_crypto_equal(const uint8_t *a, const uint8_t *b, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _RTW89_CRYPTO_H */
