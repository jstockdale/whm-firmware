/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/* crypto_id.c - node identity (Ed25519 via vendored Monocypher) +
 * TOFU pin store. SSH-model: first-seen pubkey per name is pinned;
 * a later mismatch is screamed about and REJECTED, never silently
 * re-pinned.
 *
 * 0.55.1 REWRITE: all persistence now rides the NVS BROKER
 * (whm_settings_*) as hex strings. The direct nvs_* calls executed
 * flash writes from whatever task called pin_put - on Two that was
 * the sync rx task, PSRAM-adjacent, and the cache-disable assert
 * boot-looped the unit the moment it heard One's first IDENT. The
 * broker exists precisely so flash work happens on its own
 * internal-stack context; identity now uses the front door. */
#include "crypto_id.h"
#include "sync.h"
#include "monocypher.h"
#include "settings.h"
#include "esp_random.h"
#include <string.h>
#include <stdio.h>

static uint8_t s_sk[64], s_pk[32];
static uint8_t s_xsk[32], s_xpk[32];
static bool s_up;

static void hexw(const uint8_t *b, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 15];
    }
    out[2 * n] = 0;
}
static bool hexr(const char *s, uint8_t *b, int n)
{
    for (int i = 0; i < 2 * n; i++) {
        char c = s[i]; int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else return false;
        if (i & 1) b[i >> 1] |= v; else b[i >> 1] = v << 4;
    }
    return s[2 * n] == 0;
}
static bool ld(const char *key, uint8_t *b, int n)
{
    char tmp[160];
    if (whm_settings_get_str(key, tmp, 144) != ESP_OK)
        /* <= VAL_MAX so the GET rides the broker too (hygiene;
           the internal stack above is the real guarantee) */
        return false;
    return hexr(tmp, b, n);
}
static void sv(const char *key, const uint8_t *b, int n)
{
    char tmp[160];
    hexw(b, n, tmp);
    whm_settings_set_str(key, tmp);
}

void whm_id_init(void)
{
    if (ld("cid.sk", s_sk, 64) && ld("cid.pk", s_pk, 32) &&
        ld("cid.xsk", s_xsk, 32) && ld("cid.xpk", s_xpk, 32)) {
        s_up = true;
        return;
    }
    uint8_t seed[32];
    esp_fill_random(seed, sizeof(seed));
    crypto_eddsa_key_pair(s_sk, s_pk, seed);
    esp_fill_random(s_xsk, 32);
    crypto_x25519_public_key(s_xpk, s_xsk);
    sv("cid.sk", s_sk, 64);
    sv("cid.pk", s_pk, 32);
    sv("cid.xsk", s_xsk, 32);
    sv("cid.xpk", s_xpk, 32);
    char fp[17]; whm_id_fp(s_pk, fp);
    whm_lts(); printf("identity: NEW keypair minted, fp %s\n", fp);
    s_up = true;
}
const uint8_t *whm_id_pk(void) { return s_pk; }
const uint8_t *whm_id_xpk(void) { return s_xpk; }
void whm_id_xshared(const uint8_t their_xpk[32], uint8_t out[32])
{
    crypto_x25519(out, s_xsk, their_xpk);
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
    hexw(pk, 8, out);
}

/* ---- pin registry: names in "pin.idx" (comma list), keys per
        name as hex under "pin.<name>" / "pinx.<name>" ------------ */
static void reg_get(char *out, size_t n)
{
    if (whm_settings_get_str("pin.idx", out, n) != ESP_OK) out[0] = 0;
}
static bool reg_has(const char *reg, const char *name)
{
    size_t l = strlen(name);
    const char *p = reg;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t seg = e ? (size_t)(e - p) : strlen(p);
        if (seg == l && strncmp(p, name, l) == 0) return true;
        p = e ? e + 1 : p + seg;
    }
    return false;
}
bool whm_pin_get(const char *name, uint8_t pk[32])
{
    char key[48];
    snprintf(key, sizeof(key), "pin.%s", name);
    return ld(key, pk, 32);
}
void whm_pin_put(const char *name, const uint8_t pk[32])
{
    char key[48];
    snprintf(key, sizeof(key), "pin.%s", name);
    sv(key, pk, 32);
    char reg[160];
    reg_get(reg, sizeof(reg));
    if (!reg_has(reg, name)) {
        char nr[192];
        snprintf(nr, sizeof(nr), "%s%s%s", reg, reg[0] ? "," : "",
                 name);
        whm_settings_set_str("pin.idx", nr);
    }
}
bool whm_pinx_get(const char *name, uint8_t xpk[32])
{
    char key[48];
    snprintf(key, sizeof(key), "pinx.%s", name);
    return ld(key, xpk, 32);
}
void whm_pinx_put(const char *name, const uint8_t xpk[32])
{
    char key[48];
    snprintf(key, sizeof(key), "pinx.%s", name);
    sv(key, xpk, 32);
}
bool whm_pin_del(const char *name)
{
    char key[48], reg[96], nr[96];
    snprintf(key, sizeof(key), "pin.%s", name);
    whm_settings_erase(key);
    snprintf(key, sizeof(key), "pinx.%s", name);
    whm_settings_erase(key);
    reg_get(reg, sizeof(reg));
    nr[0] = 0;
    const char *p = reg; size_t l = strlen(name);
    bool removed = false;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t seg = e ? (size_t)(e - p) : strlen(p);
        if (!(seg == l && strncmp(p, name, l) == 0)) {
            size_t cur = strlen(nr);
            snprintf(nr + cur, sizeof(nr) - cur, "%s%.*s",
                     nr[0] ? "," : "", (int)seg, p);
        } else removed = true;
        p = e ? e + 1 : p + seg;
    }
    whm_settings_set_str("pin.idx", nr);
    return removed;
}
int whm_pin_count(void)
{
    char reg[160];
    reg_get(reg, sizeof(reg));
    if (!reg[0]) return 0;
    int n = 1;
    for (const char *p = reg; *p; p++) if (*p == ',') n++;
    return n;
}
int whm_pin_list(void (*cb)(const char *, const uint8_t *))
{
    char reg[160];
    reg_get(reg, sizeof(reg));
    int cnt = 0;
    char *p = reg;
    while (*p) {
        char *e = strchr(p, ',');
        if (e) *e = 0;
        uint8_t pk[32];
        if (whm_pin_get(p, pk)) { if (cb) cb(p, pk); cnt++; }
        p = e ? e + 1 : p + strlen(p);
    }
    return cnt;
}
