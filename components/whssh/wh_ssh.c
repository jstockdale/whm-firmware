/* ssh.c — wolfSSH SSH-2.0 server for the Whitehat Watch.
 *
 * SSH in over WiFi -> the exact console command set (wh_console_exec), encrypted.
 * Public-key auth only; keys come from /sd/.ssh/authorized_keys with an NVS
 * fallback. One session at a time (memory: the handshake peaks ~35-45 KB and we
 * run tight alongside WiFi+BLE). The shell channel is bridged to the transport-
 * agnostic console dispatch, so there is no shell/PTY exec and a session can only
 * reach our own commands.
 *
 * Phases 4 (server core + bridge), 5 (authorized_keys), 6 (hardening) all live
 * here. [VERIFY-HW]: the wolfSSH API calls and the WS_UserAuthData field paths
 * are written from the docs + echoserver example and are pending first on-device
 * bring-up; expect minor API/config adjustments once it compiles against the real
 * wolfSSL/wolfSSH components. */
#include "freertos/idf_additions.h"
#include "whssh.h"
#if defined(WH_HAVE_WOLFSSH)
#include "wh_lineedit.h"
#include "wh_conlog.h"
#include "esp_console.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include <sys/socket.h>

/* ---- panel glue: the watch services this file expects ---- */
/* storage_*: REAL now. The port shipped these as always-fail stubs
 * behind a comment claiming 'the panels mount no SD' - while the
 * owner's boot log listed a 30 GB card at /sdcard. The stub made the
 * honest error message a gaslight ('no card mounted' with a card
 * mounted). SD is hereby the PRIMARY key store again, exactly the
 * watch's design: survives an NVS erase, readable off-device. */
#include <sys/stat.h>
#include <errno.h>
#define WHSSH_SD_ROOT "/sdcard/"
static void wh_storage_path(const char *rel, char *out, size_t cap)
{ snprintf(out, cap, WHSSH_SD_ROOT "%s", rel); }
static int wh_storage_read_file(const char *rel, char *b, int n)
{
    char p[96]; wh_storage_path(rel, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    int r = (int)fread(b, 1, (size_t)(n - 1), f);
    fclose(f);
    if (r >= 0) b[r] = '\0';
    return r;
}
static int wh_storage_append_file(const char *rel, const char *txt)
{
    char p[96]; wh_storage_path(rel, p, sizeof p);
    mkdir(WHSSH_SD_ROOT ".ssh", 0775);      /* EEXIST is fine */
    FILE *f = fopen(p, "a");
    if (!f) return -1;
    int ok = fputs(txt, f) >= 0;
    fclose(f);
    return ok ? 0 : -1;
}
static int wh_storage_write_file(const char *rel, const char *txt)
{
    char p[96]; wh_storage_path(rel, p, sizeof p);
    mkdir(WHSSH_SD_ROOT ".ssh", 0775);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    int ok = fputs(txt, f) >= 0;
    fclose(f);
    return ok ? 0 : -1;
}
/* wh_task_start: plain pinned task; SSH lives on core 0 beside WiFi,
 * far from the core-1 HUB75/UI path. */
static esp_err_t wh_task_start_(const char *name, void (*fn)(void *),
                                uint32_t stack, unsigned prio, int core)
{
    /* RAM AUDIT w1: PSRAM stack for the listener+session task.
       Safe here: S3 crypto is register-mode (no DMA touches this
       stack), keys persist via NVS ONLY from the USB console path,
       and the task never runs flash ops - the watch kept this
       internal out of caution the panels cannot afford. */
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack, NULL,
               prio, NULL, core, MALLOC_CAP_SPIRAM)
           == pdPASS ? ESP_OK : ESP_FAIL;
}
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"        /* EXT_RAM_BSS_ATTR */
#include "driver/usb_serial_jtag.h"  /* usb_serial_jtag_write_bytes (visible failure log) */
#include "multi_heap.h"        /* wolfSSL/wolfSSH crypto arena */
#include "freertos/semphr.h"   /* arena mutex */
#include <stdio.h>
#include <string.h>
#include <wolfssh/log.h>
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <wolfssl/wolfcrypt/memory.h>
#include <wolfssh/ssh.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#include <wolfssl/wolfcrypt/asn_public.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ssh";

#define SSH_PORT            22
#define SSH_MAX_KEYS         8
#define SSH_MAX_KEYBLOB    256   /* an ed25519/ecdsa public-key blob is well under this */
#define SSH_HOSTKEY_MAX    256   /* a P-256 SEC1 DER private key is 121 bytes */
#define SSH_HANDSHAKE_SECS  15
#define SSH_IDLE_SECS      300
#define SSH_AUTHKEYS_RELPATH ".ssh/authorized_keys"   /* under the SD mount (/sdcard) */

/* Per-channel flow-control window + max packet. wolfSSH's defaults (128 KB / 32 KB) size a
 * 128 KB buffer per channel, which failed to allocate right after auth. 16 KB is plenty for
 * an interactive console; window == maxPacket so a full packet always fits the window. */
#define WH_SSH_WINDOW_SZ    (8 * 1024)   /* channel receive-window buffer, from the crypto arena */
#define WH_SSH_MAXPACKET_SZ (8 * 1024)


/* ---- authorized_keys store (Phase 5) ---- */
typedef struct { unsigned char blob[SSH_MAX_KEYBLOB]; word32 len; } authkey_t;
EXT_RAM_BSS_ATTR static authkey_t s_keys[SSH_MAX_KEYS];   /* PSRAM (ssh/console tasks, no ISR/critsec) */
static int       s_nkeys;

