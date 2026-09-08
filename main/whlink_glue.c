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
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "whlink.h"
#include "ui.h"
#include "sync.h"

static const char *TAG = "whlink";

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

static wh_ctx   s_ctx;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static bool     s_notify_on = false;
static bool     s_paired = false;
static bool     s_synced = false;
static int64_t  s_adv_until = 0;
static uint8_t  s_addr_type;

static void adv_maybe(void);

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
    struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, len);
    if (!om) return -1;
    return ble_gatts_notify_custom(s_conn, s_tx_handle, om) == 0
               ? 0 : -1;
}

static void cb_on_message(void *user, const wh_msg *m)
{
    (void)user;
    switch (m->type) {
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

static void cb_on_paired(void *user, int ok)
{
    (void)user;
    s_paired = (ok != 0);
    printf("whlink: %s\n", ok ? "PAIRED - link sealed"
                              : "pairing rejected/failed");
    whm_ui_sas_done(ok != 0);
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
            printf("whlink: central connected\n");
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
    if (esp_timer_get_time() >= s_adv_until) return;
    if (whm_ui_ota_active()) return;       /* OTA owns the radio */
    if (ble_gap_adv_active()) return;

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
void whm_whlink_init(void)
{
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
    cfg.role = WH_ROLE_GENERIC;            /* WH_ROLE_PANEL pending
                                              reconciliation */
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

    nimble_port_freertos_init(host_task);
    printf("whlink: up (id %04x, nick '%s') - 'ble pair' opens the "
           "window\n", cfg.local_id, cfg.nick);
}

void whm_whlink_pair_window(uint32_t secs)
{
    s_adv_until = esp_timer_get_time() + (int64_t)secs * 1000000;
    adv_maybe();
    printf("whlink: pairing window open %lus\n",
           (unsigned long)secs);
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
}

void whm_whlink_sas_result(bool match)
{
    wh_pair_confirm(&s_ctx, match ? 1 : 0);
}
