/* dump_vectors.c — emit computed crypto outputs for cross-checking against a
 * trusted independent implementation (Python pyca/cryptography). Deterministic
 * pseudo-random inputs across a range of lengths, plus edge cases (empty aad,
 * empty plaintext). Output: one line per case, hex fields, "-" == empty. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define WH_LINK_REF_CRYPTO_IMPLEMENTATION
#include "wh_link_ref_crypto.h"

static uint64_t s = 0x123456789abcdef0ULL;
static uint8_t rb(void){ s ^= s<<13; s ^= s>>7; s ^= s<<17; return (uint8_t)(s>>24); }
static void fill(uint8_t *b, size_t n){ for(size_t i=0;i<n;i++) b[i]=rb(); }
static void ph(const uint8_t *b, size_t n){ if(n==0){printf("-");return;} for(size_t i=0;i<n;i++) printf("%02x", b[i]); }

int main(void){
    uint8_t buf[600]={0}, out[600]={0}, key[32]={0}, nonce[12]={0}, aad[64]={0}, tag[16]={0};

    /* SHA256 over varying lengths */
    for (size_t L = 0; L <= 200; L += 37) {
        fill(buf, L); whc_sha256(buf, L, out);
        printf("SHA256 "); ph(buf,L); printf(" "); ph(out,32); printf("\n");
    }
    /* HMAC over varying key/msg lengths */
    for (size_t K = 1; K <= 100; K += 33) {
        for (size_t M = 0; M <= 80; M += 40) {
            fill(key, K>32?32:K); uint8_t hk[100]; fill(hk,K); fill(buf,M);
            whc_hmac_sha256(hk, K, buf, M, out);
            printf("HMAC "); ph(hk,K); printf(" "); ph(buf,M); printf(" "); ph(out,32); printf("\n");
        }
    }
    /* HKDF varying salt/ikm/info/outlen */
    { size_t combos[][4] = {{13,22,10,42},{0,32,0,64},{16,16,16,80},{8,40,5,100}};
      for (int i=0;i<4;i++){
        uint8_t salt[64], ikm[64], info[64];
        size_t sl=combos[i][0], il=combos[i][1], fl=combos[i][2], ol=combos[i][3];
        fill(salt,sl); fill(ikm,il); fill(info,fl);
        whc_hkdf(sl?salt:NULL, sl, ikm, il, fl?info:NULL, fl, out, ol);
        printf("HKDF "); ph(salt,sl); printf(" "); ph(ikm,il); printf(" "); ph(info,fl);
        printf(" %zu ", ol); ph(out,ol); printf("\n");
      }
    }
    /* AEAD: varying pt lengths, some empty aad / empty pt */
    { size_t ptlens[] = {0,1,15,16,17,63,64,100,200};
      size_t aadlens[] = {0,12,20};
      for (unsigned a=0;a<3;a++) for (unsigned p=0;p<9;p++){
        size_t al=aadlens[a], pl=ptlens[p];
        fill(key,32); fill(nonce,12); fill(aad,al); fill(buf,pl);
        whc_chacha20poly1305_seal(key,nonce,aad,al,buf,pl,out,tag);
        printf("AEAD "); ph(key,32); printf(" "); ph(nonce,12); printf(" ");
        ph(aad,al); printf(" "); ph(buf,pl); printf(" "); ph(out,pl); printf(" "); ph(tag,16); printf("\n");
      }
    }
    /* X25519 base + DH */
    for (int i=0;i<4;i++){
        uint8_t sa[32], sb[32], pub[32], sh[32], pb[32];
        fill(sa,32); fill(sb,32);
        whc_x25519_base(pub, sa);
        printf("X25519B "); ph(sa,32); printf(" "); ph(pub,32); printf("\n");
        whc_x25519_base(pb, sb);
        whc_x25519(sh, sa, pb);
        printf("X25519 "); ph(sa,32); printf(" "); ph(sb,32); printf(" "); ph(sh,32); printf("\n");
    }
    return 0;
}