static WOLFSSH_CTX     *s_ctx;
static volatile bool    s_busy;    /* one session at a time */
static volatile bool    s_enabled; /* runtime gate */

/* ---- host key (per-device, generated once, stored in NVS) ---- */
static unsigned char    s_hostkey[SSH_HOSTKEY_MAX];
static word32           s_hostkey_len;
static const char      *s_hostkey_src = "none";

/* ---- tiny base64 decoder (for authorized_keys blobs) ---- */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static int b64_decode(const char *in, unsigned char *out, int out_max)
{
    int bits = 0, acc = 0, n = 0;
    for (; *in && *in != ' ' && *in != '\n' && *in != '\r'; in++) {
        if (*in == '=') break;
        int v = b64_val(*in);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= out_max) return -1; out[n++] = (unsigned char)((acc >> bits) & 0xFF); }
    }
    return n;
}

/* Parse one authorized_keys line ("ssh-ed25519 AAAA... comment") -> stored blob.
 * We store the base64-decoded key blob and compare it verbatim against the blob
 * wolfSSH hands the auth callback. */
static void authkeys_add_line(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == '\0') return;
    const char *sp = strchr(line, ' ');           /* skip the "ssh-ed25519 " type token */
    if (!sp) return;
    const char *b64 = sp + 1;
    if (s_nkeys >= SSH_MAX_KEYS) return;
    int n = b64_decode(b64, s_keys[s_nkeys].blob, SSH_MAX_KEYBLOB);
    if (n > 0) { s_keys[s_nkeys].len = (word32)n; s_nkeys++; }
}

static void authkeys_load_sd(void)
{
    /* Read the whole file once under the SD bus lock (shared with the capture
     * logger), then parse it line by line - same as the NVS path below. */
    char *buf = malloc(4096);
    if (!buf) return;
    int nread = wh_storage_read_file(SSH_AUTHKEYS_RELPATH, buf, 4096);
    if (nread > 0) {
        char *pp = buf, *nl;
        while ((nl = strchr(pp, '\n')) != NULL) { *nl = '\0'; authkeys_add_line(pp); pp = nl + 1; }
        if (*pp) authkeys_add_line(pp);
        ESP_LOGI(TAG, "loaded %d key(s) from SD %s", s_nkeys, SSH_AUTHKEYS_RELPATH);
    }
    free(buf);
}

static void authkeys_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("ssh", NVS_READONLY, &h) != ESP_OK) return;
    /* one newline-separated authorized_keys blob under "authkeys" */
    size_t sz = 0;
    if (nvs_get_blob(h, "authkeys", NULL, &sz) == ESP_OK && sz > 0 && sz < 4096) {
        char *buf = malloc(sz + 1);
        if (buf && nvs_get_blob(h, "authkeys", buf, &sz) == ESP_OK) {
            buf[sz] = '\0';
            char *p = buf, *nl;
            while ((nl = strchr(p, '\n')) != NULL) { *nl = '\0'; authkeys_add_line(p); p = nl + 1; }
            if (*p) authkeys_add_line(p);
        }
        free(buf);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "authorized_keys total after NVS: %d", s_nkeys);
}

static void authkeys_load(void)
{
    s_nkeys = 0;
    authkeys_load_sd();                                /* SD first (the primary store) */
    if (s_nkeys == 0) authkeys_load_nvs();            /* NVS fallback (card-less) */
    if (s_nkeys == 0) ESP_LOGW(TAG, "no authorized_keys - no client can authenticate");
}

/* Append one authorized key ("<type> <base64>") to the NVS blob and reload.
 * Lets a card-less device be provisioned over the serial console
 * (`ssh addkey ...`). Returns true on success. [VERIFY-HW] */
static bool authkeys_nvs_append(const char *type, const char *b64)
{
    unsigned char probe[SSH_MAX_KEYBLOB];             /* reject garbage before storing */
    if (b64_decode(b64, probe, sizeof probe) <= 0) return false;

    char *buf = malloc(2048);                         /* off the 4 KB console stack */
    if (!buf) return false;
    bool ok = false;
    nvs_handle_t h;
    if (nvs_open("ssh", NVS_READWRITE, &h) == ESP_OK) {
        size_t off = 2048 - 1;
        if (nvs_get_blob(h, "authkeys", buf, &off) != ESP_OK) off = 0;   /* none yet */
        int wrote = snprintf(buf + off, 2048 - off, "%s %s\n", type, b64);
        if (wrote > 0 && (size_t)wrote < 2048 - off) {
            if (nvs_set_blob(h, "authkeys", buf, off + wrote) == ESP_OK && nvs_commit(h) == ESP_OK)
                ok = true;
        }
        nvs_close(h);
    }
    free(buf);
    if (ok) authkeys_load();                          /* pick it up immediately */
    return ok;
}

/* Append one authorized key to the SD authorized_keys file, creating .ssh/ and the
 * file if absent, then reload. SD is the primary store (read before NVS), so a key
 * added here is the one that takes effect and it survives an NVS erase. [VERIFY-HW] */
static bool authkeys_sd_append(const char *type, const char *b64)
{
    /* Buffers on the heap, not the stack: this runs inline on the console/ssh task and
     * the FATFS write path underneath is already stack-hungry, so keep our own peak low. */
    unsigned char *probe = malloc(SSH_MAX_KEYBLOB);   /* reject garbage before writing */
    char *line = malloc(SSH_MAX_KEYBLOB * 2);
    bool ok = false;
    if (probe && line && b64_decode(b64, probe, SSH_MAX_KEYBLOB) > 0) {
        int w = snprintf(line, SSH_MAX_KEYBLOB * 2, "%s %s\n", type, b64);
        if (w > 0 && (size_t)w < (size_t)(SSH_MAX_KEYBLOB * 2)
            && wh_storage_append_file(SSH_AUTHKEYS_RELPATH, line) == 0) {
            authkeys_load();                          /* SD first, so this key is now live */
            ok = true;
        }
    }
    free(probe);
    free(line);
    return ok;
}

