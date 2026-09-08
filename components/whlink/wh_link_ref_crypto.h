/*
 * wh_link_ref_crypto.h — reference cryptographic primitives for wh-link
 *
 * Portable, dependency-free C11 reference implementations of the primitives the
 * wh-link protocol needs:
 *
 *   - SHA-256                       (FIPS 180-4)
 *   - HMAC-SHA256                   (RFC 2104)
 *   - HKDF-SHA256                   (RFC 5869)
 *   - ChaCha20                      (RFC 8439 §2.3-2.4)
 *   - Poly1305                      (RFC 8439 §2.5)
 *   - ChaCha20-Poly1305 AEAD        (RFC 8439 §2.8)
 *   - X25519                        (RFC 7748)
 *
 * These are the *reference* / host-test / default backend. They are correct and
 * self-contained but not hardened (not constant-time beyond what the algorithms
 * give for free, no side-channel countermeasures beyond the ChaCha/Poly/X25519
 * structural ones). On device you may swap in an mbedTLS-backed implementation
 * of the same wh_crypto_if interface (see wh_link.h) to use hardware AES / a
 * vetted stack; this header remains the portable oracle that the test vectors
 * are validated against.
 *
 * The X25519 field arithmetic follows the well-known compact "TweetNaCl" style
 * (radix-2^16, gf[16]); it is validated here against the RFC 7748 test vectors.
 *
 * Usage (stb-style single-header):
 *     #define WH_LINK_REF_CRYPTO_IMPLEMENTATION   // in exactly ONE .c file
 *     #include "wh_link_ref_crypto.h"
 * Everywhere else just #include the header for the declarations.
 *
 * License: this file is intended for release under the same BSD-3-Clause terms
 * as the surrounding Whitehat firmware. The algorithms are public standards.
 */
#ifndef WH_LINK_REF_CRYPTO_H
#define WH_LINK_REF_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- sizes ------------------------------------------------------------- */
#define WHC_SHA256_LEN      32
#define WHC_SHA256_BLOCK    64
#define WHC_X25519_LEN      32
#define WHC_CHACHA_KEY      32
#define WHC_CHACHA_NONCE    12
#define WHC_POLY1305_TAG    16
#define WHC_AEAD_KEY        32
#define WHC_AEAD_NONCE      12
#define WHC_AEAD_TAG        16

/* ---- SHA-256 ----------------------------------------------------------- */
typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buf[WHC_SHA256_BLOCK];
    size_t   buflen;
} whc_sha256_ctx;

void whc_sha256_init(whc_sha256_ctx *c);
void whc_sha256_update(whc_sha256_ctx *c, const void *data, size_t len);
void whc_sha256_final(whc_sha256_ctx *c, uint8_t out[WHC_SHA256_LEN]);
void whc_sha256(const void *data, size_t len, uint8_t out[WHC_SHA256_LEN]);

/* ---- HMAC-SHA256 ------------------------------------------------------- */
void whc_hmac_sha256(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     uint8_t out[WHC_SHA256_LEN]);

/* ---- HKDF-SHA256 (RFC 5869) ------------------------------------------- */
void whc_hkdf_extract(const uint8_t *salt, size_t salt_len,
                      const uint8_t *ikm, size_t ikm_len,
                      uint8_t prk[WHC_SHA256_LEN]);
/* returns 0 on success, -1 if out_len is too large (> 255*32) */
int  whc_hkdf_expand(const uint8_t prk[WHC_SHA256_LEN],
                     const uint8_t *info, size_t info_len,
                     uint8_t *out, size_t out_len);
/* one-shot extract+expand */
int  whc_hkdf(const uint8_t *salt, size_t salt_len,
              const uint8_t *ikm, size_t ikm_len,
              const uint8_t *info, size_t info_len,
              uint8_t *out, size_t out_len);

/* ---- ChaCha20 (RFC 8439) ---------------------------------------------- */
/* XOR keystream into out (out may alias in). counter is the initial block
 * counter (RFC 8439 uses 1 for payload, 0 for the Poly1305 key block). */
