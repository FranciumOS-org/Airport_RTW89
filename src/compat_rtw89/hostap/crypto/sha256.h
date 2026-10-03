/* SPDX-License-Identifier: GPL-2.0 */
/* hostap's crypto/sha256.h, reduced; implemented in ../rtw89_hostap.c. */
#ifndef RTW89_HOSTAP_SHA256_H
#define RTW89_HOSTAP_SHA256_H

#define SHA256_MAC_LEN 32

int hmac_sha256_vector(const u8 *key, size_t key_len, size_t num_elem,
                       const u8 *addr[], const size_t *len, u8 *mac);
int hmac_sha256(const u8 *key, size_t key_len, const u8 *data, size_t data_len, u8 *mac);
int sha256_prf(const u8 *key, size_t key_len, const char *label,
               const u8 *data, size_t data_len, u8 *buf, size_t buf_len);
int sha256_prf_bits(const u8 *key, size_t key_len, const char *label,
                    const u8 *data, size_t data_len, u8 *buf, size_t buf_len_bits);
/* RFC 5869 with HMAC-SHA256 (@hash_len must be 32 here) */
int hkdf_extract(size_t hash_len, const u8 *salt, size_t salt_len, size_t num_elem,
                 const u8 *addr[], const size_t len[], u8 *prk);
int hkdf_expand(size_t hash_len, const u8 *prk, size_t prk_len, const char *info,
                u8 *okm, size_t okm_len);

#endif
