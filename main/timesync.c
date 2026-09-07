/*
 * timesync.c - SNTP discipline for the battery-backed RTC.
 *
 * PROVENANCE: authored in an orphaned sibling branch of this session
 * (2026-09-06), quarantined pending review, audited line-by-line, and
 * integrated with owner approval. See evidence bundle AUDIT.md.
 *
 * Clock architecture:
 *   - The PCF85063 RTC is the persistent source of truth (survives power
 *     cycles on its coin cell) and stores LOCAL time - that is what a human
 *     means when they type `rtc set`, and what the clock screen shows.
 *   - SNTP, when enabled and the board is a station with an IP, sets system
 *     time (UTC) and writes through to the RTC in local time on every sync.
 *   - The `tz` POSIX string (NVS) converts between the two. Unset = UTC.
 *   - At boot, system time is seeded FROM the RTC so time()/localtime()
 *     are sane before the first network sync (or forever, off-grid).
 *
 * Known tradeoff, documented: storing local time in the RTC means a DST
 * transition leaves the displayed clock off by the DST delta until the next
 * SNTP sync (default interval 60 min). For a display device this beats
 * making every consumer timezone-aware.
 *
 * NVS keys (namespace "whm"):
 *   ntp_en   u8   1 = enabled (default 1)
 *   ntp_srv  str  server hostname (default "pool.ntp.org")
 *   ntp_ivl  u16  sync interval in minutes (default 60, clamp 1..1440)
 *   tz       str  POSIX TZ (default "" -> UTC0)
 */
#include "timesync.h"
#include "settings.h"
#include "sensors_test.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_wifi.h"

static const char *TAG = "whm_time";

#define NTP_DEFAULT_SERVER "pool.ntp.org"
#define NTP_DEFAULT_IVL_MIN 60

/* lwip stores the server-name POINTER, so this buffer must stay alive */
static char s_server[64] = NTP_DEFAULT_SERVER;
static char s_tz[48] = "";
static bool s_running = false;
static bool s_net_up = false;
static volatile uint32_t s_sync_count = 0;
static volatile int64_t s_last_sync_us = 0;

/* ------------------------------------------------------------- internals */

static bool cfg_enabled(void)
{
    uint8_t en = 1;
    whm_settings_get_u8("ntp_en", &en);
    return en != 0;
}

static uint16_t cfg_interval_min(void)
{
    uint8_t lo = 0, hi = 0;
    uint16_t ivl = NTP_DEFAULT_IVL_MIN;
    /* stored as two u8s to keep the settings API minimal */
    if (whm_settings_get_u8("ntp_ivl_l", &lo) == ESP_OK &&
        whm_settings_get_u8("ntp_ivl_h", &hi) == ESP_OK) {
        ivl = (uint16_t)((hi << 8) | lo);
    }
    if (ivl < 1) ivl = 1;
    if (ivl > 1440) ivl = 1440;
    return ivl;
}

static void cfg_load_server(void)
{
    char buf[sizeof(s_server)] = "";
    if (whm_settings_get_str("ntp_srv", buf, sizeof(buf)) == ESP_OK && buf[0]) {
        strlcpy(s_server, buf, sizeof(s_server));
    } else {
        strlcpy(s_server, NTP_DEFAULT_SERVER, sizeof(s_server));
    }
}

static void on_sync(struct timeval *tv)
{
    s_sync_count++;
    s_last_sync_us = esp_timer_get_time();

    struct tm lt;
    localtime_r(&tv->tv_sec, &lt);
    /* Write-through to the persistent RTC in local time. Runs in the SNTP
     * (tcpip) context; a ~1ms I2C transaction once per sync interval is an
     * acceptable stall, and keeps the flow trivially race-free. */
    esp_err_t err = whm_sensors_set_rtc((uint16_t)(lt.tm_year + 1900),
                                        (uint8_t)(lt.tm_mon + 1),
                                        (uint8_t)lt.tm_mday,
                                        (uint8_t)lt.tm_hour,
                                        (uint8_t)lt.tm_min,
                                        (uint8_t)lt.tm_sec);
    ESP_LOGI(TAG, "SNTP sync #%lu from %s: %04d-%02d-%02d %02d:%02d:%02d "
                  "local%s",
             (unsigned long)s_sync_count, s_server,
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec,
             err == ESP_OK ? " -> RTC" : " (RTC write failed)");
}

