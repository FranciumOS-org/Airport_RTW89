// SPDX-License-Identifier: GPL-2.0
/*
 * What the vendored hostap SAE code (third_party/hostap) needs from the rest
 * of wpa_supplicant, provided for the kext: hostap's crypto interface
 * (crypto/crypto.h) on Mbed TLS (third_party/mbedtls), its hash functions on
 * rtw89_crypto.c, and memory, randomness and logging. Built the same way for
 * the kernel and for the userspace smoke test.
 *
 * Only what SAE group 19 uses is here; the rest of crypto.h is left out.
 */
#include "includes.h"
#include "common.h"
#include "crypto/crypto.h"
#include "crypto/random.h"
#include "crypto/sha256.h"

#include "rtw89_crypto.h"

#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>

#ifdef KERNEL
#include <IOKit/IOLib.h>
#include <sys/random.h>
#else
#include <stdlib.h>
#include <stdio.h>
#endif

/* ------------------------------------------------------------------ */
/*  Memory, randomness, logging                                         */
/* ------------------------------------------------------------------ */

#ifdef KERNEL
/* IOFree() wants the size back: keep it in front of the block. */
struct alloc_head {
    size_t size;
    size_t pad;         /* 16-byte alignment for what follows */
};

void *rtw89_hostap_malloc(size_t size)
{
    struct alloc_head *h;

    if (size > (64U << 20))
        return NULL;
    h = (struct alloc_head *)IOMalloc(sizeof(*h) + size);
    if (!h)
        return NULL;
    h->size = size;
    return h + 1;
}

void rtw89_hostap_free(void *ptr)
{
    struct alloc_head *h;

    if (!ptr)
        return;
    h = (struct alloc_head *)ptr - 1;
    IOFree(h, sizeof(*h) + h->size);
}
#else
void *rtw89_hostap_malloc(size_t size)
{
    return malloc(size);
}

void rtw89_hostap_free(void *ptr)
{
    free(ptr);
}
#endif

void *rtw89_hostap_zalloc(size_t size)
{
    void *p = rtw89_hostap_malloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void *rtw89_hostap_calloc(size_t n, size_t size)
{
    if (size && n > (~(size_t)0) / size)
        return NULL;
    return rtw89_hostap_zalloc(n * size);
}

void *rtw89_hostap_realloc(void *ptr, size_t size)
{
    void *n;

#ifdef KERNEL
    size_t old = ptr ? ((struct alloc_head *)ptr - 1)->size : 0;
#endif

    if (!ptr)
        return rtw89_hostap_malloc(size);
#ifdef KERNEL
    n = rtw89_hostap_malloc(size);
    if (!n)
        return NULL;
    memcpy(n, ptr, old < size ? old : size);
    rtw89_hostap_free(ptr);
    return n;
#else
    n = realloc(ptr, size);
    return n;
#endif
}

int rtw89_hostap_memcmp_const(const void *a, const void *b, size_t len)
{
    return rtw89_crypto_equal(a, b, len) ? 0 : 1;
}

int random_get_bytes(void *buf, size_t len)
{
#ifdef KERNEL
    read_random(buf, (u_int)len);
#else
    arc4random_buf(buf, len);
#endif
    return 0;
}

void rtw89_hostap_printf(int level, const char *fmt, ...)
{
#ifdef KERNEL
    char line[256];
#endif
    va_list ap;

    if (level < MSG_INFO)
        return;
    va_start(ap, fmt);
#ifdef KERNEL
    vsnprintf(line, sizeof(line), fmt, ap);
    IOLog("[rtw89 sae] %s\n", line);
#else
    printf("[rtw89 sae] ");
    vprintf(fmt, ap);
    printf("\n");
#endif
    va_end(ap);
}

/* Mbed TLS's random source, for its blinding and for crypto_bignum_rand(). */
static int mbedtls_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    return random_get_bytes(out, len);
}

/* ------------------------------------------------------------------ */
/*  Hashes: on rtw89_crypto.c                                           */
/* ------------------------------------------------------------------ */

int hmac_sha256_vector(const u8 *key, size_t key_len, size_t num_elem,
                       const u8 *addr[], const size_t *len, u8 *mac)
{
    rtw89_hmac_sha256_vector(key, key_len, num_elem, addr, len, mac);
    return 0;
}

