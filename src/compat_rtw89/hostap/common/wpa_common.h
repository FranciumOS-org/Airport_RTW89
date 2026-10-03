/* SPDX-License-Identifier: GPL-2.0 */
/* hostap's common/wpa_common.h, reduced to what the SAE code uses. */
#ifndef RTW89_HOSTAP_WPA_COMMON_H
#define RTW89_HOSTAP_WPA_COMMON_H

#include "common/defs.h"

#define PMKID_LEN 16
#define RSN_SELECTOR_LEN 4
#define RSN_SELECTOR(a, b, c, d) \
    ((((u32) (a)) << 24) | (((u32) (b)) << 16) | (((u32) (c)) << 8) | (u32) (d))
#define RSN_SELECTOR_PUT(a, val) WPA_PUT_BE32((u8 *) (a), (val))
#define RSN_SELECTOR_GET(a) WPA_GET_BE32((const u8 *) (a))
#define RSN_AUTH_KEY_MGMT_SAE RSN_SELECTOR(0x00, 0x0f, 0xac, 8)
#define RSN_AUTH_KEY_MGMT_FT_SAE RSN_SELECTOR(0x00, 0x0f, 0xac, 9)
#define RSN_AUTH_KEY_MGMT_SAE_EXT_KEY RSN_SELECTOR(0x00, 0x0f, 0xac, 24)
#define RSN_AUTH_KEY_MGMT_FT_SAE_EXT_KEY RSN_SELECTOR(0x00, 0x0f, 0xac, 25)

static inline int wpa_key_mgmt_sae_ext_key(int akm)
{
    return !!(akm & (WPA_KEY_MGMT_SAE_EXT_KEY | WPA_KEY_MGMT_FT_SAE_EXT_KEY));
}

static inline u32 wpa_akm_to_suite(int akm)
{
    if (akm & WPA_KEY_MGMT_FT_SAE_EXT_KEY)
        return RSN_AUTH_KEY_MGMT_FT_SAE_EXT_KEY;
    if (akm & WPA_KEY_MGMT_SAE_EXT_KEY)
        return RSN_AUTH_KEY_MGMT_SAE_EXT_KEY;
    if (akm & WPA_KEY_MGMT_FT_SAE)
        return RSN_AUTH_KEY_MGMT_FT_SAE;
    if (akm & WPA_KEY_MGMT_SAE)
        return RSN_AUTH_KEY_MGMT_SAE;
    return 0;
}

static inline const char *wpa_key_mgmt_txt(int akm, int proto)
{
    (void)proto;
    return wpa_key_mgmt_sae_ext_key(akm) ? "SAE-EXT-KEY" : "SAE";
}

#endif