static void sntp_start_locked(void)
{
    if (s_running) return;
    cfg_load_server();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_server);
    sntp_set_sync_interval((uint32_t)cfg_interval_min() * 60000U);
    sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();
    s_running = true;
    ESP_LOGI(TAG, "SNTP started: %s every %u min", s_server,
             (unsigned)cfg_interval_min());
}

static void sntp_stop_locked(void)
{
    if (!s_running) return;
    esp_sntp_stop();
    s_running = false;
    ESP_LOGI(TAG, "SNTP stopped");
}

/* ---------------------------------------------------------------- public */

void whm_timesync_apply_tz(void)
{
    char buf[sizeof(s_tz)] = "";
    if (whm_settings_get_str("tz", buf, sizeof(buf)) == ESP_OK && buf[0]) {
        strlcpy(s_tz, buf, sizeof(s_tz));
    } else {
        s_tz[0] = '\0';
    }
    setenv("TZ", s_tz[0] ? s_tz : "UTC0", 1);
    tzset();
    ESP_LOGI(TAG, "timezone: %s", s_tz[0] ? s_tz : "UTC (unset)");
}

const char *whm_timesync_tz(void)
{
    return s_tz[0] ? s_tz : "UTC (unset)";
}

/* A POSIX TZ we can use must start with a >=3-letter zone name followed by
 * a (possibly signed) numeric offset - "PST8...", "NZST-12...", "UTC0".
 * Catches typos and stray words ("save") that newlib would silently treat
 * as UTC. Angle-bracket forms (<+05>-5) are not accepted; use the classic
 * spelling. */
static bool tz_plausible(const char *s)
{
    int alpha = 0;
    const char *p = s;
    while (*p >= 'A' && *p <= 'z' &&
           ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) {
        alpha++;
        p++;
    }
    if (alpha < 3) return false;
    if (*p == '+' || *p == '-') p++;
    return *p >= '0' && *p <= '9';
}

/* Write the current local time to the RTC now (used after a tz change so a
 * power cycle before the next SNTP sync boots in the new zone). */
esp_err_t whm_timesync_rtc_write_now(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 < 2025) return ESP_ERR_INVALID_STATE;
    return whm_sensors_set_rtc((uint16_t)(lt.tm_year + 1900),
                               (uint8_t)(lt.tm_mon + 1), (uint8_t)lt.tm_mday,
                               (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min,
                               (uint8_t)lt.tm_sec);
}

const char *whm_timesync_tz_label(void)
{
    static char lbl[24];
    lbl[0] = 0;
    whm_settings_get_str("tz_lbl", lbl, sizeof(lbl));
    return lbl;
}

esp_err_t whm_timesync_set_tz_named(const char *label, const char *posix)
{
    esp_err_t err = whm_timesync_set_tz(posix);
    if (err == ESP_OK && label && label[0]) {
        whm_settings_set_str("tz_lbl", label);
    }
    return err;
}

esp_err_t whm_timesync_set_tz(const char *tz)
{
    whm_settings_erase("tz_lbl");   /* raw path clears any friendly label */
    esp_err_t err;
    if (tz && tz[0]) {
        if (strlen(tz) >= sizeof(s_tz)) return ESP_ERR_INVALID_ARG;
        if (!tz_plausible(tz)) return ESP_ERR_INVALID_ARG;
        err = whm_settings_set_str("tz", tz);
    } else {
        err = whm_settings_erase("tz");
    }
    if (err == ESP_OK) {
        whm_timesync_apply_tz();
        whm_timesync_rtc_write_now();   /* keep RTC-local in the new zone */
    }
    return err;
}

void whm_timesync_seed_from_rtc(void)
{
    uint16_t y; uint8_t mo, d, h, mi, s;
    if (whm_sensors_read_rtc_parts(&y, &mo, &d, &h, &mi, &s) != ESP_OK) {
        ESP_LOGW(TAG, "no RTC; system time stays at epoch until SNTP");
        return;
    }
    if (y < 2025 || y > 2099) {
        ESP_LOGW(TAG, "RTC year %u implausible; not seeding system time",
                 (unsigned)y);
        return;
    }
    struct tm lt = {
        .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
        .tm_hour = h, .tm_min = mi, .tm_sec = s, .tm_isdst = -1,
    };
    time_t utc = mktime(&lt);          /* interprets lt as local (TZ set) */
    if (utc <= 0) return;
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "system time seeded from RTC (local %04u-%02u-%02u "
                  "%02u:%02u:%02u)",
             (unsigned)y, (unsigned)mo, (unsigned)d,
             (unsigned)h, (unsigned)mi, (unsigned)s);
    if (cfg_enabled() && !s_net_up) {
        ESP_LOGI(TAG, "SNTP armed - RTC will be corrected automatically "
                      "once STA-connected ('wifi join <ssid> [pw]')");
    }
}

