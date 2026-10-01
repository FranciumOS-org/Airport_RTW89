// SPDX-License-Identifier: GPL-2.0
/* See rtw89_crypto.h. Written from FIPS 180-4, RFC 2104, RFC 2898, FIPS 197
 * and RFC 3394; checked against their test vectors in tools/hosttest. */
#include "rtw89_crypto.h"

/* The kernel and libc both provide these; no header is common to both. */
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);

/* ------------------------------------------------------------------ */
/*  SHA-1                                                               */
/* ------------------------------------------------------------------ */

struct sha1_ctx {
    uint32_t h[5];
    uint64_t len;           /* bytes hashed so far */
    uint8_t block[64];
    size_t fill;
};

static uint32_t rol32(uint32_t v, unsigned int n)
{
    return (v << n) | (v >> (32 - n));
}

static void sha1_compress(struct sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80], a, b, cc, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ cc ^ d;
            k = 0xca62c1d6;
        }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_init(struct sha1_ctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xefcdab89; c->h[2] = 0x98badcfe;
    c->h[3] = 0x10325476; c->h[4] = 0xc3d2e1f0;
    c->len = 0;
    c->fill = 0;
}

static void sha1_update(struct sha1_ctx *c, const uint8_t *data, size_t len)
{
    c->len += len;
    while (len) {
        size_t n = 64 - c->fill;

        if (n > len)
            n = len;
        memcpy(c->block + c->fill, data, n);
        c->fill += n;
        data += n;
        len -= n;
        if (c->fill == 64) {
            sha1_compress(c, c->block);
            c->fill = 0;
        }
    }
}

static void sha1_final(struct sha1_ctx *c, uint8_t out[RTW89_SHA1_LEN])
{
    uint64_t bits = c->len * 8;
    uint8_t pad[72];
    size_t padlen = (c->fill < 56 ? 56 : 120) - c->fill;
    int i;

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    for (i = 0; i < 8; i++)
        pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, pad, padlen + 8);

    for (i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void rtw89_sha1(const uint8_t *data, size_t len, uint8_t out[RTW89_SHA1_LEN])
{
    struct sha1_ctx c;

    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* ------------------------------------------------------------------ */
/*  HMAC-SHA1, PBKDF2, PRF                                              */
/* ------------------------------------------------------------------ */

void rtw89_hmac_sha1_vector(const uint8_t *key, size_t key_len, size_t num,
                            const uint8_t *const pieces[], const size_t lens[],
                            uint8_t out[RTW89_SHA1_LEN])
{
    uint8_t k[64], pad[64], inner[RTW89_SHA1_LEN];
    struct sha1_ctx c;
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64)
        rtw89_sha1(key, key_len, k);
    else
        memcpy(k, key, key_len);

    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    for (i = 0; i < num; i++)
        sha1_update(&c, pieces[i], lens[i]);
    sha1_final(&c, inner);

    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha1_init(&c);
    sha1_update(&c, pad, 64);
    sha1_update(&c, inner, sizeof(inner));
    sha1_final(&c, out);

    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
}

void rtw89_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                     uint8_t out[RTW89_SHA1_LEN])
{
    rtw89_hmac_sha1_vector(key, key_len, 1, &data, &len, out);
}

void rtw89_pbkdf2_sha1(const uint8_t *password, size_t password_len,
                       const uint8_t *salt, size_t salt_len, unsigned int iterations,
                       uint8_t *out, size_t out_len)
{
    uint32_t block = 1;

    while (out_len) {
        uint8_t counter[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16),
                               (uint8_t)(block >> 8), (uint8_t)block };
        const uint8_t *pieces[2] = { salt, counter };
        size_t lens[2] = { salt_len, sizeof(counter) };
        uint8_t u[RTW89_SHA1_LEN], t[RTW89_SHA1_LEN];
        size_t n = out_len < RTW89_SHA1_LEN ? out_len : RTW89_SHA1_LEN;
        unsigned int i, j;

        rtw89_hmac_sha1_vector(password, password_len, 2, pieces, lens, u);
        memcpy(t, u, sizeof(t));
        for (i = 1; i < iterations; i++) {
            rtw89_hmac_sha1(password, password_len, u, sizeof(u), u);
            for (j = 0; j < RTW89_SHA1_LEN; j++)
                t[j] ^= u[j];
        }
        memcpy(out, t, n);
        out += n;
        out_len -= n;
        block++;
    }
}