int hmac_sha256(const u8 *key, size_t key_len, const u8 *data, size_t data_len, u8 *mac)
{
    return hmac_sha256_vector(key, key_len, 1, &data, &data_len, mac);
}

/* IEEE 802.11 KDF with the output length counted in bits. */
int sha256_prf_bits(const u8 *key, size_t key_len, const char *label,
                    const u8 *data, size_t data_len, u8 *buf, size_t buf_len_bits)
{
    size_t buf_len = (buf_len_bits + 7) / 8, pos = 0, label_len = strlen(label);
    u16 counter = 1;
    u8 length[2] = { (u8)buf_len_bits, (u8)(buf_len_bits >> 8) };
    u8 hash[SHA256_MAC_LEN];

    while (pos < buf_len) {
        u8 count[2] = { (u8)counter, (u8)(counter >> 8) };
        const u8 *addr[4] = { count, (const u8 *)label, data, length };
        size_t len[4] = { 2, label_len, data_len, 2 };
        size_t n = buf_len - pos < SHA256_MAC_LEN ? buf_len - pos : SHA256_MAC_LEN;

        rtw89_hmac_sha256_vector(key, key_len, 4, addr, len, hash);
        memcpy(buf + pos, hash, n);
        pos += n;
        counter++;
    }
    /* the bits beyond the requested length are zero */
    if (buf_len_bits % 8)
        buf[buf_len - 1] &= (u8)(0xff << (8 - buf_len_bits % 8));
    forced_memzero(hash, sizeof(hash));
    return 0;
}

int sha256_prf(const u8 *key, size_t key_len, const char *label,
               const u8 *data, size_t data_len, u8 *buf, size_t buf_len)
{
    return sha256_prf_bits(key, key_len, label, data, data_len, buf, buf_len * 8);
}

int hkdf_extract(size_t hash_len, const u8 *salt, size_t salt_len, size_t num_elem,
                 const u8 *addr[], const size_t len[], u8 *prk)
{
    static const u8 zero[SHA256_MAC_LEN];

    if (hash_len != SHA256_MAC_LEN)
        return -1;
    if (!salt || !salt_len) {
        salt = zero;
        salt_len = sizeof(zero);
    }
    rtw89_hmac_sha256_vector(salt, salt_len, num_elem, addr, len, prk);
    return 0;
}

int hkdf_expand(size_t hash_len, const u8 *prk, size_t prk_len, const char *info,
                u8 *okm, size_t okm_len)
{
    size_t info_len = info ? strlen(info) : 0, pos = 0, tlen = 0;
    u8 t[SHA256_MAC_LEN], counter = 1;

    if (hash_len != SHA256_MAC_LEN || okm_len > 255 * SHA256_MAC_LEN)
        return -1;
    while (pos < okm_len) {
        const u8 *addr[3] = { t, (const u8 *)info, &counter };
        size_t len[3] = { tlen, info_len, 1 };
        size_t n = okm_len - pos < SHA256_MAC_LEN ? okm_len - pos : SHA256_MAC_LEN;

        rtw89_hmac_sha256_vector(prk, prk_len, 3, addr, len, t);
        tlen = SHA256_MAC_LEN;
        memcpy(okm + pos, t, n);
        pos += n;
        counter++;
    }
    forced_memzero(t, sizeof(t));
    return 0;
}

/* ------------------------------------------------------------------ */
/*  crypto_bignum: an mbedtls_mpi                                        */
/* ------------------------------------------------------------------ */

#define MPI(b) ((mbedtls_mpi *)(b))
#define CMPI(b) ((const mbedtls_mpi *)(b))

struct crypto_bignum *crypto_bignum_init(void)
{
    mbedtls_mpi *n = os_zalloc(sizeof(*n));

    if (n)
        mbedtls_mpi_init(n);
    return (struct crypto_bignum *)n;
}

struct crypto_bignum *crypto_bignum_init_set(const u8 *buf, size_t len)
{
    struct crypto_bignum *n = crypto_bignum_init();

    if (n && mbedtls_mpi_read_binary(MPI(n), buf, len)) {
        crypto_bignum_deinit(n, 1);
        return NULL;
    }
    return n;
}

struct crypto_bignum *crypto_bignum_init_uint(unsigned int val)
{
    struct crypto_bignum *n = crypto_bignum_init();

