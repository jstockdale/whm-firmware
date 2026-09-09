/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/*
 * wifi_tsf.c - WiFi manager + TSF clock observation.
 *
 * Credential precedence:
 *   1. NVS (set at runtime via the console: `wifi join` / `wifi ap`)
 *   2. Kconfig compile-time defaults (WHM_WIFI_SSID, ...)
 *   3. SoftAP fallback (WHM_SOFTAP_SSID)
 *
 * Runtime switching is first-class: `wifi join`/`wifi ap` reconfigure the
 * running stack without a reboot. Both default netifs (STA + AP) are created
 * once at start; mode changes only flip esp_wifi mode/config.
 *
 * Power save stays OFF always (TSF accuracy + streaming latency).
 *
 * TSF observer: samples esp_wifi_get_tsf_time() every 2s and computes the
 * drift vs the local crystal in ppm. In STA mode this is the AP-vs-us clock
 * offset that 802.11 corrects continuously - the number the whole sync
 * design rests on. Latest snapshot is queryable from the console (`tsf`).
 */
#include "wifi_tsf.h"
#include "settings.h"
#include "timesync.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "sdkconfig.h"

static const char *TAG = "whm_wifi";

typedef enum { WHM_MODE_SOFTAP = 0, WHM_MODE_STA = 1 } whm_wifi_mode_t;

static whm_wifi_mode_t s_desired = WHM_MODE_SOFTAP;
static volatile bool s_connected = false;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;
static char s_cur_ssid[33] = "";

/* latest TSF drift snapshot for the console */
static volatile int64_t s_last_tsf = 0;
static volatile double s_last_ppm = 0.0;
static volatile bool s_ppm_valid = false;

static wifi_interface_t tsf_if(void)
{
    return (s_desired == WHM_MODE_STA) ? WIFI_IF_STA : WIFI_IF_AP;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_desired == WHM_MODE_STA) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) whm_timesync_net_down();
        s_connected = false;
        if (s_desired == WHM_MODE_STA) {
            ESP_LOGW(TAG, "STA disconnected, retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        s_connected = true;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        whm_timesync_net_up();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "SoftAP: station joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "SoftAP: station left");
    }
}

static esp_err_t apply_sta(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));

    s_desired = WHM_MODE_STA;
    s_connected = false;
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set_config");
    esp_wifi_disconnect();          /* harmless if not connected */
    esp_wifi_connect();
    ESP_LOGI(TAG, "STA mode, joining \"%s\"", ssid);
    return ESP_OK;
}

static esp_err_t apply_softap(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    /* SSID precedence: explicit arg > node name > Kconfig default */
    char node[17] = "";
    const char *use_ssid;
    if (ssid && ssid[0]) {
        use_ssid = ssid;
    } else if (whm_settings_get_str("node", node, sizeof(node)) == ESP_OK &&
               node[0]) {
        use_ssid = node;
    } else {
        use_ssid = CONFIG_WHM_SOFTAP_SSID;
    }
    const char *use_pass = pass ? pass : CONFIG_WHM_SOFTAP_PASSWORD;

    strlcpy((char *)cfg.ap.ssid, use_ssid, sizeof(cfg.ap.ssid));
    strlcpy((char *)cfg.ap.password, use_pass, sizeof(cfg.ap.password));
    cfg.ap.ssid_len = strlen(use_ssid);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 6;
    cfg.ap.authmode = strlen(use_pass) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_desired = WHM_MODE_SOFTAP;
    if (s_connected) whm_timesync_net_down();
    s_connected = false;
    strlcpy(s_cur_ssid, use_ssid, sizeof(s_cur_ssid));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "set_config");
    ESP_LOGI(TAG, "SoftAP mode, SSID \"%s\" (%s)", use_ssid,
             cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    return ESP_OK;
}

/* ------------------------------------------------------------------ public */