/* Is this a key line (vs a comment/blank)? Mirrors authkeys_add_line's acceptance:
 * non-blank, not starting with '#', and has a "<type> <base64>" split. */
static bool authkeys_is_keyline(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == '\0' || *line == '\r') return false;
    return strchr(line, ' ') != NULL;
}

static bool authkeys_text_has_key(const char *buf)
{
    for (const char *p = buf; *p; ) {
        const char *e = p; while (*e && *e != '\n') e++;
        const char *q = p; while (q < e && (*q == ' ' || *q == '\t')) q++;
        if (q < e && *q != '#' && *q != '\r') {
            const char *sp = q; while (sp < e && *sp != ' ') sp++;
            if (sp < e) return true;
        }
        p = (*e == '\n') ? e + 1 : e;
    }
    return false;
}

/* Load the active authorized_keys store text into buf. SD is active iff it holds at
 * least one key (mirrors authkeys_load's SD-first rule); otherwise NVS. Returns the
 * store: 1 = SD, 2 = NVS, 0 = none/empty. */
static int authkeys_active_store(char *buf, size_t cap)
{
    if (cap == 0) return 0;
    buf[0] = '\0';
    int n = wh_storage_read_file(SSH_AUTHKEYS_RELPATH, buf, cap);
    if (n > 0 && authkeys_text_has_key(buf)) return 1;   /* SD */
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open("ssh", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = cap - 1;
        if (nvs_get_blob(h, "authkeys", buf, &sz) == ESP_OK && sz < cap) buf[sz] = '\0';
        else buf[0] = '\0';
        nvs_close(h);
    }
    return buf[0] ? 2 : 0;
}

/* `ssh keys` - list the active store's authorized keys with an index + a short id. */
static void ssh_cmd_keys(wh_out_fn out, void *ctx)
{
    char *buf = malloc(4096);
    if (!buf) { out(ctx, "keys: out of memory\n"); return; }
    int store = authkeys_active_store(buf, 4096);
    if (store == 0) { out(ctx, "no authorized keys\n"); free(buf); return; }
    { char hdr[48]; snprintf(hdr, sizeof hdr, "authorized keys (%s):\n", store == 1 ? "SD" : "NVS"); out(ctx, hdr); }
    int idx = 0;
    for (char *p = buf; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        if (authkeys_is_keyline(p)) {
            char lc[300]; snprintf(lc, sizeof lc, "%s", p);
            char *t = lc; while (*t == ' ' || *t == '\t') t++;
            char *sp1 = strchr(t, ' ');
            char *b = sp1 ? sp1 + 1 : t;
            if (sp1) *sp1 = '\0';
            char *sp2 = b ? strchr(b, ' ') : NULL;
            const char *cmt = "";
            if (sp2) { *sp2 = '\0'; cmt = sp2 + 1; }
            size_t bl = b ? strlen(b) : 0;
            const char *tail = (bl > 12) ? b + bl - 12 : b;
            char row[160]; snprintf(row, sizeof row, "  [%d] %-11.16s ...%.16s  %.48s\n", idx, t, tail, cmt);
            out(ctx, row);
            idx++;
        }
        p = nl ? nl + 1 : NULL;
    }
    out(ctx, idx ? "remove one with: ssh rmkey <n>\n" : "  (none)\n");
    free(buf);
}

/* `ssh rmkey <n>` - drop the n-th key from the active store, rewrite it, reload. */
static void ssh_cmd_rmkey(wh_out_fn out, void *ctx, int target)
{
    char *buf = malloc(4096), *nbuf = malloc(4096);
    if (!buf || !nbuf) { out(ctx, "rmkey: out of memory\n"); free(buf); free(nbuf); return; }
    int store = authkeys_active_store(buf, 4096);
    if (store == 0) { out(ctx, "no authorized keys\n"); free(buf); free(nbuf); return; }

    int idx = 0; size_t no = 0; bool removed = false;
    for (char *p = buf; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        bool key = authkeys_is_keyline(p);
        if (key && idx++ == target) {
            removed = true;                            /* drop this line */
        } else {
            size_t ll = strlen(p);
            if (no + ll + 1 < 4096) { memcpy(nbuf + no, p, ll); no += ll; nbuf[no++] = '\n'; }
        }
        p = nl ? nl + 1 : NULL;
    }
    nbuf[no] = '\0';

    if (!removed) {
        char m[64]; snprintf(m, sizeof m, "no key #%d (see 'ssh keys')\n", target); out(ctx, m);
        free(buf); free(nbuf); return;
    }

    bool ok = false;
    if (store == 1) {
        ok = (wh_storage_write_file(SSH_AUTHKEYS_RELPATH, nbuf) == 0);
    } else {
        nvs_handle_t h;
        if (nvs_open("ssh", NVS_READWRITE, &h) == ESP_OK) {
            if (no == 0) ok = (nvs_erase_key(h, "authkeys") == ESP_OK && nvs_commit(h) == ESP_OK);
            else         ok = (nvs_set_blob(h, "authkeys", nbuf, no) == ESP_OK && nvs_commit(h) == ESP_OK);
            nvs_close(h);
        }
    }
    if (ok) {
        authkeys_load();
        char m[72]; snprintf(m, sizeof m, "removed key #%d - %d authorized key(s) now\n", target, s_nkeys); out(ctx, m);
    } else {
        out(ctx, "rmkey: write failed\n");
    }
    free(buf); free(nbuf);
}

