/* crypto_id.c - node identity (Ed25519 via vendored Monocypher) +
 * TOFU pin store. SSH-model: first-seen pubkey per name is pinned in
 * NVS; a later mismatch is screamed about and REJECTED, never
 * silently re-pinned. */
#include "crypto_id.h"
#include "monocypher.h"
#include "nvs.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

static uint8_t s_sk[64], s_pk[32];
static uint8_t s_xsk[32], s_xpk[32];
static bool s_up;

void whm_id_init(void)
{
    nvs_handle_t h;
    if (nvs_open("cid", NVS_READWRITE, &h) != ESP_OK) return;
    size_t n = sizeof(s_sk);
    if (nvs_get_blob(h, "sk", s_sk, &n) == ESP_OK && n == 64) {
        n = sizeof(s_pk);
        nvs_get_blob(h, "pk", s_pk, &n);
        n = 32; nvs_get_blob(h, "xsk", s_xsk, &n);
        n = 32; nvs_get_blob(h, "xpk", s_xpk, &n);
    } else {
        uint8_t seed[32];
        esp_fill_random(seed, sizeof(seed));
        crypto_eddsa_key_pair(s_sk, s_pk, seed);
        nvs_set_blob(h, "sk", s_sk, 64);
        nvs_set_blob(h, "pk", s_pk, 32);
        esp_fill_random(s_xsk, 32);
        crypto_x25519_public_key(s_xpk, s_xsk);
        nvs_set_blob(h, "xsk", s_xsk, 32);
        nvs_set_blob(h, "xpk", s_xpk, 32);
        nvs_commit(h);
        char fp[17]; whm_id_fp(s_pk, fp);
        printf("identity: NEW keypair minted, fp %s\n", fp);
    }
    nvs_close(h);
    s_up = true;
}
const uint8_t *whm_id_pk(void) { return s_pk; }
const uint8_t *whm_id_xpk(void) { return s_xpk; }
void whm_id_xshared(const uint8_t their_xpk[32], uint8_t out[32])
{
    crypto_x25519(out, s_xsk, their_xpk);
}
bool whm_pinx_get(const char *name, uint8_t xpk[32])
{
    char k[16]; snprintf(k, sizeof(k), "x.%s", name);
    nvs_handle_t h; size_t n = 32; bool okk = false;
    if (nvs_open("pins", NVS_READONLY, &h) != ESP_OK) return false;
    okk = nvs_get_blob(h, k, xpk, &n) == ESP_OK && n == 32;
    nvs_close(h);
    return okk;
}
void whm_pinx_put(const char *name, const uint8_t xpk[32])
{
    char k[16]; snprintf(k, sizeof(k), "x.%s", name);
    nvs_handle_t h;
    if (nvs_open("pins", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, k, xpk, 32); nvs_commit(h); nvs_close(h);
}
void whm_id_sign(const uint8_t *m, size_t n, uint8_t sig[64])
{
    if (!s_up) { memset(sig, 0, 64); return; }
    crypto_eddsa_sign(sig, s_sk, m, n);
}
bool whm_id_verify(const uint8_t pk[32], const uint8_t *m, size_t n,
                   const uint8_t sig[64])
{
    return crypto_eddsa_check(sig, pk, m, n) == 0;
}
void whm_id_fp(const uint8_t pk[32], char out[17])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[2 * i] = hx[pk[i] >> 4];
        out[2 * i + 1] = hx[pk[i] & 15];
    }
    out[16] = 0;
}
bool whm_pin_get(const char *name, uint8_t pk[32])
{
    nvs_handle_t h; size_t n = 32; bool ok = false;
    if (nvs_open("pins", NVS_READONLY, &h) != ESP_OK) return false;
    ok = nvs_get_blob(h, name, pk, &n) == ESP_OK && n == 32;
    nvs_close(h);
    return ok;
}
void whm_pin_put(const char *name, const uint8_t pk[32])
{
    nvs_handle_t h;
    if (nvs_open("pins", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, name, pk, 32); nvs_commit(h); nvs_close(h);
}
bool whm_pin_del(const char *name)
{
    nvs_handle_t h; bool ok = false;
    if (nvs_open("pins", NVS_READWRITE, &h) != ESP_OK) return false;
    ok = nvs_erase_key(h, name) == ESP_OK;
    nvs_commit(h); nvs_close(h);
    return ok;
}
int whm_pin_count(void)
{
    int n = 0;
    nvs_iterator_t it = NULL;
    if (nvs_entry_find("nvs", "pins", NVS_TYPE_BLOB, &it) != ESP_OK)
        return 0;
    while (it) { n++; if (nvs_entry_next(&it) != ESP_OK) break; }
    nvs_release_iterator(it);
    return n;
}
int whm_pin_list(void (*cb)(const char *, const uint8_t *))
{
    nvs_iterator_t it = NULL; int cnt = 0;
    if (nvs_entry_find("nvs", "pins", NVS_TYPE_BLOB, &it) != ESP_OK)
        return 0;
    while (it) {
        nvs_entry_info_t info; nvs_entry_info(it, &info);
        uint8_t pk[32];
        if (whm_pin_get(info.key, pk)) { cb(info.key, pk); cnt++; }
        if (nvs_entry_next(&it) != ESP_OK) break;
    }
    nvs_release_iterator(it);
    return cnt;
}