    if (n && mbedtls_mpi_lset(MPI(n), (mbedtls_mpi_sint)val)) {
        crypto_bignum_deinit(n, 1);
        return NULL;
    }
    return n;
}

void crypto_bignum_deinit(struct crypto_bignum *n, int clear)
{
    (void)clear;        /* mbedtls_mpi_free() always clears */
    if (!n)
        return;
    mbedtls_mpi_free(MPI(n));
    os_free(n);
}

int crypto_bignum_to_bin(const struct crypto_bignum *a, u8 *buf, size_t buflen, size_t padlen)
{
    size_t len = mbedtls_mpi_size(CMPI(a));

    if (padlen > buflen)
        return -1;
    if (padlen > len)
        len = padlen;
    if (len > buflen)
        return -1;
    if (mbedtls_mpi_write_binary(CMPI(a), buf, len))
        return -1;
    return (int)len;
}

int crypto_bignum_rand(struct crypto_bignum *r, const struct crypto_bignum *m)
{
    return mbedtls_mpi_random(MPI(r), 0, CMPI(m), mbedtls_rng, NULL) ? -1 : 0;
}

int crypto_bignum_add(const struct crypto_bignum *a, const struct crypto_bignum *b,
                      struct crypto_bignum *c)
{
    return mbedtls_mpi_add_mpi(MPI(c), CMPI(a), CMPI(b)) ? -1 : 0;
}

int crypto_bignum_mod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                      struct crypto_bignum *c)
{
    return mbedtls_mpi_mod_mpi(MPI(c), CMPI(a), CMPI(b)) ? -1 : 0;
}

int crypto_bignum_exptmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                          const struct crypto_bignum *c, struct crypto_bignum *d)
{
    return mbedtls_mpi_exp_mod(MPI(d), CMPI(a), CMPI(b), CMPI(c), NULL) ? -1 : 0;
}

int crypto_bignum_inverse(const struct crypto_bignum *a, const struct crypto_bignum *b,
                          struct crypto_bignum *c)
{
    return mbedtls_mpi_inv_mod(MPI(c), CMPI(a), CMPI(b)) ? -1 : 0;
}

int crypto_bignum_sub(const struct crypto_bignum *a, const struct crypto_bignum *b,
                      struct crypto_bignum *c)
{
    return mbedtls_mpi_sub_mpi(MPI(c), CMPI(a), CMPI(b)) ? -1 : 0;
}

int crypto_bignum_div(const struct crypto_bignum *a, const struct crypto_bignum *b,
                      struct crypto_bignum *c)
{
    return mbedtls_mpi_div_mpi(MPI(c), NULL, CMPI(a), CMPI(b)) ? -1 : 0;
}

int crypto_bignum_addmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                         const struct crypto_bignum *c, struct crypto_bignum *d)
{
    if (mbedtls_mpi_add_mpi(MPI(d), CMPI(a), CMPI(b)))
        return -1;
    return mbedtls_mpi_mod_mpi(MPI(d), MPI(d), CMPI(c)) ? -1 : 0;
}

int crypto_bignum_mulmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                         const struct crypto_bignum *c, struct crypto_bignum *d)
{
    if (mbedtls_mpi_mul_mpi(MPI(d), CMPI(a), CMPI(b)))
        return -1;
    return mbedtls_mpi_mod_mpi(MPI(d), MPI(d), CMPI(c)) ? -1 : 0;
}

int crypto_bignum_sqrmod(const struct crypto_bignum *a, const struct crypto_bignum *b,
                         struct crypto_bignum *c)
{
    return crypto_bignum_mulmod(a, a, b, c);
}

int crypto_bignum_rshift(const struct crypto_bignum *a, int n, struct crypto_bignum *r)
{
    if (mbedtls_mpi_copy(MPI(r), CMPI(a)))
        return -1;
    return mbedtls_mpi_shift_r(MPI(r), (size_t)n) ? -1 : 0;
}

int crypto_bignum_cmp(const struct crypto_bignum *a, const struct crypto_bignum *b)
{
    return mbedtls_mpi_cmp_mpi(CMPI(a), CMPI(b));
}

int crypto_bignum_is_zero(const struct crypto_bignum *a)
{
    return mbedtls_mpi_cmp_int(CMPI(a), 0) == 0;
}

