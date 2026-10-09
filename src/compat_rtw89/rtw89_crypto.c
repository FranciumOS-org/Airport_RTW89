// SPDX-License-Identifier: GPL-2.0
/* See rtw89_crypto.h. Written from FIPS 180-4, RFC 2104, RFC 2898, FIPS 197,
 * RFC 3394, RFC 4493 and IEEE 802.11-2020 12.7.1.6.2; checked against their
 * test vectors in tools/hosttest. */
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
/*  SHA-256 (FIPS 180-4), HMAC-SHA256, the 802.11 KDF                   */
/* ------------------------------------------------------------------ */

struct sha256_ctx {
    uint32_t h[8];
    uint64_t len;           /* bytes hashed so far */
    uint8_t block[64];
    size_t fill;
};

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror32(uint32_t v, unsigned int n)
{
    return (v >> n) | (v << (32 - n));
}

static void sha256_compress(struct sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 64; i++)
        w[i] = (ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7] +
               (ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) +
             sha256_k[i] + w[i];
        t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(struct sha256_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->fill = 0;
}

static void sha256_update(struct sha256_ctx *c, const uint8_t *data, size_t len)
{
    c->len += len;
    while (len) {
        size_t n = 64 - c->fill < len ? 64 - c->fill : len;

        memcpy(c->block + c->fill, data, n);
        c->fill += n;
        data += n;
        len -= n;
        if (c->fill == 64) {
            sha256_compress(c, c->block);
            c->fill = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[RTW89_SHA256_LEN])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    int i;

    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->fill != 56)
        sha256_update(c, &pad, 1);
    for (i = 7; i >= 0; i--) {
        uint8_t b = (uint8_t)(bits >> (8 * i));

        sha256_update(c, &b, 1);
    }
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
    memset(c, 0, sizeof(*c));
}

void rtw89_sha256(const uint8_t *data, size_t len, uint8_t out[RTW89_SHA256_LEN])
{
    struct sha256_ctx c;

    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

void rtw89_hmac_sha256_vector(const uint8_t *key, size_t key_len, size_t num,
                              const uint8_t *const pieces[], const size_t lens[],
                              uint8_t out[RTW89_SHA256_LEN])
{
    uint8_t k[64], pad[64], inner[RTW89_SHA256_LEN];
    struct sha256_ctx c;
    size_t i;

    memset(k, 0, sizeof(k));
    if (key_len > 64)
        rtw89_sha256(key, key_len, k);
    else
        memcpy(k, key, key_len);

    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x36;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    for (i = 0; i < num; i++)
        sha256_update(&c, pieces[i], lens[i]);
    sha256_final(&c, inner);

    for (i = 0; i < 64; i++)
        pad[i] = k[i] ^ 0x5c;
    sha256_init(&c);
    sha256_update(&c, pad, 64);
    sha256_update(&c, inner, sizeof(inner));
    sha256_final(&c, out);

    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
    memset(inner, 0, sizeof(inner));
}

void rtw89_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len,
                       uint8_t out[RTW89_SHA256_LEN])
{
    rtw89_hmac_sha256_vector(key, key_len, 1, &data, &len, out);
}

