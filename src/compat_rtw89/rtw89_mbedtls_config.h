/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mbed TLS configuration for the kext (MBEDTLS_CONFIG_FILE): only the bignum
 * and elliptic-curve modules, and only the P-256 curve, which is all SAE
 * group 19 needs. Memory comes from calloc()/free(), which the kernel build
 * maps to rtw89_hostap_calloc()/rtw89_hostap_free() (Makefile).
 */
#ifndef RTW89_MBEDTLS_CONFIG_H
#define RTW89_MBEDTLS_CONFIG_H

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
/* NIST-specific reduction: faster, and the curve's own code */
#define MBEDTLS_ECP_NIST_OPTIM
/* MBEDTLS_HAVE_ASM stays off: plain C only in the kernel */
/* no 128-bit division: the kernel does not export __udivti3 */
#define MBEDTLS_NO_UDBL_DIVISION

#endif