esp_err_t whm_wifi_join(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    whm_settings_set_u8("wifi_mode", WHM_MODE_STA);
    whm_settings_set_str("wifi_ssid", ssid);
    whm_settings_set_str("wifi_pass", pass ? pass : "");
    return apply_sta(ssid, pass);
}

const char *whm_wifi_ap_default_psk(void)
{
    return CONFIG_WHM_SOFTAP_PASSWORD;
}

esp_err_t whm_wifi_softap(const char *ssid, const char *pass)
{
    whm_settings_set_u8("wifi_mode", WHM_MODE_SOFTAP);
    if (ssid && ssid[0]) whm_settings_set_str("wifi_ssid", ssid);
    else whm_settings_erase("wifi_ssid");
    if (pass) whm_settings_set_str("wifi_pass", pass);
    else whm_settings_erase("wifi_pass");
    return apply_softap(ssid, pass);
}

esp_err_t whm_wifi_clear(void)
{
    whm_settings_erase("wifi_mode");
    whm_settings_erase("wifi_ssid");
    whm_settings_erase("wifi_pass");
    ESP_LOGI(TAG, "saved WiFi settings cleared; compile-time defaults apply on next boot");
    return ESP_OK;
}

bool whm_wifi_is_sta(void) { return s_desired == WHM_MODE_STA; }
bool whm_wifi_connected(void) { return s_connected; }

void whm_wifi_status_print(void)
{
    printf("mode:      %s\n", s_desired == WHM_MODE_STA ? "STA" : "SoftAP");
    printf("ssid:      %s\n", s_cur_ssid);
    if (s_desired == WHM_MODE_STA) {
        printf("connected: %s\n", s_connected ? "yes" : "no");
        if (s_connected) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                printf("rssi:      %d dBm  channel: %d\n", ap.rssi, ap.primary);
            }
            esp_netif_ip_info_t ip;
            if (s_netif_sta && esp_netif_get_ip_info(s_netif_sta, &ip) == ESP_OK) {
                printf("ip:        " IPSTR "\n", IP2STR(&ip.ip));
            }
        }
    } else {
        wifi_sta_list_t stas;
        if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK) {
            printf("stations:  %d\n", stas.num);
        }
        esp_netif_ip_info_t ip;
        if (s_netif_ap && esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK) {
            printf("ip:        " IPSTR "\n", IP2STR(&ip.ip));
        }
    }
    int64_t tsf = esp_wifi_get_tsf_time(tsf_if());
    printf("tsf:       %lld us\n", (long long)tsf);
    if (s_ppm_valid) {
        printf("drift:     %+.1f ppm (tsf vs local crystal, 2s window)\n",
               (double)s_last_ppm);
    }
    printf("powersave: OFF (pinned for TSF accuracy)\n");
}

int whm_wifi_scan_print(void)
{
    wifi_mode_t prev;
    esp_wifi_get_mode(&prev);
    bool restore_ap = false;
    if (prev == WIFI_MODE_AP) {
        /* need a STA interface to scan; keep the AP up meanwhile */
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
            printf("scan: could not enable APSTA for scanning\n");
            return -1;
        }
        restore_ap = true;
    }

    printf("scanning...\n");
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(err));
        if (restore_ap) esp_wifi_set_mode(WIFI_MODE_AP);
        return -1;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t *recs = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
    if (recs) {
        esp_wifi_scan_get_ap_records(&n, recs);
        printf("%-32s %5s %4s %s\n", "SSID", "RSSI", "CH", "AUTH");
        for (int i = 0; i < n; i++) {
            printf("%-32s %5d %4d %s\n",
                   (char *)recs[i].ssid, recs[i].rssi, recs[i].primary,
                   recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "sec");
        }
        free(recs);
    }
    if (restore_ap) esp_wifi_set_mode(WIFI_MODE_AP);
    return n;
}

const char *whm_wifi_ssid(void) { return s_cur_ssid; }

int whm_wifi_rssi(void)
{
    if (s_desired != WHM_MODE_STA || !s_connected) return 127;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 127;
    return ap.rssi;
}