void rtw89_kdf_sha256(const uint8_t *key, size_t key_len, const char *label,
                      const uint8_t *context, size_t context_len, uint8_t *out, size_t out_len)
{
    const uint16_t bits = (uint16_t)(out_len * 8);
    uint8_t length[2] = { (uint8_t)bits, (uint8_t)(bits >> 8) };
    size_t label_len = 0;
    uint16_t i = 1;

    while (label[label_len])
        label_len++;

    while (out_len) {
        /* i and Length little-endian; the label without its NUL */
        uint8_t counter[2] = { (uint8_t)i, (uint8_t)(i >> 8) };
        const uint8_t *pieces[4] = { counter, (const uint8_t *)label, context, length };
        size_t lens[4] = { 2, label_len, context_len, 2 };
        uint8_t hash[RTW89_SHA256_LEN];
        size_t n = out_len < RTW89_SHA256_LEN ? out_len : RTW89_SHA256_LEN;

        rtw89_hmac_sha256_vector(key, key_len, 4, pieces, lens, hash);
        memcpy(out, hash, n);
        memset(hash, 0, sizeof(hash));
        out += n;
        out_len -= n;
        i++;
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

/* AES-CMAC (RFC 4493): the EAPOL-Key MIC of SAE (AKM 8) and BIP's MIC. */
static void cmac_shift(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = in[0] & 0x80;
    int i;

    for (i = 0; i < 15; i++)
        out[i] = (uint8_t)(in[i] << 1 | in[i + 1] >> 7);
    out[15] = (uint8_t)(in[15] << 1);
    if (carry)
        out[15] ^= 0x87;
}

void rtw89_aes_cmac_vector(const uint8_t key[16], size_t num, const uint8_t *const pieces[],
                           const size_t lens[], uint8_t mac[16])
{
    uint8_t zero[16] = { 0 }, l[16], k1[16], k2[16], x[16], block[16];
    size_t total = 0, done = 0, fill = 0, i, j;

    rtw89_aes128_encrypt(key, zero, l);
    cmac_shift(l, k1);
    cmac_shift(k1, k2);
    for (i = 0; i < num; i++)
        total += lens[i];

    memset(x, 0, sizeof(x));
    for (i = 0; i < num; i++) {
        for (j = 0; j < lens[i]; j++) {
            block[fill++] = pieces[i][j];
            done++;
            /* the last block is kept back for the subkey */
            if (fill == 16 && done < total) {
                for (fill = 0; fill < 16; fill++)
                    x[fill] ^= block[fill];
                rtw89_aes128_encrypt(key, x, x);
                fill = 0;
            }
        }
    }
    if (total && fill == 16) {
        for (i = 0; i < 16; i++)
            x[i] ^= block[i] ^ k1[i];
    } else {
        block[fill] = 0x80;
        for (i = fill + 1; i < 16; i++)
            block[i] = 0;
        for (i = 0; i < 16; i++)
            x[i] ^= block[i] ^ k2[i];
    }
    rtw89_aes128_encrypt(key, x, mac);
    memset(l, 0, sizeof(l));
    memset(k1, 0, sizeof(k1));
    memset(k2, 0, sizeof(k2));
}

void rtw89_aes_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16])
{
    rtw89_aes_cmac_vector(key, 1, &data, &len, mac);
}

/* CCM nonce (12.5.3.3.4): flags (priority, or the management bit), A2, PN. */
static void ccmp_nonce(const uint8_t *hdr, size_t hdr_len, uint64_t pn, uint8_t nonce[13])
{
    unsigned int i;

    nonce[0] = 0;
    if ((hdr[0] & 0x0c) == 0x00)                /* management frame */
        nonce[0] = 0x10;
    else if ((hdr[0] & 0x8c) == 0x88)           /* QoS data: the TID */
        nonce[0] = hdr[hdr_len - 2] & 0x0f;
    memcpy(nonce + 1, hdr + 10, 6);
    for (i = 0; i < 6; i++)
        nonce[7 + i] = (uint8_t)(pn >> (8 * (5 - i)));
}

/* Additional authenticated data (12.5.3.3.3), with its 2-byte length in
 * front; returns the whole length. */
static size_t ccmp_aad(const uint8_t *hdr, size_t hdr_len, uint8_t aad[32])
{
    bool data = (hdr[0] & 0x0c) == 0x08, qos = (hdr[0] & 0x8c) == 0x88;
    size_t n = 2;

    aad[n++] = data ? hdr[0] & 0x8f : hdr[0];   /* data: subtype bits 4-6 masked */
    /* retry, power management, more data masked; protected set; order
     * masked in QoS data */
    aad[n++] = (hdr[1] & (qos ? 0x47 : 0xc7)) | 0x40;
    memcpy(aad + n, hdr + 4, 18);               /* A1, A2, A3 */
    n += 18;
    aad[n++] = hdr[22] & 0x0f;                  /* fragment number alone */
    aad[n++] = 0;
    if (hdr_len >= 30 && (hdr[1] & 0x03) == 0x03) {
        memcpy(aad + n, hdr + 24, 6);           /* A4 */
        n += 6;
    }
    if (qos) {
        aad[n++] = hdr[hdr_len - 2] & 0x0f;     /* QoS control: the TID */
        aad[n++] = 0;
    }
    aad[0] = 0;
    aad[1] = (uint8_t)(n - 2);
    return n;
}