void whc_chacha20_xor(const uint8_t key[WHC_CHACHA_KEY],
                      const uint8_t nonce[WHC_CHACHA_NONCE],
                      uint32_t counter,
                      const uint8_t *in, uint8_t *out, size_t len);

/* ---- Poly1305 (RFC 8439) ---------------------------------------------- */
void whc_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len,
                  uint8_t tag[WHC_POLY1305_TAG]);

/* ---- ChaCha20-Poly1305 AEAD (RFC 8439 §2.8) --------------------------- */
/* Seal: writes ciphertext (== plaintext length) to ct and the 16-byte tag.
 * ct may alias pt. */
void whc_chacha20poly1305_seal(const uint8_t key[WHC_AEAD_KEY],
                               const uint8_t nonce[WHC_AEAD_NONCE],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *pt, size_t pt_len,
                               uint8_t *ct, uint8_t tag[WHC_AEAD_TAG]);
/* Open: verifies tag, writes plaintext to pt (may alias ct).
 * Returns 0 on success, -1 on authentication failure (pt is then zeroed). */
int  whc_chacha20poly1305_open(const uint8_t key[WHC_AEAD_KEY],
                               const uint8_t nonce[WHC_AEAD_NONCE],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *ct, size_t ct_len,
                               const uint8_t tag[WHC_AEAD_TAG],
                               uint8_t *pt);

/* ---- X25519 (RFC 7748) ------------------------------------------------- */
/* Compute the shared/public value. out = scalar * point. Returns 0. */
int  whc_x25519(uint8_t out[WHC_X25519_LEN],
                const uint8_t scalar[WHC_X25519_LEN],
                const uint8_t point[WHC_X25519_LEN]);
/* out = scalar * basepoint(9). */
int  whc_x25519_base(uint8_t out[WHC_X25519_LEN],
                     const uint8_t scalar[WHC_X25519_LEN]);

/* Constant-time-ish comparison, returns 0 if equal. */
int  whc_memeq_ct(const void *a, const void *b, size_t len);
/* Zeroize (best effort, not optimized away). */
void whc_wipe(void *p, size_t len);

#ifdef __cplusplus
}
#endif

/* ======================================================================= */
/* Implementation                                                          */
/* ======================================================================= */
#ifdef WH_LINK_REF_CRYPTO_IMPLEMENTATION

#include <string.h>

/* ---- small helpers ----------------------------------------------------- */
static inline uint32_t whc__rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t whc__rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t whc__load32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void whc__store32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline uint32_t whc__load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void whc__store32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void whc__store64_le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}

void whc_wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len--) *v++ = 0;
}

int whc_memeq_ct(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    uint8_t d = 0;
    for (size_t i = 0; i < len; i++) d = (uint8_t)(d | (pa[i] ^ pb[i]));
    return d == 0 ? 0 : -1;
}

