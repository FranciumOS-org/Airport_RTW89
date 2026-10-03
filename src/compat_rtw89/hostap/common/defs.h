/* SPDX-License-Identifier: GPL-2.0 */
/* hostap's common/defs.h, reduced: the key management bits the SAE code uses. */
#ifndef RTW89_HOSTAP_DEFS_H
#define RTW89_HOSTAP_DEFS_H

#define WPA_KEY_MGMT_SAE BIT(10)
#define WPA_KEY_MGMT_FT_SAE BIT(11)
#define WPA_KEY_MGMT_SAE_EXT_KEY BIT(26)
#define WPA_KEY_MGMT_FT_SAE_EXT_KEY BIT(27)

#define WPA_PROTO_RSN BIT(1)

#endif