/* CBC-MAC over B0, the AAD and the plaintext: the unencrypted MIC. */
static void ccmp_cbc_mac(const uint8_t tk[16], const uint8_t nonce[13], const uint8_t *aad,
                         size_t aad_len, const uint8_t *plain, size_t len, uint8_t x[16])
{
    uint8_t b[16];
    size_t i, j;

    b[0] = 0x59;                /* AAD present, 8-byte MIC, 2-byte length */
    memcpy(b + 1, nonce, 13);
    b[14] = (uint8_t)(len >> 8);
    b[15] = (uint8_t)len;
    rtw89_aes128_encrypt(tk, b, x);
    for (i = 0; i < aad_len; i += 16) {
        for (j = 0; j < 16; j++)
            x[j] ^= i + j < aad_len ? aad[i + j] : 0;
        rtw89_aes128_encrypt(tk, x, x);
    }
    for (i = 0; i < len; i += 16) {
        for (j = 0; j < 16 && i + j < len; j++)
            x[j] ^= plain[i + j];
        rtw89_aes128_encrypt(tk, x, x);
    }
}

/* Counter mode over @data, and the first counter block's key stream for the MIC. */
static void ccmp_ctr(const uint8_t tk[16], const uint8_t nonce[13], uint8_t *data, size_t len,
                     uint8_t s0[16])
{
    uint8_t a[16], s[16];
    size_t i, j;

    a[0] = 0x01;
    memcpy(a + 1, nonce, 13);
    a[14] = 0;
    a[15] = 0;
    rtw89_aes128_encrypt(tk, a, s0);
    for (i = 0; i < len; i += 16) {
        uint16_t ctr = (uint16_t)(i / 16 + 1);

        a[14] = (uint8_t)(ctr >> 8);
        a[15] = (uint8_t)ctr;
        rtw89_aes128_encrypt(tk, a, s);
        for (j = 0; j < 16 && i + j < len; j++)
            data[i + j] ^= s[j];
    }
    memset(s, 0, sizeof(s));
}

void rtw89_ccmp_encrypt(const uint8_t tk[16], const uint8_t *hdr, size_t hdr_len, uint64_t pn,
                        uint8_t *payload, size_t len, uint8_t mic[RTW89_CCMP_MIC_LEN])
{
    uint8_t nonce[13], aad[32], x[16], s0[16];
    size_t aad_len, i;

    ccmp_nonce(hdr, hdr_len, pn, nonce);
    aad_len = ccmp_aad(hdr, hdr_len, aad);
    ccmp_cbc_mac(tk, nonce, aad, aad_len, payload, len, x);
    ccmp_ctr(tk, nonce, payload, len, s0);
    for (i = 0; i < RTW89_CCMP_MIC_LEN; i++)
        mic[i] = x[i] ^ s0[i];
}

bool rtw89_ccmp_decrypt(const uint8_t tk[16], const uint8_t *hdr, size_t hdr_len, uint64_t pn,
                        uint8_t *payload, size_t len, const uint8_t mic[RTW89_CCMP_MIC_LEN])
{
    uint8_t nonce[13], aad[32], x[16], s0[16];
    size_t aad_len, i;

    ccmp_nonce(hdr, hdr_len, pn, nonce);
    aad_len = ccmp_aad(hdr, hdr_len, aad);
    ccmp_ctr(tk, nonce, payload, len, s0);
    ccmp_cbc_mac(tk, nonce, aad, aad_len, payload, len, x);
    for (i = 0; i < RTW89_CCMP_MIC_LEN; i++)
        x[i] ^= s0[i];
    return rtw89_crypto_equal(x, mic, RTW89_CCMP_MIC_LEN);
}

bool rtw89_crypto_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    size_t i;

    for (i = 0; i < len; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}