int crypto_bignum_is_one(const struct crypto_bignum *a)
{
    return mbedtls_mpi_cmp_int(CMPI(a), 1) == 0;
}

int crypto_bignum_is_odd(const struct crypto_bignum *a)
{
    return mbedtls_mpi_get_bit(CMPI(a), 0);
}

/* 1 for a quadratic residue mod p, -1 for a non-residue, 0 for 0; -2 on error */
int crypto_bignum_legendre(const struct crypto_bignum *a, const struct crypto_bignum *p)
{
    mbedtls_mpi exp, t;
    int ret = -2;

    mbedtls_mpi_init(&exp);
    mbedtls_mpi_init(&t);
    /* a^((p-1)/2) mod p */
    if (mbedtls_mpi_sub_int(&exp, CMPI(p), 1) || mbedtls_mpi_shift_r(&exp, 1) ||
        mbedtls_mpi_exp_mod(&t, CMPI(a), &exp, CMPI(p), NULL))
        goto out;
    if (mbedtls_mpi_cmp_int(&t, 1) == 0) {
        ret = 1;
    } else if (mbedtls_mpi_cmp_int(&t, 0) == 0) {
        ret = 0;
    } else {
        if (mbedtls_mpi_add_int(&t, &t, 1))
            goto out;
        ret = mbedtls_mpi_cmp_mpi(&t, CMPI(p)) == 0 ? -1 : -2;
    }
out:
    mbedtls_mpi_free(&exp);
    mbedtls_mpi_free(&t);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  crypto_ec: an mbedtls_ecp_group, points as mbedtls_ecp_point       */
/* ------------------------------------------------------------------ */

struct crypto_ec {
    mbedtls_ecp_group group;
    mbedtls_mpi a;      /* Mbed TLS keeps A empty for a = -3: here p - 3 */
};

#define PT(p) ((mbedtls_ecp_point *)(p))
#define CPT(p) ((const mbedtls_ecp_point *)(p))

struct crypto_ec *crypto_ec_init(int group)
{
    struct crypto_ec *e;

    if (group != 19)
        return NULL;
    e = os_zalloc(sizeof(*e));
    if (!e)
        return NULL;
    mbedtls_ecp_group_init(&e->group);
    mbedtls_mpi_init(&e->a);
    if (mbedtls_ecp_group_load(&e->group, MBEDTLS_ECP_DP_SECP256R1) ||
        mbedtls_mpi_sub_int(&e->a, &e->group.P, 3)) {
        crypto_ec_deinit(e);
        return NULL;
    }
    return e;
}

void crypto_ec_deinit(struct crypto_ec *e)
{
    if (!e)
        return;
    mbedtls_mpi_free(&e->a);
    mbedtls_ecp_group_free(&e->group);
    os_free(e);
}

size_t crypto_ec_prime_len(struct crypto_ec *e)
{
    return mbedtls_mpi_size(&e->group.P);
}

size_t crypto_ec_prime_len_bits(struct crypto_ec *e)
{
    return mbedtls_mpi_bitlen(&e->group.P);
}

size_t crypto_ec_order_len(struct crypto_ec *e)
{
    return mbedtls_mpi_size(&e->group.N);
}

const struct crypto_bignum *crypto_ec_get_prime(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->group.P;
}

const struct crypto_bignum *crypto_ec_get_order(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->group.N;
}

const struct crypto_bignum *crypto_ec_get_a(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->a;
}

const struct crypto_bignum *crypto_ec_get_b(struct crypto_ec *e)
{
    return (const struct crypto_bignum *)&e->group.B;
}

struct crypto_ec_point *crypto_ec_point_init(struct crypto_ec *e)
{
    mbedtls_ecp_point *p = os_zalloc(sizeof(*p));

    (void)e;
    if (p)
        mbedtls_ecp_point_init(p);
    return (struct crypto_ec_point *)p;
}

void crypto_ec_point_deinit(struct crypto_ec_point *p, int clear)
{
    (void)clear;
    if (!p)
        return;
    mbedtls_ecp_point_free(PT(p));
    os_free(p);
}

int crypto_ec_point_to_bin(struct crypto_ec *e, const struct crypto_ec_point *point,
                           u8 *x, u8 *y)
{
    size_t len = crypto_ec_prime_len(e);

    if (x && mbedtls_mpi_write_binary(&CPT(point)->MBEDTLS_PRIVATE(X), x, len))
        return -1;
    if (y && mbedtls_mpi_write_binary(&CPT(point)->MBEDTLS_PRIVATE(Y), y, len))
        return -1;
    return 0;
}

struct crypto_ec_point *crypto_ec_point_from_bin(struct crypto_ec *e, const u8 *val)
{
    size_t len = crypto_ec_prime_len(e);
    struct crypto_ec_point *p = crypto_ec_point_init(e);

    if (!p)
        return NULL;
    if (mbedtls_mpi_read_binary(&PT(p)->MBEDTLS_PRIVATE(X), val, len) ||
        mbedtls_mpi_read_binary(&PT(p)->MBEDTLS_PRIVATE(Y), val + len, len) ||
        mbedtls_mpi_lset(&PT(p)->MBEDTLS_PRIVATE(Z), 1)) {
        crypto_ec_point_deinit(p, 1);
        return NULL;
    }
    return p;
}

int crypto_ec_point_add(struct crypto_ec *e, const struct crypto_ec_point *a,
                        const struct crypto_ec_point *b, struct crypto_ec_point *c)
{
    mbedtls_mpi one;
    int ret;

    mbedtls_mpi_init(&one);
    ret = mbedtls_mpi_lset(&one, 1) ||
          mbedtls_ecp_muladd(&e->group, PT(c), &one, CPT(a), &one, CPT(b)) ? -1 : 0;
    mbedtls_mpi_free(&one);
    return ret;
}

int crypto_ec_point_mul(struct crypto_ec *e, const struct crypto_ec_point *p,
                        const struct crypto_bignum *b, struct crypto_ec_point *res)
{
    mbedtls_mpi k;
    int ret;

    /* Mbed TLS takes multipliers below the group order only */
    mbedtls_mpi_init(&k);
    ret = mbedtls_mpi_mod_mpi(&k, CMPI(b), &e->group.N) ||
          mbedtls_ecp_mul(&e->group, PT(res), &k, CPT(p), mbedtls_rng, NULL) ? -1 : 0;
    mbedtls_mpi_free(&k);
    return ret;
}

int crypto_ec_point_invert(struct crypto_ec *e, struct crypto_ec_point *p)
{
    mbedtls_mpi *y = &PT(p)->MBEDTLS_PRIVATE(Y);

    if (mbedtls_mpi_cmp_int(y, 0) == 0)
        return 0;
    return mbedtls_mpi_sub_mpi(y, &e->group.P, y) ? -1 : 0;
}

struct crypto_bignum *crypto_ec_point_compute_y_sqr(struct crypto_ec *e,
                                                    const struct crypto_bignum *x)
{
    struct crypto_bignum *y2 = crypto_bignum_init();
    mbedtls_mpi t;
    int bad;

    if (!y2)
        return NULL;
    mbedtls_mpi_init(&t);
    /* y^2 = x^3 + a x + b mod p */
    bad = mbedtls_mpi_mul_mpi(&t, CMPI(x), CMPI(x)) ||
          mbedtls_mpi_mod_mpi(&t, &t, &e->group.P) ||
          mbedtls_mpi_add_mpi(&t, &t, &e->a) ||
          mbedtls_mpi_mul_mpi(MPI(y2), &t, CMPI(x)) ||
          mbedtls_mpi_add_mpi(MPI(y2), MPI(y2), &e->group.B) ||
          mbedtls_mpi_mod_mpi(MPI(y2), MPI(y2), &e->group.P);
    mbedtls_mpi_free(&t);
    if (bad) {
        crypto_bignum_deinit(y2, 1);
        return NULL;
    }
    return y2;
}

int crypto_ec_point_is_at_infinity(struct crypto_ec *e, const struct crypto_ec_point *p)
{
    (void)e;
    return mbedtls_ecp_is_zero((mbedtls_ecp_point *)CPT(p));
}

int crypto_ec_point_is_on_curve(struct crypto_ec *e, const struct crypto_ec_point *p)
{
    return mbedtls_ecp_check_pubkey(&e->group, CPT(p)) == 0;
}

int crypto_ec_point_cmp(const struct crypto_ec *e, const struct crypto_ec_point *a,
                        const struct crypto_ec_point *b)
{
    (void)e;
    return mbedtls_ecp_point_cmp(CPT(a), CPT(b));
}