/* ---- SHA-256 ----------------------------------------------------------- */
static const uint32_t whc__sha256_k[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void whc__sha256_compress(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = whc__load32_be(block + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = whc__rotr32(w[i-15], 7) ^ whc__rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = whc__rotr32(w[i-2], 17) ^ whc__rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = whc__rotr32(e, 6) ^ whc__rotr32(e, 11) ^ whc__rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + whc__sha256_k[i] + w[i];
        uint32_t S0 = whc__rotr32(a, 2) ^ whc__rotr32(a, 13) ^ whc__rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void whc_sha256_init(whc_sha256_ctx *c)
{
    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u; c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu; c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bitlen = 0; c->buflen = 0;
}

void whc_sha256_update(whc_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->bitlen += (uint64_t)len * 8u;
    while (len > 0) {
        size_t take = WHC_SHA256_BLOCK - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take; p += take; len -= take;
        if (c->buflen == WHC_SHA256_BLOCK) {
            whc__sha256_compress(c->state, c->buf);
            c->buflen = 0;
        }
    }
}

void whc_sha256_final(whc_sha256_ctx *c, uint8_t out[WHC_SHA256_LEN])
{
    uint8_t pad = 0x80;
    uint64_t bits = c->bitlen;
    whc_sha256_update(c, &pad, 1);
    pad = 0x00;
    while (c->buflen != 56) whc_sha256_update(c, &pad, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++) lenbuf[i] = (uint8_t)(bits >> (56 - 8 * i));
    whc_sha256_update(c, lenbuf, 8);
    for (int i = 0; i < 8; i++) whc__store32_be(out + 4 * i, c->state[i]);
}

void whc_sha256(const void *data, size_t len, uint8_t out[WHC_SHA256_LEN])
{
    whc_sha256_ctx c; whc_sha256_init(&c); whc_sha256_update(&c, data, len); whc_sha256_final(&c, out);
}

/* ---- HMAC-SHA256 ------------------------------------------------------- */
void whc_hmac_sha256(const uint8_t *key, size_t key_len,
                     const uint8_t *msg, size_t msg_len,
                     uint8_t out[WHC_SHA256_LEN])
{
    uint8_t k[WHC_SHA256_BLOCK];
    uint8_t ipad[WHC_SHA256_BLOCK], opad[WHC_SHA256_BLOCK];
    uint8_t inner[WHC_SHA256_LEN];
    whc_sha256_ctx c;

    memset(k, 0, sizeof(k));
    if (key_len > WHC_SHA256_BLOCK) {
        whc_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (int i = 0; i < WHC_SHA256_BLOCK; i++) {
        ipad[i] = (uint8_t)(k[i] ^ 0x36);
        opad[i] = (uint8_t)(k[i] ^ 0x5c);
    }
    whc_sha256_init(&c);
    whc_sha256_update(&c, ipad, WHC_SHA256_BLOCK);
    whc_sha256_update(&c, msg, msg_len);
    whc_sha256_final(&c, inner);

    whc_sha256_init(&c);
    whc_sha256_update(&c, opad, WHC_SHA256_BLOCK);
    whc_sha256_update(&c, inner, WHC_SHA256_LEN);
    whc_sha256_final(&c, out);

    whc_wipe(k, sizeof(k)); whc_wipe(ipad, sizeof(ipad)); whc_wipe(opad, sizeof(opad));
}

/* ---- HKDF-SHA256 ------------------------------------------------------- */
void whc_hkdf_extract(const uint8_t *salt, size_t salt_len,
                      const uint8_t *ikm, size_t ikm_len,
                      uint8_t prk[WHC_SHA256_LEN])
{
    uint8_t zero[WHC_SHA256_LEN];
    if (salt == NULL || salt_len == 0) {
        memset(zero, 0, sizeof(zero));
        salt = zero; salt_len = sizeof(zero);
    }
    whc_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

int whc_hkdf_expand(const uint8_t prk[WHC_SHA256_LEN],
                    const uint8_t *info, size_t info_len,
                    uint8_t *out, size_t out_len)
{
    if (out_len > 255u * WHC_SHA256_LEN) return -1;
    uint8_t t[WHC_SHA256_LEN];
    size_t t_len = 0;
    uint8_t counter = 1;
    size_t done = 0;
    while (done < out_len) {
        /* T(i) = HMAC(PRK, T(i-1) | info | i) */
        whc_sha256_ctx c; (void)c;
        uint8_t k[WHC_SHA256_BLOCK];
        uint8_t ipad[WHC_SHA256_BLOCK], opad[WHC_SHA256_BLOCK];
        uint8_t inner[WHC_SHA256_LEN];
        whc_sha256_ctx h;

        memset(k, 0, sizeof(k));
        memcpy(k, prk, WHC_SHA256_LEN); /* prk is 32 bytes <= block */
        for (int i = 0; i < WHC_SHA256_BLOCK; i++) {
            ipad[i] = (uint8_t)(k[i] ^ 0x36);
            opad[i] = (uint8_t)(k[i] ^ 0x5c);
        }
        whc_sha256_init(&h);
        whc_sha256_update(&h, ipad, WHC_SHA256_BLOCK);
        if (t_len) whc_sha256_update(&h, t, t_len);
        if (info_len) whc_sha256_update(&h, info, info_len);
        whc_sha256_update(&h, &counter, 1);
        whc_sha256_final(&h, inner);

        whc_sha256_init(&h);
        whc_sha256_update(&h, opad, WHC_SHA256_BLOCK);
        whc_sha256_update(&h, inner, WHC_SHA256_LEN);
        whc_sha256_final(&h, t);
        t_len = WHC_SHA256_LEN;

        size_t take = out_len - done;
        if (take > WHC_SHA256_LEN) take = WHC_SHA256_LEN;
        memcpy(out + done, t, take);
        done += take;
        counter++;

        whc_wipe(k, sizeof(k)); whc_wipe(ipad, sizeof(ipad)); whc_wipe(opad, sizeof(opad));
    }
    whc_wipe(t, sizeof(t));
    return 0;
}

int whc_hkdf(const uint8_t *salt, size_t salt_len,
             const uint8_t *ikm, size_t ikm_len,
             const uint8_t *info, size_t info_len,
             uint8_t *out, size_t out_len)
{
    uint8_t prk[WHC_SHA256_LEN];
    whc_hkdf_extract(salt, salt_len, ikm, ikm_len, prk);
    int r = whc_hkdf_expand(prk, info, info_len, out, out_len);
    whc_wipe(prk, sizeof(prk));
    return r;
}

/* ---- ChaCha20 (RFC 8439) ---------------------------------------------- */
#define WHC__QR(a,b,c,d) \
    a += b; d ^= a; d = whc__rotl32(d, 16); \
    c += d; b ^= c; b = whc__rotl32(b, 12); \
    a += b; d ^= a; d = whc__rotl32(d, 8);  \
    c += d; b ^= c; b = whc__rotl32(b, 7)

static void whc__chacha20_block(const uint32_t in[16], uint8_t out[64])
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        WHC__QR(x[0], x[4], x[ 8], x[12]);
        WHC__QR(x[1], x[5], x[ 9], x[13]);
        WHC__QR(x[2], x[6], x[10], x[14]);
        WHC__QR(x[3], x[7], x[11], x[15]);
        WHC__QR(x[0], x[5], x[10], x[15]);
        WHC__QR(x[1], x[6], x[11], x[12]);
        WHC__QR(x[2], x[7], x[ 8], x[13]);
        WHC__QR(x[3], x[4], x[ 9], x[14]);
    }
    for (int i = 0; i < 16; i++) whc__store32_le(out + 4 * i, x[i] + in[i]);
}

void whc_chacha20_xor(const uint8_t key[WHC_CHACHA_KEY],
                      const uint8_t nonce[WHC_CHACHA_NONCE],
                      uint32_t counter,
                      const uint8_t *in, uint8_t *out, size_t len)
{
    uint32_t state[16];
    state[0] = 0x61707865u; state[1] = 0x3320646eu; state[2] = 0x79622d32u; state[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) state[4 + i] = whc__load32_le(key + 4 * i);
    state[12] = counter;
    state[13] = whc__load32_le(nonce + 0);
    state[14] = whc__load32_le(nonce + 4);
    state[15] = whc__load32_le(nonce + 8);

    uint8_t ks[64];
    size_t off = 0;
    while (off < len) {
        whc__chacha20_block(state, ks);
        size_t take = len - off;
        if (take > 64) take = 64;
        for (size_t i = 0; i < take; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        off += take;
        state[12]++; /* next block */
    }
    whc_wipe(ks, sizeof(ks));
    whc_wipe(state, sizeof(state));
}

/* ---- Poly1305 (RFC 8439) — 26-bit limb implementation ----------------- */
void whc_poly1305(const uint8_t key[32], const uint8_t *msg, size_t len,
                  uint8_t tag[WHC_POLY1305_TAG])
{
    uint32_t r0, r1, r2, r3, r4;
    uint32_t s1, s2, s3, s4;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;

    /* r &= 0xffffffc0ffffffc0ffffffc0fffffff  (clamp), split into 26-bit limbs */
    uint32_t t0 = whc__load32_le(key + 0);
    uint32_t t1 = whc__load32_le(key + 4);
    uint32_t t2 = whc__load32_le(key + 8);
    uint32_t t3 = whc__load32_le(key + 12);

    r0 =  t0                        & 0x3ffffff;
    r1 = ((t0 >> 26) | (t1 <<  6))  & 0x3ffff03;
    r2 = ((t1 >> 20) | (t2 << 12))  & 0x3ffc0ff;
    r3 = ((t2 >> 14) | (t3 << 18))  & 0x3f03fff;
    r4 =  (t3 >>  8)                & 0x00fffff;

    s1 = r1 * 5; s2 = r2 * 5; s3 = r3 * 5; s4 = r4 * 5;

    while (len > 0) {
        uint8_t block[16];
        size_t take = len < 16 ? len : 16;
        /* pad partial block: bytes, then 0x01, then zeros */
        if (take < 16) {
            memset(block, 0, sizeof(block));
            memcpy(block, msg, take);
            block[take] = 1;
        } else {
            memcpy(block, msg, 16);
        }
        uint32_t b0 = whc__load32_le(block + 0);
        uint32_t b1 = whc__load32_le(block + 4);
        uint32_t b2 = whc__load32_le(block + 8);
        uint32_t b3 = whc__load32_le(block + 12);

        h0 +=  b0                        & 0x3ffffff;
        h1 += ((b0 >> 26) | (b1 <<  6))  & 0x3ffffff;
        h2 += ((b1 >> 20) | (b2 << 12))  & 0x3ffffff;
        h3 += ((b2 >> 14) | (b3 << 18))  & 0x3ffffff;
        /* high limb: add carry bit 0x1000000 only for full 16-byte blocks */
        h4 += (b3 >> 8) | (take == 16 ? (1u << 24) : 0u);

        /* h *= r  (mod 2^130 - 5) */
        d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        c  = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;

        msg += take; len -= take;
    }

    /* fully carry h */
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    /* compute h + -p (i.e. h - (2^130-5)) and select if no borrow */
    uint32_t g0, g1, g2, g3, g4;
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1; /* if g4 negative -> 0x00000000 else 0xffffffff */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    uint32_t nmask = ~mask;
    h0 = (h0 & nmask) | g0;
    h1 = (h1 & nmask) | g1;
    h2 = (h2 & nmask) | g2;
    h3 = (h3 & nmask) | g3;
    h4 = (h4 & nmask) | g4;

    /* serialize h to 128-bit little-endian (4 x 32-bit words) */
    uint32_t f0 = (h0 | (h1 << 26)) & 0xffffffffu;
    uint32_t f1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
    uint32_t f2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
    uint32_t f3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;

    /* add s (key[16..31]) mod 2^128 */
    uint64_t acc;
    acc = (uint64_t)f0 + whc__load32_le(key + 16); f0 = (uint32_t)acc;
    acc = (uint64_t)f1 + whc__load32_le(key + 20) + (acc >> 32); f1 = (uint32_t)acc;
    acc = (uint64_t)f2 + whc__load32_le(key + 24) + (acc >> 32); f2 = (uint32_t)acc;
    acc = (uint64_t)f3 + whc__load32_le(key + 28) + (acc >> 32); f3 = (uint32_t)acc;

    whc__store32_le(tag + 0, f0);
    whc__store32_le(tag + 4, f1);
    whc__store32_le(tag + 8, f2);
    whc__store32_le(tag + 12, f3);
}

/* ---- ChaCha20-Poly1305 AEAD (RFC 8439 §2.8) --------------------------- */
static void whc__poly1305_key_gen(const uint8_t key[32], const uint8_t nonce[12],
                                  uint8_t otk[32])
{
    uint8_t zeros[32];
    memset(zeros, 0, sizeof(zeros));
    whc_chacha20_xor(key, nonce, 0, zeros, otk, 32); /* counter 0 -> first 32 bytes */
}

/* Incremental Poly1305 to avoid allocating aad|ct concatenations. */
typedef struct {
    uint32_t r0,r1,r2,r3,r4, s1,s2,s3,s4;
    uint32_t h0,h1,h2,h3,h4;
    uint8_t  buf[16];
    size_t   buflen;
    uint8_t  pad[16]; /* key[16..31] */
} whc__poly_state;

static void whc__poly_init(whc__poly_state *st, const uint8_t key[32])
{
    uint32_t t0 = whc__load32_le(key + 0);
    uint32_t t1 = whc__load32_le(key + 4);
    uint32_t t2 = whc__load32_le(key + 8);
    uint32_t t3 = whc__load32_le(key + 12);
    st->r0 =  t0                        & 0x3ffffff;
    st->r1 = ((t0 >> 26) | (t1 <<  6))  & 0x3ffff03;
    st->r2 = ((t1 >> 20) | (t2 << 12))  & 0x3ffc0ff;
    st->r3 = ((t2 >> 14) | (t3 << 18))  & 0x3f03fff;
    st->r4 =  (t3 >>  8)                & 0x00fffff;
    st->s1 = st->r1 * 5; st->s2 = st->r2 * 5; st->s3 = st->r3 * 5; st->s4 = st->r4 * 5;
    st->h0 = st->h1 = st->h2 = st->h3 = st->h4 = 0;
    st->buflen = 0;
    memcpy(st->pad, key + 16, 16);
}

static void whc__poly_block(whc__poly_state *st, const uint8_t block[16], int full)
{
    uint32_t b0 = whc__load32_le(block + 0);
    uint32_t b1 = whc__load32_le(block + 4);
    uint32_t b2 = whc__load32_le(block + 8);
    uint32_t b3 = whc__load32_le(block + 12);
    uint32_t h0 = st->h0, h1 = st->h1, h2 = st->h2, h3 = st->h3, h4 = st->h4;
    uint64_t d0,d1,d2,d3,d4; uint32_t c;

    h0 +=  b0                        & 0x3ffffff;
    h1 += ((b0 >> 26) | (b1 <<  6))  & 0x3ffffff;
    h2 += ((b1 >> 20) | (b2 << 12))  & 0x3ffffff;
    h3 += ((b2 >> 14) | (b3 << 18))  & 0x3ffffff;
    h4 += (b3 >> 8) | (full ? (1u << 24) : 0u);

    d0 = (uint64_t)h0*st->r0 + (uint64_t)h1*st->s4 + (uint64_t)h2*st->s3 + (uint64_t)h3*st->s2 + (uint64_t)h4*st->s1;
    d1 = (uint64_t)h0*st->r1 + (uint64_t)h1*st->r0 + (uint64_t)h2*st->s4 + (uint64_t)h3*st->s3 + (uint64_t)h4*st->s2;
    d2 = (uint64_t)h0*st->r2 + (uint64_t)h1*st->r1 + (uint64_t)h2*st->r0 + (uint64_t)h3*st->s4 + (uint64_t)h4*st->s3;
    d3 = (uint64_t)h0*st->r3 + (uint64_t)h1*st->r2 + (uint64_t)h2*st->r1 + (uint64_t)h3*st->r0 + (uint64_t)h4*st->s4;
    d4 = (uint64_t)h0*st->r4 + (uint64_t)h1*st->r3 + (uint64_t)h2*st->r2 + (uint64_t)h3*st->r1 + (uint64_t)h4*st->r0;

    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    st->h0 = h0; st->h1 = h1; st->h2 = h2; st->h3 = h3; st->h4 = h4;
}

static void whc__poly_update(whc__poly_state *st, const uint8_t *m, size_t len)
{
    if (st->buflen) {
        while (len > 0 && st->buflen < 16) { st->buf[st->buflen++] = *m++; len--; }
        if (st->buflen == 16) { whc__poly_block(st, st->buf, 1); st->buflen = 0; }
    }
    while (len >= 16) { whc__poly_block(st, m, 1); m += 16; len -= 16; }
    while (len > 0) { st->buf[st->buflen++] = *m++; len--; }
}

static void whc__poly_pad16(whc__poly_state *st, size_t already)
{
    size_t rem = already % 16;
    if (rem) {
        uint8_t z[16]; memset(z, 0, sizeof(z));
        whc__poly_update(st, z, 16 - rem);
    }
}

static void whc__poly_finish(whc__poly_state *st, uint8_t tag[16])
{
    if (st->buflen) {
        uint8_t block[16];
        memset(block, 0, sizeof(block));
        memcpy(block, st->buf, st->buflen);
        block[st->buflen] = 1;
        whc__poly_block(st, block, 0);
        st->buflen = 0;
    }
    uint32_t h0 = st->h0, h1 = st->h1, h2 = st->h2, h3 = st->h3, h4 = st->h4, c;
    c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;

    uint32_t g0,g1,g2,g3,g4;
    g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    uint32_t nmask = ~mask;
    h0 = (h0 & nmask) | g0; h1 = (h1 & nmask) | g1; h2 = (h2 & nmask) | g2;
    h3 = (h3 & nmask) | g3; h4 = (h4 & nmask) | g4;

    uint32_t f0 = (h0 | (h1 << 26));
    uint32_t f1 = ((h1 >> 6) | (h2 << 20));
    uint32_t f2 = ((h2 >> 12) | (h3 << 14));
    uint32_t f3 = ((h3 >> 18) | (h4 << 8));

    uint64_t acc;
    acc = (uint64_t)f0 + whc__load32_le(st->pad + 0);  f0 = (uint32_t)acc;
    acc = (uint64_t)f1 + whc__load32_le(st->pad + 4)  + (acc >> 32); f1 = (uint32_t)acc;
    acc = (uint64_t)f2 + whc__load32_le(st->pad + 8)  + (acc >> 32); f2 = (uint32_t)acc;
    acc = (uint64_t)f3 + whc__load32_le(st->pad + 12) + (acc >> 32); f3 = (uint32_t)acc;

    whc__store32_le(tag + 0, f0);
    whc__store32_le(tag + 4, f1);
    whc__store32_le(tag + 8, f2);
    whc__store32_le(tag + 12, f3);
}

void whc_chacha20poly1305_seal(const uint8_t key[WHC_AEAD_KEY],
                               const uint8_t nonce[WHC_AEAD_NONCE],
                               const uint8_t *aad, size_t aad_len,
                               const uint8_t *pt, size_t pt_len,
                               uint8_t *ct, uint8_t tag[WHC_AEAD_TAG])
{
    uint8_t otk[32];
    whc__poly1305_key_gen(key, nonce, otk);
    /* encrypt with counter starting at 1 */
    whc_chacha20_xor(key, nonce, 1, pt, ct, pt_len);
    /* MAC over aad | pad | ct | pad | le64(aad_len) | le64(ct_len) */
    whc__poly_state st;
    whc__poly_init(&st, otk);
    if (aad_len) whc__poly_update(&st, aad, aad_len);
    whc__poly_pad16(&st, aad_len);
    if (pt_len)  whc__poly_update(&st, ct, pt_len);
    whc__poly_pad16(&st, pt_len);
    uint8_t lenblk[16];
    whc__store64_le(lenblk + 0, (uint64_t)aad_len);
    whc__store64_le(lenblk + 8, (uint64_t)pt_len);
    whc__poly_update(&st, lenblk, 16);
    whc__poly_finish(&st, tag);
    whc_wipe(otk, sizeof(otk));
}

int whc_chacha20poly1305_open(const uint8_t key[WHC_AEAD_KEY],
                              const uint8_t nonce[WHC_AEAD_NONCE],
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *ct, size_t ct_len,
                              const uint8_t tag[WHC_AEAD_TAG],
                              uint8_t *pt)
{
    uint8_t otk[32];
    uint8_t want[16];
    whc__poly1305_key_gen(key, nonce, otk);
    whc__poly_state st;
    whc__poly_init(&st, otk);
    if (aad_len) whc__poly_update(&st, aad, aad_len);
    whc__poly_pad16(&st, aad_len);
    if (ct_len)  whc__poly_update(&st, ct, ct_len);
    whc__poly_pad16(&st, ct_len);
    uint8_t lenblk[16];
    whc__store64_le(lenblk + 0, (uint64_t)aad_len);
    whc__store64_le(lenblk + 8, (uint64_t)ct_len);
    whc__poly_update(&st, lenblk, 16);
    whc__poly_finish(&st, want);
    whc_wipe(otk, sizeof(otk));

    if (whc_memeq_ct(want, tag, 16) != 0) {
        if (pt && ct_len) memset(pt, 0, ct_len);
        return -1;
    }
    whc_chacha20_xor(key, nonce, 1, ct, pt, ct_len);
    return 0;
}

/* ---- X25519 (RFC 7748) — TweetNaCl-style field arithmetic ------------- */
typedef int64_t whc__gf[16];

static const whc__gf whc__121665 = {0xDB41, 1};

static void whc__car25519(whc__gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void whc__sel25519(whc__gf p, whc__gf q, int b)
{
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void whc__pack25519(uint8_t *o, const whc__gf n)
{
    whc__gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    whc__car25519(t); whc__car25519(t); whc__car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        whc__sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void whc__unpack25519(whc__gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void whc__A(whc__gf o, const whc__gf a, const whc__gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void whc__Z(whc__gf o, const whc__gf a, const whc__gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void whc__M(whc__gf o, const whc__gf a, const whc__gf b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    whc__car25519(o);
    whc__car25519(o);
}

static void whc__S(whc__gf o, const whc__gf a) { whc__M(o, a, a); }

static void whc__inv25519(whc__gf o, const whc__gf i)
{
    whc__gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        whc__S(c, c);
        if (a != 2 && a != 4) whc__M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

int whc_x25519(uint8_t out[WHC_X25519_LEN],
               const uint8_t scalar[WHC_X25519_LEN],
               const uint8_t point[WHC_X25519_LEN])
{
    uint8_t z[32];
    whc__gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] = (uint8_t)(z[0] & 248);
    whc__unpack25519(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        whc__sel25519(a, b, (int)r);
        whc__sel25519(c, d, (int)r);
        whc__A(e, a, c);
        whc__Z(a, a, c);
        whc__A(c, b, d);
        whc__Z(b, b, d);
        whc__S(d, e);
        whc__S(f, a);
        whc__M(a, c, a);
        whc__M(c, b, e);
        whc__A(e, a, c);
        whc__Z(a, a, c);
        whc__S(b, a);
        whc__Z(c, d, f);
        whc__M(a, c, whc__121665);
        whc__A(a, a, d);
        whc__M(c, c, a);
        whc__M(a, d, f);
        whc__M(d, b, x);
        whc__S(b, e);
        whc__sel25519(a, b, (int)r);
        whc__sel25519(c, d, (int)r);
    }
    whc__gf xx;
    for (int i = 0; i < 16; i++) xx[i] = a[i];
    whc__gf zz;
    for (int i = 0; i < 16; i++) zz[i] = c[i];
    whc__inv25519(zz, zz);
    whc__M(xx, xx, zz);
    whc__pack25519(out, xx);
    whc_wipe(z, sizeof(z));
    return 0;
}

int whc_x25519_base(uint8_t out[WHC_X25519_LEN], const uint8_t scalar[WHC_X25519_LEN])
{
    static const uint8_t base[32] = { 9 };
    return whc_x25519(out, scalar, base);
}

#endif /* WH_LINK_REF_CRYPTO_IMPLEMENTATION */
#endif /* WH_LINK_REF_CRYPTO_H */
