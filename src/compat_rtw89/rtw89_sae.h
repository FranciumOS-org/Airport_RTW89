/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The MLME's way into hostap's SAE code (WPA3-Personal, group 19). The MLME
 * is built against the Linux headers and hostap against its own, whose basic
 * types clash; this interface uses neither. rtw89_sae.c, built with hostap.
 *
 * Message bodies are what follows the Authentication frame's algorithm,
 * sequence number and status code fields.
 */
#ifndef _RTW89_SAE_H
#define _RTW89_SAE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rtw89_sae;

/* Password element and own commit for this station and access point. NULL
 * if there is no memory or the password makes no element. @h2e: the element
 * by hash-to-element (from the network name too; 12.4.4.2.3), which 6 GHz
 * and some access points require, rather than by hunting and pecking. */
struct rtw89_sae *rtw89_sae_begin(const uint8_t own[6], const uint8_t peer[6],
                                  const uint8_t *ssid, size_t ssid_len,
                                  const uint8_t *password, size_t password_len, bool h2e);
void rtw89_sae_end(struct rtw89_sae *sae);

/* Own commit, with the anti-clogging token the AP asked for (or none).
 * Returns the body length, or negative. */
int rtw89_sae_write_commit(struct rtw89_sae *sae, const uint8_t *token, size_t token_len,
                           uint8_t *out, size_t max);
/* The AP's commit (status 0): checked, and the keys derived. Returns 0, or the
 * 802.11 status code to give up with. */
int rtw89_sae_rx_commit(struct rtw89_sae *sae, const uint8_t *body, size_t len);
/* Own confirm. Returns the body length, or negative. */
int rtw89_sae_write_confirm(struct rtw89_sae *sae, uint8_t *out, size_t max);
/* The AP's confirm: 0 if it proves the AP has the same password. */
int rtw89_sae_rx_confirm(struct rtw89_sae *sae, const uint8_t *body, size_t len);
/* After a good confirm: the 32-byte PMK and the 16-byte PMKID. */
void rtw89_sae_keys(const struct rtw89_sae *sae, uint8_t pmk[32], uint8_t pmkid[16]);

#ifdef __cplusplus
}
#endif

#endif /* _RTW89_SAE_H */