/* ---- host key: prefer a per-device NVS key; generate one if absent ---- */
static bool hostkey_load_nvs(unsigned char *der, word32 *derlen)
{
    nvs_handle_t h;
    if (nvs_open("ssh", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = SSH_HOSTKEY_MAX;
    esp_err_t e = nvs_get_blob(h, "hostkey", der, &sz);
    nvs_close(h);
    if (e == ESP_OK && sz > 0) { *derlen = (word32)sz; return true; }
    return false;
}

static bool hostkey_store_nvs(const unsigned char *der, word32 derlen)
{
    nvs_handle_t h;
    if (nvs_open("ssh", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, "hostkey", der, derlen);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

/* Generate a fresh P-256 host key -> SEC1 DER (same format as the embedded key).
 * ecc_key + RNG are heap-allocated to keep this off small task stacks; with
 * WOLFSSL_SMALL_STACK the keygen internals also use the heap. [VERIFY-HW] */
static bool hostkey_generate(unsigned char *der, word32 *derlen)
{
    WC_RNG *rng = malloc(sizeof(WC_RNG));
    ecc_key *key = malloc(sizeof(ecc_key));
    bool ok = false;
    if (rng && key && wc_InitRng(rng) == 0) {
        if (wc_ecc_init(key) == 0) {
            if (wc_ecc_make_key(rng, 32, key) == 0) {   /* 32 bytes -> secp256r1 */
                int n = wc_EccKeyToDer(key, der, SSH_HOSTKEY_MAX);
                if (n > 0) { *derlen = (word32)n; ok = true; }
            }
            wc_ecc_free(key);
        }
        wc_FreeRng(rng);
    }
    free(key);
    free(rng);
    return ok;
}

/* Choose the host key. Returns true only if we have a UNIQUE, per-device key.
 *
 * The design goal: one firmware image can flash any number of units and no two
 * ever share a host key. The key is therefore created at RUNTIME on first boot
 * from the hardware RNG (never baked into the image), stored in NVS, and reused
 * on every later boot. If we cannot obtain a unique key we fail closed and leave
 * SSH off -- we never fall back to a shared, image-baked key. */
static bool hostkey_select(void)
{
    if (hostkey_load_nvs(s_hostkey, &s_hostkey_len)) {
        s_hostkey_src = "NVS (per-device)";
        return true;
    }
    /* No stored key yet -> mint one for THIS device. */
    if (hostkey_generate(s_hostkey, &s_hostkey_len)) {
        if (hostkey_store_nvs(s_hostkey, s_hostkey_len)) {
            s_hostkey_src = "generated -> NVS (per-device)";
            ESP_LOGI(TAG, "minted a per-device host key and stored it in NVS");
        } else {
            /* Unique but unpersisted: it changes next boot (clients see a host-
             * key-changed notice), yet it is still never shared between devices. */
            s_hostkey_src = "generated, ephemeral (NVS write failed)";
            ESP_LOGW(TAG, "host key generated but not persisted - changes on reboot");
        }
        return true;
    }
    /* Could not produce a unique key. Do NOT substitute a shared one. */
    ESP_LOGE(TAG, "host key generation failed - SSH disabled (no shared-key fallback)");
    s_hostkey_src = "none (keygen failed)";
    return false;
}

/* ---- user-auth callback (Phase 5): public key only ---- */
static int ssh_userauth(byte authType, WS_UserAuthData *authData, void *ctx)
{
    (void)ctx;
    const char *user = "?"; int userSz = 1;
    if (authData && authData->username) { user = (const char *)authData->username; userSz = (int)authData->usernameSz; }

    if (authType != WOLFSSH_USERAUTH_PUBLICKEY) {
        ESP_LOGW(TAG, "auth: user '%.*s' tried %s - refused (pubkey only)", userSz, user,
                 authType == WOLFSSH_USERAUTH_PASSWORD ? "password" : "non-pubkey method");
        return WOLFSSH_USERAUTH_FAILURE;               /* passwords disabled */
    }
    const unsigned char *offered = authData->sf.publicKey.publicKey;
    word32 offeredSz = authData->sf.publicKey.publicKeySz;
    const char *ktype = (const char *)authData->sf.publicKey.publicKeyType;
    int ktypeSz = (int)authData->sf.publicKey.publicKeyTypeSz;
    for (int i = 0; i < s_nkeys; i++) {
        if (s_keys[i].len == offeredSz && memcmp(s_keys[i].blob, offered, offeredSz) == 0) {
            ESP_LOGI(TAG, "auth: user '%.*s' pubkey ACCEPTED (%.*s, authorized key #%d)",
                     userSz, user, ktypeSz, ktype, i);
            return WOLFSSH_USERAUTH_SUCCESS;
        }
    }
    ESP_LOGW(TAG, "auth: user '%.*s' pubkey REJECTED (%.*s, %u B, not among %d authorized key(s))",
             userSz, user, ktypeSz, ktype, (unsigned)offeredSz, s_nkeys);
    return WOLFSSH_USERAUTH_INVALID_PUBLICKEY;
}

/* ---- console output sink -> SSH channel, with \n -> \r\n for terminals ---- */
static void ssh_out(void *vssh, const char *s)
{
    WOLFSSH *ssh = (WOLFSSH *)vssh;
    const char *p = s;
    char buf[128];
    int n = 0;
    while (*p) {
        if (*p == '\n' && n < (int)sizeof buf - 1) buf[n++] = '\r';
        buf[n++] = *p++;
        if (n >= (int)sizeof buf - 2) { wolfSSH_stream_send(ssh, (byte *)buf, n); n = 0; }
    }
    if (n) wolfSSH_stream_send(ssh, (byte *)buf, n);
}

/* ---- the channel as a stdio stream: fopencookie write with \n -> \r\n ---- */
static ssize_t ssh_cookie_write(void *vssh, const char *b, size_t n)
{
    WOLFSSH *ssh = (WOLFSSH *)vssh;
    char buf[160]; int o = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == '\n' && o < (int)sizeof buf - 1) buf[o++] = '\r';
        buf[o++] = b[i];
        if (o >= (int)sizeof buf - 2) {
            wolfSSH_stream_send(ssh, (byte *)buf, o); o = 0;
        }
    }
    if (o) wolfSSH_stream_send(ssh, (byte *)buf, o);
    return (ssize_t)n;
}

/* ---- panel MOTD: identity + link + memory, printf -> the channel ---- */
static void panel_motd(const char *peer_ip)
{
    const esp_app_desc_t *ad = esp_app_get_description();
    printf("\r\n\x1b[38;5;44m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\x1b[0m\r\n");
    printf("  \x1b[1mWHM panel\x1b[0m · %s · IDF %s\r\n",
           ad->version, ad->idf_ver);
    {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ii;
        wifi_ap_record_t ap;
        if (nif && esp_netif_get_ip_info(nif, &ii) == ESP_OK && ii.ip.addr)
            printf("  ip " IPSTR, IP2STR(&ii.ip));
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            printf("   rssi %d dBm   '%s'", ap.rssi, (const char *)ap.ssid);
        printf("\r\n");
    }
    printf("  up %llus   heap %uK   psram %uK\r\n",
           (unsigned long long)(esp_timer_get_time() / 1000000),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("  client %s   ·   \x1b[38;5;42mhelp\x1b[0m for commands, "
           "\x1b[38;5;42mexit\x1b[0m to leave\r\n",
           peer_ip ? peer_ip : "?");
    printf("\x1b[38;5;44m━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\x1b[0m\r\n\r\n");
}

/* ---- one session: handshake, then drive the shared console loop over the channel ---- */

/* Buffered, non-blocking-ish reader: hands the console loop one byte at a time,
 * refilling from the channel with a short socket timeout so the loop stays free to
 * drain job output + poll for ^C. Returns -1 when no byte is ready right now, or -2
 * on disconnect / idle timeout (ends the session). */
typedef struct { WOLFSSH *ssh; byte buf[128]; int len; int pos; int64_t last_us; } ssh_io_t;

static int ssh_getch(void *vio)
{
    ssh_io_t *io = (ssh_io_t *)vio;
    if (io->pos >= io->len) {
        int n = wolfSSH_stream_read(io->ssh, io->buf, sizeof io->buf);
        if (n <= 0) {
            /* wolfSSH often returns a generic negative (WS_FATAL_ERROR) and puts the real
             * status in wolfSSH_get_error(); a bare "n == WS_WANT_READ" check misses that and
             * kills the session on the first idle read. Treat want-read/write and an
             * in-progress rekey as "nothing right now", everything else as a real end. */
            int err = wolfSSH_get_error(io->ssh);
            if (n == WS_WANT_READ  || n == WS_WANT_WRITE ||
                err == WS_WANT_READ || err == WS_WANT_WRITE || err == WS_REKEYING) {
                if (esp_timer_get_time() - io->last_us > (int64_t)SSH_IDLE_SECS * 1000000)
                    return -2;                                /* idle too long: drop the session */
                return -1;                                    /* no byte ready now */
            }
            ESP_LOGW(TAG, "ssh read ended: ret=%d err=%d", n, err);   /* EOF / transport error */
            return -2;
        }
        io->len = n;
        io->pos = 0;
        io->last_us = esp_timer_get_time();
    }
    return io->buf[io->pos++];
}

static void ssh_session(int fd)
{
    WOLFSSH *ssh = wolfSSH_new(s_ctx);
    if (!ssh) { ESP_LOGE(TAG, "wolfSSH_new failed"); return; }
    wolfSSH_set_fd(ssh, fd);

    if (wolfSSH_accept(ssh) != WS_SUCCESS) {
        ESP_LOGW(TAG, "handshake failed: %d", wolfSSH_get_error(ssh));
        wolfSSH_free(ssh);
        return;
    }
    ESP_LOGI(TAG, "session up");

    /* Post-handshake: shorten the read timeout so the console loop can drain job
     * output and poll for ^C between keystrokes (accept above used the longer
     * handshake timeout the caller set). */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 40000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    ssh_io_t io = { .ssh = ssh, .buf = {0}, .len = 0, .pos = 0, .last_us = esp_timer_get_time() };

    /* Resolve the connecting client for the MOTD (best-effort). */
    char peer_ip[INET_ADDRSTRLEN] = "?";
    struct sockaddr_in pa; socklen_t pl = sizeof pa;
    if (getpeername(fd, (struct sockaddr *)&pa, &pl) == 0)
        inet_ntop(AF_INET, &pa.sin_addr, peer_ip, sizeof peer_ip);

    /* THE FLEET BRIDGE: esp_console commands print via printf, and newlib
       stdio is PER-TASK - so this session swaps ITS OWN stdout/stderr onto
       the SSH channel with fopencookie, then runs lines through
       esp_console_run(). Every registered fleet verb (help, fleet, version,
       wifi, show, keys, ...) works over SSH verbatim, zero duplication, and
       the USB REPL on its own task is untouched. The line editor + its
       24-deep history (~4.9 KB) parks in PSRAM: task-only, keystroke-rate,
       never ISR/DMA - the placement law's easiest tenant. */
    FILE *chan = fopencookie(ssh, "w", (cookie_io_functions_t){
        .write = ssh_cookie_write });
    wh_lineedit_t *le = heap_caps_malloc(sizeof *le, MALLOC_CAP_SPIRAM);
    if (!le) le = malloc(sizeof *le);
    if (chan && le) {
        setvbuf(chan, NULL, _IONBF, 0);
        FILE *old_out = stdout, *old_err = stderr;
        stdout = chan; stderr = chan;
        panel_motd(peer_ip);
        wh_le_init(le, "whm> ");
        char emit[512], line[WH_LE_LINE_MAX];
        char logl[160], fmtd[360];
        size_t pn = wh_le_prompt(le, emit, sizeof emit - 1);
        emit[pn] = '\0'; fputs(emit, chan);
        whssh_log_set_active(true);
        int alive = 1;
        while (alive) {
            bool any = false;
            while (whssh_log_take(logl, sizeof logl)) {
                if (!any) { fputs("\r\x1b[K", chan); any = true; }
                wh_log_line_fmt(logl, fmtd, sizeof fmtd);
                fputs(fmtd, chan);
            }
            {
                unsigned dp = whssh_log_dropped_take();
                if (dp) {
                    if (!any) { fputs("\r\x1b[K", chan); any = true; }
                    fprintf(chan, "\x1b[2m-- %u log line%s dropped --"
                                  "\x1b[0m\r\n", dp, dp == 1 ? "" : "s");
                }
            }
            if (any) {
                size_t rn = wh_le_render(le, emit, sizeof emit - 1);
                emit[rn] = '\0'; fputs(emit, chan);
            }
            int ch = ssh_getch(&io);
            if (ch == -2) break;
            if (ch < 0) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            size_t olen = 0;
            int done = wh_le_feed(le, (char)ch, emit, sizeof emit,
                                  &olen, line, sizeof line);
            if (olen) { emit[olen] = '\0'; fputs(emit, chan); }
            if (!done) continue;
            if (line[0]) {
                if (!strcmp(line, "exit") || !strcmp(line, "quit") ||
                    !strcmp(line, "logout")) {
                    fputs("bye\r\n", chan);
                    alive = 0;
                } else {
                    int rr = 0;
                    esp_err_t e = esp_console_run(line, &rr);
                    if (e == ESP_ERR_NOT_FOUND)
                        fprintf(chan,
                                "unknown command '%s' (try help)\r\n",
                                line);
                }
            }
            if (alive) {
                pn = wh_le_prompt(le, emit, sizeof emit - 1);
                emit[pn] = '\0'; fputs(emit, chan);
            }
        }
        whssh_log_set_active(false);
        stdout = old_out; stderr = old_err;
    } else {
        ESP_LOGE(TAG, "session: out of memory for bridge");
    }
    if (chan) fclose(chan);
    if (le) free(le);

    wolfSSH_stream_exit(ssh, 0);
    wolfSSH_free(ssh);
    ESP_LOGI(TAG, "session closed");
}

/* ---- server task: listen + accept, one session at a time ---- */

static void ssh_task(void *arg)
{
    (void)arg;
    /* THE MBOX LESSON (owner's boot log): lwIP sockets before the
       tcpip thread exists trip 'Invalid mbox' and ABORT - the retry
       loop never ran because the first socket() call died. Wait for
       the STA netif to exist (created during wifi init) before ever
       touching the socket API. */
    while (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL)
        vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(200));      /* let tcpip settle */
    /* PANEL RESILIENCE: the watch exited on bind failure; a wall
       panel must survive any boot order and every WiFi outage, so
       the listener retries forever with a gentle backoff. */
    int lfd = -1;
    for (;;) {
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd >= 0) {
            int one = 1;
            setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one,
                       sizeof one);
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(SSH_PORT);
            if (bind(lfd, (struct sockaddr *)&addr,
                     sizeof addr) == 0 && listen(lfd, 1) == 0)
                break;
            close(lfd);
        }
        static bool warned;
        if (!warned) {
            ESP_LOGW(TAG, "ssh listener not up yet - retrying");
            warned = true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "listening on :%d", SSH_PORT);

    for (;;) {
        struct sockaddr_in peer; socklen_t plen = sizeof peer;
        int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        uint32_t a = ntohl(peer.sin_addr.s_addr);
        ESP_LOGI(TAG, "connection from %u.%u.%u.%u:%u",
                 (unsigned)((a >> 24) & 0xff), (unsigned)((a >> 16) & 0xff),
                 (unsigned)((a >> 8) & 0xff), (unsigned)(a & 0xff), (unsigned)ntohs(peer.sin_port));
        if (!s_enabled || s_busy) {                    /* one session; refuse extras */
            ESP_LOGI(TAG, "  refused (%s)", s_enabled ? "session already active" : "ssh disabled");
            const char *msg = "ssh busy\r\n";
            send(fd, msg, strlen(msg), 0);
            close(fd);
            continue;
        }
        /* handshake + idle timeouts so a stuck peer can't pin the single slot */
        struct timeval tv = { .tv_sec = SSH_HANDSHAKE_SECS, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        s_busy = true;
        ssh_session(fd);
        close(fd);
        s_busy = false;
    }
}

/* ---- wolfSSL/wolfSSH allocator: a dedicated PSRAM arena ----
 *
 * Every wolfSSL/wolfSSH dynamic allocation (crypto temporaries, the session I/O buffers, and
 * the per-channel window buffer) is routed here via wolfSSL_SetAllocators and drawn from a
 * single contiguous PSRAM arena reserved once at boot - NOT the shared heap. This is
 * deliberate: the handshake churns many allocations, and doing that on the general PSRAM heap
 * fragmented it enough that the channel-window buffer could no longer find a contiguous block
 * right after auth ("resource shortage: Not enough resources"). A private arena confines that
 * churn so it can never fragment memory the rest of the firmware needs, and keeps the channel
 * buffer reliably allocatable. If the arena is exhausted (or unavailable at boot) we fall back
 * to the general PSRAM heap, then internal RAM, so crypto still works - just without the
 * fragmentation guarantee.
 *
 * Safe in PSRAM because the S3 AES/SHA run in register (non-DMA) mode (esp32_aes.c sets
 * AES_DMA_ENABLE_REG = 0), so these buffers never need to be DMA-capable.
 *
 * IMPORTANT: this only takes effect because wolfssl/wolfssh are built with WOLFSSL_TRACK_MEMORY
 * (see main/CMakeLists.txt); on FreeRTOS wolfSSL's XMALLOC otherwise bypasses the callbacks.
 *
 * multi_heap is not internally locked and crypto runs from both the init task (setup) and the
 * ssh task (per session), so a mutex guards the arena ops; the fallback heap_caps calls are
 * already thread-safe. */
#define WH_WOLF_ARENA_SZ (128 * 1024)   /* peak session use ~50 KB; headroom + room to coalesce */
static multi_heap_handle_t s_wolf_heap;
static uint8_t            *s_wolf_arena;
static SemaphoreHandle_t   s_wolf_lock;

static void wolf_arena_init(void)
{
    s_wolf_lock  = xSemaphoreCreateMutex();
    s_wolf_arena = heap_caps_malloc(WH_WOLF_ARENA_SZ, MALLOC_CAP_SPIRAM);
    if (s_wolf_arena && s_wolf_lock)
        s_wolf_heap = multi_heap_register(s_wolf_arena, WH_WOLF_ARENA_SZ);
    if (s_wolf_heap)
        ESP_LOGI(TAG, "wolfSSL arena: %u KiB in PSRAM", (unsigned)(WH_WOLF_ARENA_SZ / 1024));
    else {
        ESP_LOGW(TAG, "wolfSSL arena unavailable - using general heap");
        if (s_wolf_arena) { heap_caps_free(s_wolf_arena); s_wolf_arena = NULL; }
    }
}

static inline bool wolf_in_arena(const void *p)
{
    return s_wolf_arena && (const uint8_t *)p >= s_wolf_arena
                        && (const uint8_t *)p <  s_wolf_arena + WH_WOLF_ARENA_SZ;
}

static void *wh_wolf_malloc(size_t n)
{
    void *p = NULL;
    if (s_wolf_heap) {
        xSemaphoreTake(s_wolf_lock, portMAX_DELAY);
        p = multi_heap_malloc(s_wolf_heap, n);
        xSemaphoreGive(s_wolf_lock);
    }
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);                     /* arena full: general PSRAM */
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); /* last resort: internal */
    if (!p) {
        /* Straight through the USB-JTAG driver so it reaches the monitor (esp_rom_printf won't
         * once the driver owns the port). Names the arena + both heaps so we can see what ran out. */
        char m[176];
        int ln = snprintf(m, sizeof m,
                 "\r\n[!] wolf_malloc NULL n=%u  arena_free=%u  psram=%u/big=%u  int=%u/big=%u\r\n",
                 (unsigned)n, (unsigned)(s_wolf_heap ? multi_heap_free_size(s_wolf_heap) : 0),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (ln > 0) usb_serial_jtag_write_bytes((const uint8_t *)m, (size_t)ln, pdMS_TO_TICKS(50));
    }
    return p;
}

static void wh_wolf_free(void *p)
{
    if (!p) return;
    if (wolf_in_arena(p)) {
        xSemaphoreTake(s_wolf_lock, portMAX_DELAY);
        multi_heap_free(s_wolf_heap, p);
        xSemaphoreGive(s_wolf_lock);
    } else {
        heap_caps_free(p);
    }
}

static void *wh_wolf_realloc(void *p, size_t n)
{
    if (!p)     return wh_wolf_malloc(n);
    if (n == 0) { wh_wolf_free(p); return NULL; }
    /* Always-correct realloc: sizeof(old) -> alloc new (arena-first) -> copy -> free old. Handles
     * a block moving between the arena and the fallback heap; peak arena use is well under its
     * size so in practice the new block stays in the arena. */
    size_t oldsz;
    if (wolf_in_arena(p)) {
        xSemaphoreTake(s_wolf_lock, portMAX_DELAY);
        oldsz = multi_heap_get_allocated_size(s_wolf_heap, p);
        xSemaphoreGive(s_wolf_lock);
    } else {
        oldsz = heap_caps_get_allocated_size(p);
    }
    void *nw = wh_wolf_malloc(n);
    if (nw) {
        memcpy(nw, p, oldsz < n ? oldsz : n);
        wh_wolf_free(p);
    }
    return nw;   /* NULL -> original left intact (realloc contract) */
}

void wh_ssh_start(void)
{
    /* Reserve the crypto arena, then redirect wolfSSL's heap onto it before anything
     * allocates (wolfSSH_Init pulls in wolfCrypt). */
    wolf_arena_init();
    wolfSSL_SetAllocators(wh_wolf_malloc, wh_wolf_free, wh_wolf_realloc);
    if (wolfSSH_Init() != WS_SUCCESS) { ESP_LOGE(TAG, "wolfSSH_Init failed"); return; }
    s_ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (!s_ctx) { ESP_LOGE(TAG, "CTX_new failed"); return; }

    /* Shrink the per-channel flow-control window from wolfSSH's 128 KB default to 16 KB.
     * ChannelNew() allocates a buffer of exactly windowSz for each channel's receive window;
     * at 128 KB that allocation was failing right after auth (WS_RESOURCE_E -> the client saw
     * "channel open failed: resource shortage"). 16 KB is far more than an interactive shell
     * needs and allocates comfortably. maxPacketSz is set equal so a full packet always fits
     * the window. Both buffers land in PSRAM via the allocator above. */
    wolfSSH_CTX_SetWindowPacketSize(s_ctx, WH_SSH_WINDOW_SZ, WH_SSH_MAXPACKET_SZ);

    if (!hostkey_select()) {                          /* fail closed: never a shared key */
        ESP_LOGE(TAG, "SSH not started (no per-device host key)");
        wolfSSH_CTX_free(s_ctx);
        s_ctx = NULL;
        return;
    }
    if (wolfSSH_CTX_UsePrivateKey_buffer(s_ctx, s_hostkey, s_hostkey_len,
                                         WOLFSSH_FORMAT_ASN1) != WS_SUCCESS) {
        ESP_LOGE(TAG, "host key load failed (%s)", s_hostkey_src);
        return;
    }
    ESP_LOGI(TAG, "host key: %s (%u bytes)", s_hostkey_src, (unsigned)s_hostkey_len);
    wolfSSH_SetUserAuth(s_ctx, ssh_userauth);
    authkeys_load();
    s_enabled = true;

    /* Crypto path -> keep the stack internal and generous. */
    /* stack 7168: real ed25519 handshake + active session peaked at 4124 (mem); command
     * dispatch over the channel is shallower than the handshake, so ~3K margin covers it */
    if (wh_task_start_("ssh", ssh_task, 7168, 5, 0) != ESP_OK)
        ESP_LOGE(TAG, "failed to start ssh task");
}

void wh_ssh_cmd(wh_out_fn out, void *ctx, char **av, int n)
{
    /* `ssh newkey` — regenerate the per-device host key (applies on reboot). */
    if (n >= 2 && !strcmp(av[1], "newkey")) {
        unsigned char der[SSH_HOSTKEY_MAX]; word32 dl = 0;
        if (hostkey_generate(der, &dl) && hostkey_store_nvs(der, dl))
            out(ctx, "new host key generated and stored in NVS - reboot to apply\n");
        else
            out(ctx, "host key generation failed\n");
        return;
    }
    /* `ssh keys` - list authorized keys with an index. */
    if (n >= 2 && !strcmp(av[1], "keys")) { ssh_cmd_keys(out, ctx); return; }
    /* `ssh rmkey <n>` - remove one authorized key. */
    if (n >= 2 && !strcmp(av[1], "rmkey")) {
        if (n < 3) { out(ctx, "usage: ssh rmkey <n>   (see 'ssh keys')\n"); return; }
        int idx = atoi(av[2]);
        if (idx < 0) { out(ctx, "rmkey: bad index\n"); return; }
        ssh_cmd_rmkey(out, ctx, idx);
        return;
    }
    /* `ssh addkey [sd] <type> <base64>` - authorize a client key.
     *   ssh addkey <type> <base64>       -> NVS (card-less)
     *   ssh addkey sd <type> <base64>    -> SD /sdcard/.ssh/authorized_keys (primary store) */
    if (n >= 2 && !strcmp(av[1], "addkey")) {
        if (n >= 3 && !strcmp(av[2], "sd")) {
            if (n < 5) { out(ctx, "usage: ssh addkey sd <type> <base64>\n"); return; }
            if (authkeys_sd_append(av[3], av[4])) {
                char b[80];
                snprintf(b, sizeof b, "key written to SD authorized_keys - %d key(s) now\n", s_nkeys);
                out(ctx, b);
            } else {
                out(ctx, "failed to write SD key (no card mounted, bad base64, or write error)\n");
            }
            return;
        }
        if (n < 4) {
            out(ctx, "usage: ssh addkey <type> <base64>       (NVS, card-less)\n"
                     "       ssh addkey sd <type> <base64>    (SD authorized_keys, primary)\n");
            return;
        }
        if (authkeys_nvs_append(av[2], av[3])) {
            char b[64];
            snprintf(b, sizeof b, "key added to NVS - %d authorized key(s) now\n", s_nkeys);
            out(ctx, b);
        } else {
            out(ctx, "failed to add key (bad base64 or NVS error)\n");
        }
        return;
    }
    /* bare `ssh` (or `ssh status`) — status */
    char b[112];
    snprintf(b, sizeof b, "ssh    : %s on :%d  |  %s  |  %d authorized key(s)\n",
             s_enabled ? "enabled" : "disabled", SSH_PORT,
             s_busy ? "session active" : "idle", s_nkeys);
    out(ctx, b);
    snprintf(b, sizeof b, "         host key: %s\n", s_hostkey_src);
    out(ctx, b);
    out(ctx, "         keys: ssh keys (list) | ssh addkey [sd] <t> <b64> | ssh rmkey <n>\n");
    out(ctx, "         host: ssh newkey (regenerate per-device host key)\n");
}

#else /* !WH_HAVE_WOLFSSH — host gate / builds without the wolfSSL+wolfSSH components */

void wh_ssh_start(void) {}
void wh_ssh_cmd(wh_out_fn out, void *ctx, char **av, int n)
{
    (void)av; (void)n;
    out(ctx, "ssh    : not built (no wolfSSH)\n");
}


#endif
