/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/* wh-link L1 glue: the WH_LINK_IMPLEMENTATION TU + a NimBLE
 * peripheral wearing the ring's GATT contract VERBATIM
 * (RING-LINK-RESPONSE.md):
 *   service 6e574800-a9c4-4b7e-9d21-5748c0de0001
 *   RX      6e574801-... (WRITE | WRITE_NO_RSP)  central -> us
 *   TX      6e574802-... (NOTIFY)                us -> central
 * Service UUID rides the ADV (passive-scan visible); name rides the
 * scan response. MTU preferred 517; the central initiates exchange.
 * Discoverable ONLY inside an explicit pairing window (`ble pair`),
 * per the RENDEZVOUS privacy doctrine; advertising also yields to
 * an active OTA pull. */
#define WH_LINK_IMPLEMENTATION
#include "wh_link.h"

#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <sys/time.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_attr.h"
#include "whlink.h"
#include "ui.h"
#include "sync.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#ifndef WHM_VERSION_STR
#define WHM_VERSION_STR "dev"
#endif

static const char *TAG = "whlink";

/* RECONCILIATION-PROPOSED, NOT YET IN THE SHARED HEADER (which
 * stays verbatim): WH_MSG_WHMCAST = one verbatim whmcast (WHML)
 * datagram per message. Filed with the watch agent per wh-link §6;
 * 0x68 is next-free after NETSCAN_RESULT. Old peers NAK unknown
 * types - graceful by spec design. */
#define WHM_MSG_WHMCAST_PROPOSED       0x68  /* CANONICAL r1 -
    applied upstream (RECONCILE-r1-panel), values confirmed
    byte-identical incl golden vectors; these local defines retire
    for header symbols the moment the redistributed wh_link.h file
    arrives (verbatim doctrine: we do not edit the vendored copy
    ourselves) */
#define WHM_MSG_PANEL_STATUS_PROPOSED  0x69  /* the 19-B brief,
    MOVED OFF 0x62: that type is the shared wh_device_status (34 B)
    and our brief misparses under wh_dec_status. The watch caught
    it; my recon had the struct on screen and missed the collision -
    doctrine 17, personally. ANNOUNCE covers identity; this carries
    only the fleet brief. */

static bool s_feed = false;      /* ble feed on|off */
static bool s_wl_up = false;     /* init completed */
static uint16_t s_wl_id = 0;

/* LSB-first 128-bit UUIDs; only byte[12] differs (00/01/02). */
static const ble_uuid128_t k_svc_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0xde, 0xc0, 0x48, 0x57, 0x21, 0x9d,
    0x7e, 0x4b, 0xc4, 0xa9, 0x00, 0x48, 0x57, 0x6e);
static const ble_uuid128_t k_rx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0xde, 0xc0, 0x48, 0x57, 0x21, 0x9d,
    0x7e, 0x4b, 0xc4, 0xa9, 0x01, 0x48, 0x57, 0x6e);
static const ble_uuid128_t k_tx_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0xde, 0xc0, 0x48, 0x57, 0x21, 0x9d,
    0x7e, 0x4b, 0xc4, 0xa9, 0x02, 0x48, 0x57, 0x6e);

EXT_RAM_BSS_ATTR static wh_ctx s_ctx;   /* ~4.4 KB (reasm 4096) -
                                           PSRAM; no ISR touch */
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static bool     s_notify_on = false;
static bool     s_paired = false;
static bool     s_synced = false;
static int64_t  s_adv_until = 0;
static uint8_t  s_addr_type;

/* ---- L4: the bond (NVS) + rotating pseudonym ----
 * The bond IS the exported session blob (keys, prefixes, counter);
 * the shared header's persistence layer does the hard parts: import
 * bumps tx_ctr by WH_PERSIST_MARGIN so a pre-restart nonce can
 * never be reused, and we re-save immediately after restore plus
 * every WH_PERSIST_STRIDE sends, exactly as its contract demands.
 * Both ends run byte-identical logic - resume symmetry with the
 * watch is a property of the header, not a negotiation. */
