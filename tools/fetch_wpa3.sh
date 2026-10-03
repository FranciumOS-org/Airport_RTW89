#!/bin/sh
# Download the unmodified upstream code WPA3 (SAE) is built from, at pinned
# commits (approved by the user, 2026-10-02; the last eleven Mbed TLS headers
# and the bignum_mod files in
# second and third approvals the same day):
#
#   third_party/hostap/    hostap (wpa_supplicant/hostapd): the SAE protocol.
#                          BSD-3-Clause.
#   third_party/mbedtls/   Mbed TLS 3.6 LTS: bignum and elliptic-curve code.
#                          Apache-2.0 OR GPL-2.0-or-later (used under GPL-2.0).
#
# Usage: tools/fetch_wpa3.sh
set -eu
hostap=5b156e272a0266ca6be0f394192bad42b0ff176c
mbedtls=1ee94c5a1147
cd "$(dirname "$0")/.."

hfetch() { # <path in hostap>
    mkdir -p "third_party/hostap/$(dirname "$1")"
    curl -fsSL -m 120 -o "third_party/hostap/$1" \
        "https://git.w1.fi/cgit/hostap/plain/$1?id=$hostap"
    echo "  $(wc -c < "third_party/hostap/$1") third_party/hostap/$1"
}
mfetch() { # <path in mbedtls>
    mkdir -p "third_party/mbedtls/$(dirname "$1")"
    curl -fsS -m 120 -H 'Accept: application/vnd.github.raw' -o "third_party/mbedtls/$1" \
        "https://api.github.com/repos/Mbed-TLS/mbedtls/contents/$1?ref=$mbedtls"
    echo "  $(wc -c < "third_party/mbedtls/$1") third_party/mbedtls/$1"
}

for f in COPYING README \
         src/common/sae.c src/common/sae.h src/common/dragonfly.c src/common/dragonfly.h \
         src/common/ieee802_11_defs.h src/crypto/crypto.h \
         src/utils/common.h src/utils/const_time.h src/utils/includes.h \
         src/utils/wpabuf.h src/utils/wpabuf.c; do
    hfetch $f
done

for f in LICENSE \
         library/bignum.c library/bignum_core.c library/bignum_core.h library/bn_mul.h \
         library/bignum_internal.h library/ecp.c library/ecp_curves.c \
         library/ecp_internal_alt.h library/constant_time.c library/constant_time_impl.h \
         library/constant_time_internal.h library/platform_util.c library/common.h \
         library/alignment.h \
         include/mbedtls/bignum.h include/mbedtls/ecp.h include/mbedtls/build_info.h \
         include/mbedtls/mbedtls_config.h include/mbedtls/platform_util.h \
         include/mbedtls/private_access.h include/mbedtls/check_config.h \
         include/mbedtls/config_adjust_legacy_crypto.h \
         include/mbedtls/config_adjust_legacy_from_psa.h \
         include/mbedtls/config_adjust_psa_from_legacy.h \
         include/mbedtls/config_adjust_psa_superset_legacy.h \
         include/mbedtls/config_adjust_ssl.h include/mbedtls/config_adjust_x509.h \
         include/mbedtls/config_psa.h include/mbedtls/platform.h \
         include/mbedtls/platform_time.h \
         library/bignum_core_invasive.h library/ecp_invasive.h \
         include/mbedtls/constant_time.h include/mbedtls/error.h include/mbedtls/threading.h \
         include/psa/crypto_adjust_auto_enabled.h \
         include/psa/crypto_adjust_config_dependencies.h \
         include/psa/crypto_adjust_config_key_pair_types.h \
         include/psa/crypto_adjust_config_synonyms.h \
         include/psa/crypto_config.h include/psa/crypto_legacy.h \
         library/bignum_mod.h library/bignum_mod_raw.h library/bignum_mod_raw_invasive.h \
         library/bignum_mod.c library/bignum_mod_raw.c; do
    mfetch $f
done