void rtw89_sha1_prf(const uint8_t *key, size_t key_len, const char *label,
                    const uint8_t *data, size_t data_len, uint8_t *out, size_t out_len)
{
    size_t label_len = 0;
    uint8_t counter = 0;

    while (label[label_len])
        label_len++;

    while (out_len) {
        /* the label's terminating NUL is part of the input */
        const uint8_t *pieces[3] = { (const uint8_t *)label, data, &counter };
        size_t lens[3] = { label_len + 1, data_len, 1 };
        uint8_t hash[RTW89_SHA1_LEN];
        size_t n = out_len < RTW89_SHA1_LEN ? out_len : RTW89_SHA1_LEN;

        rtw89_hmac_sha1_vector(key, key_len, 3, pieces, lens, hash);
        memcpy(out, hash, n);
        out += n;
        out_len -= n;
        counter++;
    }
}

/* ------------------------------------------------------------------ */
/*  AES-128                                                             */
/* ------------------------------------------------------------------ */

static uint8_t aes_sbox[256], aes_inv_sbox[256];
static bool aes_tables_ready;

static uint8_t rol8(uint8_t v, unsigned int n)
{
    return (uint8_t)((v << n) | (v >> (8 - n)));
}

/* Build the S-box from its definition (inverse in GF(2^8), then the affine
 * map) instead of trusting 256 typed-in constants. */
static void aes_init_tables(void)
{
    uint8_t p = 1, q = 1;
    int i;

    if (aes_tables_ready)
        return;
    do {
        /* p walks the field by multiplying by 3, q by dividing by 3, so q = 1/p */
        p = (uint8_t)(p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0));
        q ^= (uint8_t)(q << 1);
        q ^= (uint8_t)(q << 2);
        q ^= (uint8_t)(q << 4);
        if (q & 0x80)
            q ^= 0x09;
        aes_sbox[p] = (uint8_t)(q ^ rol8(q, 1) ^ rol8(q, 2) ^ rol8(q, 3) ^ rol8(q, 4) ^ 0x63);
    } while (p != 1);
    aes_sbox[0] = 0x63;
    for (i = 0; i < 256; i++)
        aes_inv_sbox[aes_sbox[i]] = (uint8_t)i;
    aes_tables_ready = true;
}