typedef struct {
    uint8_t  ver;                    /* 1 */
    uint8_t  present;
    uint16_t peer_id;
    char     nick[16];
    uint8_t  blob[WH_SESSION_BLOB_LEN];
} wl_bond_t;
static wl_bond_t s_bond;
static uint32_t  s_since_save;
static int64_t   s_pseud_epoch = -1;
static bool      s_pseud_adv = false;

static void bond_save_from_ctx(void)
{
    if (!wh_is_established(&s_ctx)) return;
    if (wh_session_export(&s_ctx, s_bond.blob,
                          sizeof(s_bond.blob)) <= 0) return;
    s_bond.ver = 1;
    s_bond.present = 1;
    s_bond.peer_id = s_ctx.cfg.peer_id;
    nvs_handle_t h;
    if (nvs_open("whlink", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bond0", &s_bond, sizeof(s_bond));
        nvs_commit(h);
        nvs_close(h);
        s_since_save = 0;
    }
}

static void bond_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof(s_bond);
    if (nvs_open("whlink", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "bond0", &s_bond, &n) != ESP_OK ||
            s_bond.ver != 1)
            memset(&s_bond, 0, sizeof(s_bond));
        nvs_close(h);
    }
    if (s_bond.present)
        printf("whlink: bond loaded (peer %04x '%s') - silent "
               "reconnect armed\n", s_bond.peer_id, s_bond.nick);
}

static void bond_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("whlink", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "bond0");
        nvs_commit(h);
        nvs_close(h);
    }
    memset(&s_bond, 0, sizeof(s_bond));
    printf("whlink: bond forgotten\n");
}

/* pseudonym token: HMAC(K_pseud, LE64(unix/60))[0..5].
 * K_pseud = HKDF(salt="wh-pseud-v1", ikm = tx_key XOR rx_key) -
 * the XOR makes the ikm identical on both sides (my tx is their
 * rx). Proposed for reconciliation in SPEC-panel-link. */
static void pseud_token(uint8_t out[6], int64_t epoch)
{
    uint8_t ikm[WH_KEY_LEN], kp[32], mac[32], msg[8];
    for (int i = 0; i < WH_KEY_LEN; i++)
        ikm[i] = s_ctx.sess.tx_key[i] ^ s_ctx.sess.rx_key[i];
    whc_hkdf((const uint8_t *)"wh-pseud-v1", 11, ikm, sizeof(ikm),
             NULL, 0, kp, sizeof(kp));
    for (int i = 0; i < 8; i++)
        msg[i] = (uint8_t)((uint64_t)epoch >> (8 * i));
    whc_hmac_sha256(kp, sizeof(kp), msg, sizeof(msg), mac);
    memcpy(out, mac, 6);
}

static void adv_maybe(void);
static void wl_status_send(void);

/* ---- L2: console bridge (CONSOLE 0x2F -> esp_console_run) ----
 * One command per BLE line; the panels are stdout for a display
 * fleet, so CONSOLE_OUT carries the disciplined ack and STATUS
 * carries the state a wrist renders. Sealed-only: an unsealed
 * CONSOLE is dropped with a log line. Runs in its own small task
 * (internal stack - console commands touch flash). */
typedef struct { char line[200]; uint16_t corr; } wl_cmd_t;
static QueueHandle_t s_cmd_q;

static void wl_exec_task(void *arg)
{
    (void)arg;
    wl_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &c, portMAX_DELAY) != pdTRUE)
            continue;
        printf("whlink: console <- '%s'\n", c.line);
        int ret = 0;
        esp_err_t e = esp_console_run(c.line, &ret);
        char ack[24];
        if (e == ESP_ERR_NOT_FOUND)
            strlcpy(ack, "unknown", sizeof(ack));
        else if (e != ESP_OK)
            snprintf(ack, sizeof(ack), "err %d", (int)e);
        else if (ret != 0)
            snprintf(ack, sizeof(ack), "rc %d", ret);
        else
            strlcpy(ack, "ok", sizeof(ack));
        wh_send(&s_ctx, WH_MSG_CONSOLE_OUT, WH_FLAG_RESP, c.corr,
                (const uint8_t *)ack, strlen(ack));
        wl_status_send();          /* state likely changed - tell
                                      the wrist right away */
    }
}

