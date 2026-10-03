/* SPDX-License-Identifier: GPL-2.0 */
/* hostap's crypto/dh_groups.h, reduced: no finite field groups here, only the
 * elliptic curve group 19, so dh_groups_get() never finds one. */
#ifndef RTW89_HOSTAP_DH_GROUPS_H
#define RTW89_HOSTAP_DH_GROUPS_H

struct dh_group {
    int id;
    const u8 *generator;
    size_t generator_len;
    const u8 *prime;
    size_t prime_len;
    const u8 *order;
    size_t order_len;
    unsigned int safe_prime:1;
};

static inline const struct dh_group *dh_groups_get(int id)
{
    (void)id;
    return NULL;
}

#endif