static uint8_t xtime(uint8_t v)
{
    return (uint8_t)((v << 1) ^ (v & 0x80 ? 0x1b : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;

    while (b) {
        if (b & 1)
            r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

/* 11 round keys of 16 bytes */
static void aes128_expand(const uint8_t key[16], uint8_t rk[176])
{
    uint8_t rcon = 1;
    int i;

    aes_init_tables();
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        uint8_t t[4] = { rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1] };

        if (i % 16 == 0) {
            uint8_t first = t[0];

            t[0] = aes_sbox[t[1]] ^ rcon;
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[first];
            rcon = xtime(rcon);
        }
        rk[i] = rk[i - 16] ^ t[0];
        rk[i + 1] = rk[i - 15] ^ t[1];
        rk[i + 2] = rk[i - 14] ^ t[2];
        rk[i + 3] = rk[i - 13] ^ t[3];
    }
}

static void add_round_key(uint8_t s[16], const uint8_t *rk)
{
    int i;

    for (i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

/* The state is column-major: s[4 * column + row]. */
static void shift_rows(uint8_t s[16], bool inverse)
{
    uint8_t t[16];
    int r, c;

    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            t[4 * c + r] = s[4 * ((c + (inverse ? 4 - r : r)) % 4) + r];
    memcpy(s, t, 16);
}

static void mix_columns(uint8_t s[16], bool inverse)
{
    static const uint8_t fwd[4] = { 2, 3, 1, 1 }, inv[4] = { 14, 11, 13, 9 };
    const uint8_t *m = inverse ? inv : fwd;
    int c, r;

    for (c = 0; c < 4; c++) {
        uint8_t col[4] = { s[4 * c], s[4 * c + 1], s[4 * c + 2], s[4 * c + 3] };

        for (r = 0; r < 4; r++)
            s[4 * c + r] = gmul(col[0], m[(4 - r) % 4]) ^ gmul(col[1], m[(5 - r) % 4]) ^
                           gmul(col[2], m[(6 - r) % 4]) ^ gmul(col[3], m[(7 - r) % 4]);
    }
}

void rtw89_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16];
    int round, i;

    aes128_expand(key, rk);
    memcpy(s, in, 16);
    add_round_key(s, rk);
    for (round = 1; round <= 10; round++) {
        for (i = 0; i < 16; i++)
            s[i] = aes_sbox[s[i]];
        shift_rows(s, false);
        if (round < 10)
            mix_columns(s, false);
        add_round_key(s, rk + 16 * round);
    }
    memcpy(out, s, 16);
    memset(rk, 0, sizeof(rk));
}

void rtw89_aes128_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16];
    int round, i;

    aes128_expand(key, rk);
    memcpy(s, in, 16);
    add_round_key(s, rk + 160);
    for (round = 9; round >= 0; round--) {
        shift_rows(s, true);
        for (i = 0; i < 16; i++)
            s[i] = aes_inv_sbox[s[i]];
        add_round_key(s, rk + 16 * round);
        if (round > 0)
            mix_columns(s, true);
    }
    memcpy(out, s, 16);
    memset(rk, 0, sizeof(rk));
}

/* ------------------------------------------------------------------ */
/*  AES key wrap (RFC 3394)                                             */
/* ------------------------------------------------------------------ */

void rtw89_aes_wrap(const uint8_t kek[16], const uint8_t *plain, size_t plain_len,
                    uint8_t *wrapped)
{
    size_t n = plain_len / 8, i, j;
    uint8_t *a = wrapped, *r = wrapped + 8, b[16];
    int k;

    memset(a, 0xa6, 8);
    memcpy(r, plain, plain_len);
    for (j = 0; j < 6; j++) {
        for (i = 1; i <= n; i++) {
            uint64_t t = n * j + i;

            memcpy(b, a, 8);
            memcpy(b + 8, r + 8 * (i - 1), 8);
            rtw89_aes128_encrypt(kek, b, b);
            memcpy(a, b, 8);
            for (k = 0; k < 8; k++)
                a[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(r + 8 * (i - 1), b + 8, 8);
        }
    }
}

bool rtw89_aes_unwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t wrapped_len,
                      uint8_t *plain)
{
    size_t n, i;
    uint8_t a[8], b[16], bad = 0;
    int j, k;

    if (wrapped_len < 24 || wrapped_len % 8)
        return false;
    n = wrapped_len / 8 - 1;

    memcpy(a, wrapped, 8);
    memcpy(plain, wrapped + 8, 8 * n);
    for (j = 5; j >= 0; j--) {
        for (i = n; i >= 1; i--) {
            uint64_t t = n * (size_t)j + i;

            memcpy(b, a, 8);
            for (k = 0; k < 8; k++)
                b[7 - k] ^= (uint8_t)(t >> (8 * k));
            memcpy(b + 8, plain + 8 * (i - 1), 8);
            rtw89_aes128_decrypt(kek, b, b);
            memcpy(a, b, 8);
            memcpy(plain + 8 * (i - 1), b + 8, 8);
        }
    }

    for (k = 0; k < 8; k++)
        bad |= a[k] ^ 0xa6;
    return bad == 0;
}

bool rtw89_crypto_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    size_t i;

    for (i = 0; i < len; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