/* ---- wh-link platform callbacks ---- */
static void cb_rng(void *user, uint8_t *buf, size_t len)
{
    (void)user;
    esp_fill_random(buf, len);
}

static uint32_t cb_now_ms(void *user)
{
    (void)user;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int cb_send(void *user, const uint8_t *frame, size_t len)
{
    (void)user;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify_on) return -1;
    if (wh_is_established(&s_ctx) &&
        ++s_since_save >= WH_PERSIST_STRIDE)
        bond_save_from_ctx();      /* stride re-save < margin */
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, len);
    if (!om) return -1;
    return ble_gatts_notify_custom(s_conn, s_tx_handle, om) == 0
               ? 0 : -1;
}

static void cb_feed_tap(const uint8_t *buf, int len)
{
    if (!s_feed || !s_notify_on || !wh_is_established(&s_ctx))
        return;
    wh_send(&s_ctx, WHM_MSG_WHMCAST_PROPOSED, WH_FLAG_EVENT, 0,
            buf, (size_t)len);
}

static void cb_on_message(void *user, const wh_msg *m)
{
    (void)user;
    switch (m->type) {
    case WHM_MSG_WHMCAST_PROPOSED: {
        if (!wh_is_established(&s_ctx)) {
            printf("whlink: WHMCAST before pairing - dropped\n");
            break;
        }
        if (m->len < 8 || memcmp(m->payload, "WHML", 4) != 0)
            break;                 /* not family - ignore */
        whm_sync_inject(m->payload, (int)m->len, true);
        break;
    }
    case WH_MSG_ANNOUNCE: {
        wh_announce a;
        if (wh_dec_announce(m->payload, m->len, &a) == WH_OK) {
            ESP_LOGI(TAG, "peer: '%s' role=%u caps=0x%lx id=%u",
                     a.nick, a.role, (unsigned long)a.caps,
                     (unsigned)a.short_id);
            wh_ctx_set_peer(&s_ctx, a.short_id);
        }
        break;
    }
    case WH_MSG_CONSOLE: {
        if (!wh_is_established(&s_ctx)) {
            printf("whlink: CONSOLE before pairing - dropped\n");
            break;
        }
        wl_cmd_t c;
        size_t n = m->len < sizeof(c.line) - 1 ? m->len
                                               : sizeof(c.line) - 1;
        memcpy(c.line, m->payload, n);
        c.line[n] = 0;
        c.corr = m->corr;
        if (!s_cmd_q || xQueueSend(s_cmd_q, &c, 0) != pdTRUE) {
            const char *b = "busy";
            wh_send(&s_ctx, WH_MSG_CONSOLE_OUT, WH_FLAG_RESP,
                    m->corr, (const uint8_t *)b, 4);
        }
        break;
    }
    case WH_MSG_PING:
        wh_send(&s_ctx, WH_MSG_PONG, WH_FLAG_RESP, m->corr,
                m->payload, m->len);
        break;
    default:
        ESP_LOGI(TAG, "msg type 0x%02x len %u%s", m->type,
                 (unsigned)m->len,
                 wh_is_established(&s_ctx) ? " (sealed)" : "");
        break;
    }
}

static void cb_on_sas(void *user, const char *digits)
{
    (void)user;
    printf("whlink: SAS %s - confirm on panel (tap=yes hold=no)\n",
           digits);
    whm_ui_sas_show(digits);
}

/* ---- L3: STATUS (0x62) - small LE body, EVENT-flagged ----
 * [0]=ver 1, [1]=flags (b0 lead, b1 conductor), [2]=ui-role,
 * [3]=fresh, [4]=stale, [5..12]=fw (NUL-padded), [13..16]=uptime s
 * LE, [17..18]=free internal heap KB LE. Emitted on pairing, after
 * every bridged command, and on a 10 s heartbeat. */
