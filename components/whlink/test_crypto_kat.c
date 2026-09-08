/* test_crypto_kat.c — known-answer tests for wh_link_ref_crypto.h
 *
 * Validates every primitive against published RFC/NIST test vectors:
 *   SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 4231), HKDF-SHA256 (RFC 5869),
 *   ChaCha20 / Poly1305 / ChaCha20-Poly1305 (RFC 8439), X25519 (RFC 7748).
 *
 * Exit 0 iff all pass. Prints a line per test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define WH_LINK_REF_CRYPTO_IMPLEMENTATION
#include "wh_link_ref_crypto.h"

static int g_fail = 0;
static int g_pass = 0;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* parse hex string into buf, return byte count (or -1). ignores spaces. */
static long unhex(const char *s, uint8_t *buf, size_t cap)
{
    size_t n = 0; int hi = -1;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\n' || *s == ':') continue;
        int v = hexval(*s);
        if (v < 0) return -1;
        if (hi < 0) { hi = v; }
        else {
            if (n >= cap) return -1;
            buf[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) return -1;
    return (long)n;
}

static void report(const char *name, int ok)
{
    if (ok) { g_pass++; printf("  PASS  %s\n", name); }
    else    { g_fail++; printf("  FAIL  %s\n", name); }
}

static int eqhex(const char *name, const uint8_t *got, const char *want_hex, size_t len)
{
    uint8_t want[512];
    long wl = unhex(want_hex, want, sizeof(want));
    int ok = (wl == (long)len) && (memcmp(got, want, len) == 0);
    if (!ok) {
        printf("  FAIL  %s\n    want %s\n    got  ", name, want_hex);
        for (size_t i = 0; i < len; i++) printf("%02x", got[i]);
        printf("\n");
        g_fail++;
    } else {
        g_pass++;
        printf("  PASS  %s\n", name);
    }
    return ok;
}

/* ------------------------------------------------------------------ SHA-256 */
static void test_sha256(void)
{
    printf("[SHA-256]\n");
    uint8_t out[32];
    whc_sha256("abc", 3, out);
    eqhex("sha256(\"abc\")", out,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);

    whc_sha256("", 0, out);
    eqhex("sha256(\"\")", out,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32);

    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    whc_sha256(msg, strlen(msg), out);
    eqhex("sha256(56-byte)", out,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32);

    /* streaming in odd chunks must match one-shot */
    whc_sha256_ctx c; whc_sha256_init(&c);
    for (const char *p = msg; *p; ) {
        size_t take = ((size_t)(p - msg) % 7) + 1;
        size_t rem = strlen(p);
        if (take > rem) take = rem;
        whc_sha256_update(&c, p, take);
        p += take;
    }
    uint8_t out2[32]; whc_sha256_final(&c, out2);
    report("sha256 streaming == one-shot", memcmp(out, out2, 32) == 0);
}

/* -------------------------------------------------------------- HMAC-SHA256 */
static void test_hmac(void)
{
    printf("[HMAC-SHA256] (RFC 4231)\n");
    uint8_t key[131], out[32];

    /* Test Case 1 */
    memset(key, 0x0b, 20);
    whc_hmac_sha256(key, 20, (const uint8_t *)"Hi There", 8, out);
    eqhex("hmac tc1", out,
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);

    /* Test Case 2: key "Jefe", data "what do ya want for nothing?" */
    whc_hmac_sha256((const uint8_t *)"Jefe", 4,
                    (const uint8_t *)"what do ya want for nothing?", 28, out);
    eqhex("hmac tc2", out,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);

    /* Test Case 6: key = 131 bytes of 0xaa, data = long string */
    memset(key, 0xaa, 131);
    whc_hmac_sha256(key, 131,
        (const uint8_t *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, out);
    eqhex("hmac tc6", out,
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32);
}

/* -------------------------------------------------------------- HKDF-SHA256 */
static void test_hkdf(void)
{
    printf("[HKDF-SHA256] (RFC 5869)\n");
    uint8_t ikm[80], salt[80], info[80], okm[82], prk[32];
    long il = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, sizeof(ikm));
    long sl = unhex("000102030405060708090a0b0c", salt, sizeof(salt));
    long fl = unhex("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));

    whc_hkdf_extract(salt, (size_t)sl, ikm, (size_t)il, prk);
    eqhex("hkdf tc1 PRK", prk,
          "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32);

    whc_hkdf_expand(prk, info, (size_t)fl, okm, 42);
    eqhex("hkdf tc1 OKM", okm,
          "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
          "34007208d5b887185865", 42);

    /* Test Case 3: zero-length salt and info */
    whc_hkdf(NULL, 0, ikm, (size_t)il, NULL, 0, okm, 42);
    eqhex("hkdf tc3 OKM (empty salt/info)", okm,
          "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
          "9d201395faa4b61a96c8", 42);
}

/* ------------------------------------------------------------------ ChaCha20 */
static void test_chacha20(void)
{
    printf("[ChaCha20] (RFC 8439 2.4.2)\n");
    uint8_t key[32], nonce[12];
    unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
    unhex("000000000000004a00000000", nonce, 12);
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                     "only one tip for the future, sunscreen would be it.";
    size_t ptlen = strlen(pt);
    uint8_t ct[256];
    whc_chacha20_xor(key, nonce, 1, (const uint8_t *)pt, ct, ptlen);
    eqhex("chacha20 keystream/ct", ct,
          "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
          "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
          "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
          "5af90bbf74a35be6b40b8eedf2785e42874d", ptlen);
}

/* ------------------------------------------------------------------ Poly1305 */
static void test_poly1305(void)
{
    printf("[Poly1305] (RFC 8439 2.5.2)\n");
    uint8_t key[32], tag[16];
    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    const char *msg = "Cryptographic Forum Research Group";
    whc_poly1305(key, (const uint8_t *)msg, strlen(msg), tag);
    eqhex("poly1305 tag", tag, "a8061dc1305136c6c22b8baf0c0127a9", 16);
}

/* ------------------------------------------------------ ChaCha20-Poly1305 AEAD */
static void test_aead(void)
{
    printf("[ChaCha20-Poly1305 AEAD] (RFC 8439 2.8.2)\n");
    uint8_t key[32], nonce[12], aad[12], ct[128], tag[16];
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, 32);
    unhex("070000004041424344454647", nonce, 12);
    unhex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you "
                     "only one tip for the future, sunscreen would be it.";
    size_t ptlen = strlen(pt);
    whc_chacha20poly1305_seal(key, nonce, aad, 12, (const uint8_t *)pt, ptlen, ct, tag);
    int ok = eqhex("aead ciphertext", ct,
        "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
        "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
        "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
        "3ff4def08e4b7a9de576d26586cec64b6116", ptlen);
    ok &= eqhex("aead tag", tag, "1ae10b594f09e26a7e902ecbd0600691", 16);

    /* round-trip open */
    uint8_t dec[128];
    int r = whc_chacha20poly1305_open(key, nonce, aad, 12, ct, ptlen, tag, dec);
    report("aead open (valid)", r == 0 && memcmp(dec, pt, ptlen) == 0);

    /* tamper ciphertext -> must fail */
    ct[0] ^= 0x01;
    r = whc_chacha20poly1305_open(key, nonce, aad, 12, ct, ptlen, tag, dec);
    report("aead open (tampered ct rejected)", r != 0);
    ct[0] ^= 0x01;

    /* tamper tag -> must fail */
    uint8_t badtag[16]; memcpy(badtag, tag, 16); badtag[15] ^= 0x80;
    r = whc_chacha20poly1305_open(key, nonce, aad, 12, ct, ptlen, badtag, dec);
    report("aead open (tampered tag rejected)", r != 0);

    /* tamper aad -> must fail */
    uint8_t badaad[12]; memcpy(badaad, aad, 12); badaad[0] ^= 0xff;
    r = whc_chacha20poly1305_open(key, nonce, badaad, 12, ct, ptlen, tag, dec);
    report("aead open (tampered aad rejected)", r != 0);

    (void)ok;
}

/* -------------------------------------------------------------------- X25519 */
static void test_x25519(void)
{
    printf("[X25519] (RFC 7748)\n");
    uint8_t scalar[32], point[32], out[32];

    /* Section 5.2 vector 1 */
    unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    whc_x25519(out, scalar, point);
    eqhex("x25519 5.2 vec1", out,
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);

    /* Section 5.2 vector 2 */
    unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", scalar, 32);
    unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", point, 32);
    whc_x25519(out, scalar, point);
    eqhex("x25519 5.2 vec2", out,
          "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32);

    /* Section 6.1 Diffie-Hellman */
    uint8_t a_priv[32], b_priv[32], a_pub[32], b_pub[32], ss_a[32], ss_b[32];
    unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_priv, 32);
    unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b_priv, 32);
    whc_x25519_base(a_pub, a_priv);
    eqhex("x25519 6.1 Alice pub", a_pub,
          "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
    whc_x25519_base(b_pub, b_priv);
    eqhex("x25519 6.1 Bob pub", b_pub,
          "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
    whc_x25519(ss_a, a_priv, b_pub);
    whc_x25519(ss_b, b_priv, a_pub);
    eqhex("x25519 6.1 shared (a)", ss_a,
          "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    report("x25519 6.1 shared a==b", memcmp(ss_a, ss_b, 32) == 0);
}

int main(void)
{
    printf("=== wh-link reference crypto KATs ===\n");
    test_sha256();
    test_hmac();
    test_hkdf();
    test_chacha20();
    test_poly1305();
    test_aead();
    test_x25519();
    printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