void whm_timesync_net_up(void)
{
    s_net_up = true;
    if (cfg_enabled()) sntp_start_locked();
}

void whm_timesync_net_down(void)
{
    s_net_up = false;
    sntp_stop_locked();
}

esp_err_t whm_timesync_enable(bool on)
{
    ESP_RETURN_ON_ERROR(whm_settings_set_u8("ntp_en", on ? 1 : 0), TAG, "nvs");
    if (on) {
        if (s_net_up) sntp_start_locked();
    } else {
        sntp_stop_locked();
    }
    return ESP_OK;
}

esp_err_t whm_timesync_set_server(const char *host)
{
    esp_err_t err;
    if (host && host[0]) {
        if (strlen(host) >= sizeof(s_server)) return ESP_ERR_INVALID_ARG;
        err = whm_settings_set_str("ntp_srv", host);
    } else {
        err = whm_settings_erase("ntp_srv");
    }
    if (err != ESP_OK) return err;
    if (s_running) {                    /* apply live */
        sntp_stop_locked();
        sntp_start_locked();
    } else {
        cfg_load_server();
    }
    return ESP_OK;
}

esp_err_t whm_timesync_set_interval(uint16_t minutes)
{
    if (minutes < 1 || minutes > 1440) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(whm_settings_set_u8("ntp_ivl_l",
                                            (uint8_t)(minutes & 0xFF)),
                        TAG, "nvs");
    ESP_RETURN_ON_ERROR(whm_settings_set_u8("ntp_ivl_h",
                                            (uint8_t)(minutes >> 8)),
                        TAG, "nvs");
    if (s_running) sntp_set_sync_interval((uint32_t)minutes * 60000U);
    return ESP_OK;
}

esp_err_t whm_timesync_force(void)
{
    if (!cfg_enabled()) return ESP_ERR_INVALID_STATE;
    if (!s_net_up) return ESP_ERR_WIFI_NOT_CONNECT;
    /* restart issues an immediate request */
    sntp_stop_locked();
    sntp_start_locked();
    return ESP_OK;
}

whm_timesync_state_t whm_timesync_state(void)
{
    if (!cfg_enabled()) return WHM_TS_DISABLED;
    if (s_sync_count == 0) return WHM_TS_WAITING;
    int64_t age = esp_timer_get_time() - s_last_sync_us;
    int64_t stale = 2LL * (int64_t)cfg_interval_min() * 60LL * 1000000LL;
    return age <= stale ? WHM_TS_SYNCED : WHM_TS_STALE;
}

void whm_timesync_status_print(void)
{
    printf("ntp:      %s%s\n", cfg_enabled() ? "enabled" : "disabled",
           s_running ? " (running)" : (cfg_enabled() && !s_net_up
                                       ? " (waiting for STA network)" : ""));
    cfg_load_server();
    printf("server:   %s%s\n", s_server,
           strcmp(s_server, NTP_DEFAULT_SERVER) == 0 ? " (default)" : "");
    printf("interval: %u min\n", (unsigned)cfg_interval_min());
    if (s_sync_count) {
        int64_t age_s = (esp_timer_get_time() - s_last_sync_us) / 1000000;
        printf("last:     %llds ago (%lu syncs this boot)\n",
               (long long)age_s, (unsigned long)s_sync_count);
    } else {
        printf("last:     never (this boot)\n");
    }
    {
        const char *lbl = whm_timesync_tz_label();
        if (lbl[0]) printf("tz:       %s (%s)\n", lbl, whm_timesync_tz());
        else printf("tz:       %s\n", whm_timesync_tz());
    }

    time_t now = time(NULL);
    struct tm lt, gm;
    localtime_r(&now, &lt);
    gmtime_r(&now, &gm);
    printf("system:   %04d-%02d-%02d %02d:%02d:%02d local / "
           "%02d:%02d:%02d UTC\n",
           lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
           lt.tm_hour, lt.tm_min, lt.tm_sec,
           gm.tm_hour, gm.tm_min, gm.tm_sec);
}