static void wl_status_send(void)
{
    if (!wh_is_established(&s_ctx) || !s_notify_on) return;
    int role = 0, fresh = 0, stale = 0;
    whm_sync_brief(&role, &fresh, &stale, NULL);
    uint8_t b[19] = { 0 };
    b[0] = 1;
    b[1] = (uint8_t)((role >= 1 ? 1 : 0) | (role == 2 ? 2 : 0));
    b[2] = (uint8_t)role;
    b[3] = (uint8_t)(fresh > 255 ? 255 : fresh);
    b[4] = (uint8_t)(stale > 255 ? 255 : stale);
    strncpy((char *)&b[5], WHM_VERSION_STR, 8);
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    b[13] = up & 0xFF; b[14] = (up >> 8) & 0xFF;
    b[15] = (up >> 16) & 0xFF; b[16] = (up >> 24) & 0xFF;
    uint16_t kb = (uint16_t)(esp_get_free_heap_size() / 1024);
    b[17] = kb & 0xFF; b[18] = (kb >> 8) & 0xFF;
    wh_send(&s_ctx, WHM_MSG_PANEL_STATUS_PROPOSED, WH_FLAG_EVENT,
            0, b, sizeof(b));
}

static void wl_fuse_clear(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    if (nvs_open("whlink", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "fuse", 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void wl_status_tick(void *arg)
{
    (void)arg;
    wl_status_send();
}

static void cb_on_paired(void *user, int ok)
{
    (void)user;
    s_paired = (ok != 0);
    printf("whlink: %s\n", ok ? "PAIRED - link sealed"
                              : "pairing rejected/failed");
    whm_ui_sas_done(ok != 0);
    if (ok) {
        strlcpy(s_bond.nick, "watch", sizeof(s_bond.nick));
        bond_save_from_ctx();      /* the pairing IS the bond */
        printf("whlink: bond saved - future reconnects are "
               "silent\n");
        wl_status_send();          /* first brief, immediately */
    }
}

/* ---- GATT ---- */
static int rx_access_cb(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[260];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf),
                                &len) == 0) {
            wh_on_frame(&s_ctx, buf, len);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int tx_access_cb(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* notify-only characteristic; never-called read stub kept
       non-NULL on purpose (watch review, cosmetic note 3) */
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return 0;
}

static const struct ble_gatt_svc_def k_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &k_rx_uuid.u,
                .access_cb = rx_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &k_tx_uuid.u,
                .access_cb = tx_access_cb,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP ---- */
static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            if (s_bond.present) {
                /* silent reconnect: restore the sealed session and
                   RE-SAVE IMMEDIATELY (the header's contract - the
                   on-flash counter must advance every restore). A
                   fresh PAIR_REQ instead simply replaces the bond
                   on success; a stranger's sealed frames fail AEAD
                   and drop. */
                if (wh_session_import(&s_ctx, s_bond.blob,
                                      WH_SESSION_BLOB_LEN)
                        == WH_OK) {
                    wh_ctx_set_peer(&s_ctx, s_bond.peer_id);
                    s_paired = true;
                    bond_save_from_ctx();
                    if (s_feed) whm_ui_walk_params_poke();
                    printf("whlink: central connected - sealed "
                           "session RESUMED (bond)\n");
                } else {
                    printf("whlink: central connected (bond "
                           "restore failed - pair again)\n");
                }
            } else {
                printf("whlink: central connected\n");
            }
        } else {
            adv_maybe();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("whlink: disconnected (reason %d)\n",
               ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify_on = false;
        s_paired = false;
        wh_ctx_init(&s_ctx, &s_ctx.cfg);   /* fresh session state */
        adv_maybe();                       /* re-adv inside window */
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            s_notify_on = ev->subscribe.cur_notify;
            if (s_notify_on) {
                printf("whlink: pipe up (central subscribed) - "
                       "announcing\n");
                wh_announce_send(&s_ctx, 100, 0);
            }
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        printf("whlink: MTU %u\n", ev->mtu.value);
        return 0;
    default:
        return 0;
    }
}

static void adv_maybe(void)
{
    if (!s_synced) return;
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (whm_ui_ota_active()) return;       /* OTA owns the radio */
    bool window = esp_timer_get_time() < s_adv_until;
    if (!window && s_bond.present) {
        /* bonded, outside the window: ROTATING PSEUDONYM - no
           service UUID, no name, just a resolvable 6-byte token in
           manufacturer data. Only the bonded watch can recognize
           us; to everyone else we are BLE noise. */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t epoch = tv.tv_sec / 60;
        if (ble_gap_adv_active()) {
            if (epoch == s_pseud_epoch && s_pseud_adv) return;
            ble_gap_adv_stop();
        }
        uint8_t mfg[10] = { 0xFF, 0xFF, 'W', 'P' };
        /* restore keys into a scratch session if not live */
        if (!wh_is_established(&s_ctx))
            wh_session_import(&s_ctx, s_bond.blob,
                                      WH_SESSION_BLOB_LEN);
        pseud_token(&mfg[4], epoch);
        struct ble_hs_adv_fields adv = { 0 };
        adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        adv.mfg_data = mfg;
        adv.mfg_data_len = sizeof(mfg);
        ble_gap_adv_set_fields(&adv);
        struct ble_gap_adv_params p = { 0 };
        p.conn_mode = BLE_GAP_CONN_MODE_UND;
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        p.itvl_min = 1280; p.itvl_max = 1760;   /* 800-1100 ms */
        ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p,
                          gap_event, NULL);
        s_pseud_epoch = epoch;
        s_pseud_adv = true;
        return;
    }
    if (!window) return;
    if (ble_gap_adv_active()) {
        if (!s_pseud_adv) return;
        ble_gap_adv_stop();        /* window opens: full identity */
    }
    s_pseud_adv = false;

    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&k_svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&adv);

    struct ble_hs_adv_fields rsp = { 0 };
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p,
                      gap_event, NULL);
    printf("whlink: advertising (window %ds left)\n",
           (int)((s_adv_until - esp_timer_get_time()) / 1000000));
}