esp_err_t whm_wifi_ip_str(char *out, size_t out_len)
{
    esp_netif_t *nif = (s_desired == WHM_MODE_STA) ? s_netif_sta : s_netif_ap;
    esp_netif_ip_info_t ip;
    if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK) return ESP_FAIL;
    snprintf(out, out_len, IPSTR, IP2STR(&ip.ip));
    return ESP_OK;
}

int whm_wifi_ap_stations(void)
{
    if (s_desired != WHM_MODE_SOFTAP) return -1;
    wifi_sta_list_t l;
    if (esp_wifi_ap_get_sta_list(&l) != ESP_OK) return -1;
    return l.num;
}

int64_t whm_wifi_tsf_now(void)
{
    return esp_wifi_get_tsf_time(tsf_if());
}

bool whm_wifi_drift(double *ppm)
{
    if (!s_ppm_valid) return false;
    *ppm = s_last_ppm;
    return true;
}

/* --------------------------------------------------------------- observer */

static void tsf_task(void *arg)
{
    (void)arg;
    int64_t last_tsf = 0, last_et = 0;
    bool have_last = false;
    uint32_t quiet = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        int64_t tsf = esp_wifi_get_tsf_time(tsf_if());
        int64_t et = esp_timer_get_time();
        s_last_tsf = tsf;

        if (tsf == 0) {
            s_ppm_valid = false;
            have_last = false;
            if ((quiet++ % 15) == 0) {
                ESP_LOGD(TAG, "TSF: 0 (no beacon yet / not associated)");
            }
            continue;
        }
        if (have_last) {
            int64_t d_tsf = tsf - last_tsf;
            int64_t d_et = et - last_et;
            double ppm = d_et ? ((double)(d_tsf - d_et) / (double)d_et) * 1e6 : 0.0;
            s_last_ppm = ppm;
            s_ppm_valid = true;
            /* log at low rate; `tsf` console command gives it on demand */
            static bool first_logged = false;
            if (!first_logged) {
                first_logged = true;
                ESP_LOGI(TAG, "TSF locked: %lld us, offset %+.1f ppm "
                              "('tsf' for live; periodic lines at debug)",
                         (long long)tsf, ppm);
            } else if ((quiet++ % 5) == 0) {
                ESP_LOGD(TAG, "TSF=%lld us  offset=%+.1f ppm",
                         (long long)tsf, ppm);
            }
        }
        last_tsf = tsf;
        last_et = et;
        have_last = true;
    }
}

/* ------------------------------------------------------------------- start */

esp_err_t whm_wifi_start(void)
{
    /* nvs already initialized by whm_settings_init() in app_main */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi_event, NULL, NULL));

    /* decide initial mode: NVS overrides Kconfig; SoftAP is the floor */
    uint8_t mode = 0xFF;
    char ssid[33] = "", pass[65] = "";
    bool have_nvs_mode = (whm_settings_get_u8("wifi_mode", &mode) == ESP_OK);
    whm_settings_get_str("wifi_ssid", ssid, sizeof(ssid));
    whm_settings_get_str("wifi_pass", pass, sizeof(pass));

    /* wifi must be started before set_config on some paths? No: set mode/config
       then start is the canonical order; events drive connect. */
    if (have_nvs_mode && mode == WHM_MODE_STA && ssid[0]) {
        apply_sta(ssid, pass);
    } else if (have_nvs_mode && mode == WHM_MODE_SOFTAP) {
        apply_softap(ssid[0] ? ssid : NULL, pass[0] ? pass : NULL);
    } else if (strlen(CONFIG_WHM_WIFI_SSID) > 0) {
        apply_sta(CONFIG_WHM_WIFI_SSID, CONFIG_WHM_WIFI_PASSWORD);
    } else {
        apply_softap(NULL, NULL);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "WiFi power save: DISABLED (required for TSF accuracy)");

    xTaskCreatePinnedToCore(tsf_task, "whm_tsf", 3072, NULL, 3, NULL, 0);
    return ESP_OK;
}