static void on_hs_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    s_synced = true;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---- public ---- */
void whm_whlink_status_line(char *out, size_t cap)
{
    if (!s_wl_up) {
        strlcpy(out, "down (fuse/disable or preflight)", cap);
        return;
    }
    char bnd[32] = "no bond";
    if (s_bond.present)
        snprintf(bnd, sizeof(bnd), "bonded '%s'", s_bond.nick);
    snprintf(out, cap, "up id %04x | %s | %s | feed %s",
             s_wl_id, bnd,
             s_conn != BLE_HS_CONN_HANDLE_NONE ? "SEALED" : "idle",
             s_feed ? "on" : "off");
}

void whm_whlink_init(void)
{
    /* BOOT-LOOP FUSE + PRE-FLIGHT (bench conviction: the BT
       controller does not RETURN on malloc failure - it asserts
       (emi.c 164) into an IWDT panic loop. Prevention only.) */
    {
        nvs_handle_t h;
        uint8_t fuse = 0, dis = 0;
        if (nvs_open("whlink", NVS_READWRITE, &h) == ESP_OK) {
            nvs_get_u8(h, "disable", &dis);
            if (dis) {
                nvs_close(h);
                printf("whlink: DISABLED ('ble enable' to turn "
                       "back on)\n");
                return;
            }
            nvs_get_u8(h, "fuse", &fuse);
            if (fuse >= 3) {
                nvs_close(h);
                printf("whlink: DISABLED by boot-fuse (%u rapid "
                       "boots) - fix memory, then 'ble enable'\n",
                       fuse);
                return;
            }
            nvs_set_u8(h, "fuse", (uint8_t)(fuse + 1));
            nvs_commit(h);
            nvs_close(h);
        }
        size_t big = heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (big < 50 * 1024) {
            printf("whlink: SKIPPED - largest internal block %u KB "
                   "< 50 KB (controller would assert, not "
                   "return)\n", (unsigned)(big / 1024));
            return;
        }
    }
    if (nimble_port_init() != ESP_OK) {
        printf("whlink: nimble init FAILED - BLE unavailable\n");
        return;
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(k_gatt_svcs);
    ble_gatts_add_svcs(k_gatt_svcs);
    ble_svc_gap_device_name_set(whm_sync_node_name());
    ble_att_set_preferred_mtu(517);
    ble_hs_cfg.sync_cb = on_hs_sync;

    wh_config cfg = { 0 };
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    cfg.local_id = (uint16_t)((mac[4] << 8) | mac[5]);
    cfg.peer_id = WH_ID_BROADCAST;
    cfg.role = 4;                          /* WH_ROLE_PANEL -
                                              canonical r1 (value
                                              confirmed; symbol
                                              lands with the header
                                              redistribution) */
    cfg.caps = WH_CAP_WIFI | WH_CAP_BLE | WH_CAP_SD | WH_CAP_RTC |
               WH_CAP_AUDIO | WH_CAP_DISPLAY | WH_CAP_IMU;
    strlcpy(cfg.nick, whm_sync_node_name(), sizeof(cfg.nick));
    cfg.rng = cb_rng;
    cfg.now_ms = cb_now_ms;
    cfg.transport_send = cb_send;
    cfg.on_message = cb_on_message;
    cfg.on_sas = cb_on_sas;
    cfg.on_paired = cb_on_paired;
    wh_ctx_init(&s_ctx, &cfg);
    whm_sync_set_tap(cb_feed_tap);

    bond_load();
    s_cmd_q = xQueueCreate(4, sizeof(wl_cmd_t));
    xTaskCreate(wl_exec_task, "wl_exec", 4096, NULL, 4, NULL);
    esp_timer_create_args_t ta = { .callback = wl_status_tick,
                                   .name = "wl_status" };
    esp_timer_handle_t th;
    if (esp_timer_create(&ta, &th) == ESP_OK)
        esp_timer_start_periodic(th, 10 * 1000000);
    nimble_port_freertos_init(host_task);
    s_wl_up = true;
    s_wl_id = cfg.local_id;
    printf("whlink: up (id %04x, nick '%s') - 'ble pair' opens the "
           "window\n", cfg.local_id, cfg.nick);
    {   /* healthy for 30 s clears the boot fuse */
        esp_timer_create_args_t fa = { .callback = wl_fuse_clear,
                                       .name = "wl_fuse" };
        esp_timer_handle_t fh;
        if (esp_timer_create(&fa, &fh) == ESP_OK)
            esp_timer_start_once(fh, 15 * 1000000);   /* 15 s: bench
                                        reflash cycles tripped 30 */
    }
}

void whm_whlink_pair_window(uint32_t secs)
{
    s_adv_until = esp_timer_get_time() + (int64_t)secs * 1000000;
    adv_maybe();
    printf("whlink: pairing window open %lus\n",
           (unsigned long)secs);
}

void whm_whlink_feed(bool on)
{
    s_feed = on;
    if (on) whm_ui_walk_params_poke();   /* new subscriber: send
                                            the world seed now */
    printf("whlink: feed %s\n", on ? "ON - tunneling fleet types "
           "2/7/9 to the sealed peer" : "off");
}

void whm_whlink_forget(void)
{
    bond_forget();
    whm_whlink_off();
}

void whm_whlink_off(void)
{
    s_adv_until = 0;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    printf("whlink: window closed\n");
}

void whm_whlink_status_print(void)
{
    printf("whlink: %s%s%s | window %s | mtu pref 517\n",
           s_conn != BLE_HS_CONN_HANDLE_NONE ? "connected"
                                             : "idle",
           s_notify_on ? " +subscribed" : "",
           s_paired ? " +SEALED" : "",
           esp_timer_get_time() < s_adv_until ? "OPEN" : "closed");
    printf("whlink: feed %s (types 2/7/9 -> WHMCAST 0x68 "
           "proposed)\n", s_feed ? "ON" : "off");
    if (s_bond.present)
        printf("whlink: bonded to %04x '%s' (pseudonym adv %s)\n",
               s_bond.peer_id, s_bond.nick,
               s_pseud_adv ? "on" : "off");
}

void whm_whlink_sas_result(bool match)
{
    wh_pair_confirm(&s_ctx, match ? 1 : 0);
}
