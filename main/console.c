/*
 * console.c - interactive REPL over USB-Serial-JTAG (this board's only
 * exposed console; UART0's default pins are repurposed for I2S/SD here).
 *
 * Commands:
 *   wifi status                 mode, ssid, ip, rssi, tsf, drift
 *   wifi join <ssid> [pass]     save to NVS + connect now (no reboot)
 *   wifi ap [ssid] [pass]       switch to SoftAP (defaults if omitted)
 *   wifi clear                  forget saved credentials
 *   wifi scan                   list nearby APs
 *   tsf                         TSF value + drift snapshot
 *   sensors                     one-shot IMU / temp+RH / RTC
 *   sysinfo                     heap, uptime, reset reason, versions
 *   sd info|bench|ls            card status / throughput / root listing
 *   audio test                  tone burst + mic RMS (on demand)
 *   brightness <0-255>          live + persisted display brightness
 *   pattern <name>              hold a test pattern; 'cycle' resumes tour
 *   log <error|warn|info|debug> background log verbosity
 *   reboot
 */
#include "console.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_console.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "sdkconfig.h"

#ifndef WHM_VERSION_STR
#define WHM_VERSION_STR "dev"
#endif

#include "wifi_tsf.h"
#include "nvs_flash.h"
#include "ota.h"
#include "sensors_test.h"
#include "storage_test.h"
#include "audio_test.h"
#include "display_hal.h"
#include "ui.h"
#include "mp3_player.h"
#include "tz_table.h"
#include "sync.h"
#include "whlink.h"
#include "http_svc.h"
#include "wifi_tsf.h"
#include "nvs_flash.h"
#include "ota.h"

static void prompt_refresh(void);
#include <ctype.h>
#include "settings.h"
#include "timesync.h"

static const char *TAG = "whm_con";

/* ----------------------------------------------------------------- wifi */

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        whm_wifi_status_print();
        return 0;
    }
    if (strcmp(argv[1], "join") == 0) {
        if (argc < 3) {
            printf("usage: wifi join <ssid> [password]\n");
            return 1;
        }
        const char *pass = (argc >= 4) ? argv[3] : "";
        esp_err_t err = whm_wifi_join(argv[2], pass);
        printf(err == ESP_OK ? "joining \"%s\" (saved)\n" : "join failed: %s\n",
               err == ESP_OK ? argv[2] : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "ap") == 0) {
        const char *ssid = (argc >= 3) ? argv[2] : NULL;
        const char *pass = (argc >= 4) ? argv[3] : NULL;
        esp_err_t err = whm_wifi_softap(ssid, pass);
        printf(err == ESP_OK ? "SoftAP up (saved)\n" : "ap failed: %s\n",
               err == ESP_OK ? "" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "clear") == 0) {
        whm_wifi_clear();
        printf("cleared; compile-time defaults apply on next boot\n");
        return 0;
    }
    if (strcmp(argv[1], "scan") == 0) {
        int n = whm_wifi_scan_print();
        if (n >= 0) printf("%d network(s)\n", n);
        return n >= 0 ? 0 : 1;
    }
    printf("usage: wifi <status|join|ap|clear|scan>\n");
    return 1;
}

/* ------------------------------------------------------------------ tsf */

static int cmd_tsf(int argc, char **argv)
{
    (void)argc; (void)argv;
    int64_t tsf = whm_wifi_tsf_now();
    printf("tsf: %lld us (%s)\n", (long long)tsf,
           whm_wifi_is_sta() ? "STA - disciplined to AP beacons"
                             : "SoftAP - we are the beacon source");
    double ppm;
    if (whm_wifi_drift(&ppm)) {
        printf("drift vs local crystal: %+.1f ppm over last 2s window\n", ppm);
    } else {
        printf("drift: no estimate yet (need two samples with association)\n");
    }
    return 0;
}

/* -------------------------------------------------------------- sensors */

static int cmd_sensors(int argc, char **argv)
{
    (void)argc; (void)argv;
    float ax, ay, az, gx, gy, gz, t, rh;
    char ts[32];
    if (whm_sensors_read_imu(&ax, &ay, &az, &gx, &gy, &gz) == ESP_OK) {
        printf("imu:  accel[m/s2] %+.2f %+.2f %+.2f  gyro[rad/s] %+.3f %+.3f %+.3f\n",
               ax, ay, az, gx, gy, gz);
    } else {
        printf("imu:  unavailable\n");
    }
    if (whm_sensors_read_th(&t, &rh) == ESP_OK) {
        printf("env:  %.1f C  %.0f %%RH\n", t, rh);
    } else {
        printf("env:  unavailable\n");
    }
    if (whm_sensors_read_rtc(ts, sizeof(ts)) == ESP_OK) {
        printf("rtc:  %s\n", ts);
    } else {
        printf("rtc:  unavailable\n");
    }
    return 0;
}

/* ------------------------------------------------------------------ ntp */

static int cmd_ntp(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        whm_timesync_status_print();
        return 0;
    }
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        bool on = argv[1][1] == 'n';
        esp_err_t err = whm_timesync_enable(on);
        if (err != ESP_OK) { printf("failed: %s\n", esp_err_to_name(err)); return 1; }
        printf("ntp %s (saved)%s\n", on ? "enabled" : "disabled",
               on ? " - syncs when the board is a station with an IP" : "");
        return 0;
    }
    if (strcmp(argv[1], "server") == 0) {
        if (argc < 3) { printf("usage: ntp server <host|clear>\n"); return 1; }
        const char *host = strcmp(argv[2], "clear") == 0 ? NULL : argv[2];
        esp_err_t err = whm_timesync_set_server(host);
        if (err != ESP_OK) { printf("failed: %s\n", esp_err_to_name(err)); return 1; }
        printf("ntp server: %s (saved)\n", host ? host : "pool.ntp.org (default)");
        return 0;
    }
    if (strcmp(argv[1], "interval") == 0) {
        if (argc < 3) { printf("usage: ntp interval <minutes 1-1440>\n"); return 1; }
        int m = atoi(argv[2]);
        esp_err_t err = whm_timesync_set_interval((uint16_t)m);
        if (err != ESP_OK) { printf("out of range (1-1440)\n"); return 1; }
        printf("ntp interval: %d min (saved)\n", m);
        return 0;
    }
    if (strcmp(argv[1], "sync") == 0) {
        esp_err_t err = whm_timesync_force();
        if (err == ESP_ERR_INVALID_STATE) { printf("ntp is disabled ('ntp on' first)\n"); return 1; }
        if (err == ESP_ERR_WIFI_NOT_CONNECT) { printf("no STA network - join one first ('wifi join ...')\n"); return 1; }
        printf("sync requested; watch for 'SNTP sync' in the log\n");
        return 0;
    }
    printf("usage: ntp [status|on|off|server <host|clear>|interval <min>|sync]\n");
    return 1;
}

static void tz_apply_echo(const char *label, const char *posix)
{
    esp_err_t err = label ? whm_timesync_set_tz_named(label, posix)
                          : whm_timesync_set_tz(posix);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("not a usable POSIX TZ string: '%s'\n", posix);
        printf("try 'tz list', a city ('tz auckland'), a region "
               "('tz pacific'),\nor classic POSIX: tz PST8PDT,M3.2.0,M11.1.0\n");
        return;
    }
    if (err != ESP_OK) { printf("failed: %s\n", esp_err_to_name(err)); return; }
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    printf("tz applied: %s%s%s\n", label ? label : "",
           label ? " - " : "", whm_timesync_tz());
    if (lt.tm_year + 1900 >= 2025) {
        printf("local now:  %04d-%02d-%02d %02d:%02d:%02d (RTC updated)\n",
               lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
               lt.tm_hour, lt.tm_min, lt.tm_sec);
    } else {
        printf("(system time unknown - RTC left alone; sync or 'rtc set')\n");
    }
}

static bool ci_contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && p[k] &&
               tolower((unsigned char)p[k]) ==
                   tolower((unsigned char)needle[k])) {
            k++;
        }
        if (k == nl) return true;
    }
    return false;
}

static int cmd_tz(int argc, char **argv)
{
    if (argc < 2) {
        const char *lbl = whm_timesync_tz_label();
        if (lbl[0]) printf("tz: %s (%s)\n", lbl, whm_timesync_tz());
        else printf("tz: %s\n", whm_timesync_tz());
        printf("usage: tz <city|region>   e.g. tz pacific, tz auckland\n");
        printf("       tz list | tz clear | tz <POSIX string>\n");
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        whm_timesync_set_tz(NULL);
        printf("tz cleared (UTC)\n");
        return 0;
    }
    if (strcmp(argv[1], "list") == 0) {
        for (int i = 0; i < k_tz_count; i++) {
            printf("  %-13s %-24s %s\n", k_tz_table[i].alias,
                   k_tz_table[i].place, k_tz_table[i].posix);
        }
        return 0;
    }
    /* exact alias */
    for (int i = 0; i < k_tz_count; i++) {
        if (strcasecmp(argv[1], k_tz_table[i].alias) == 0) {
            tz_apply_echo(k_tz_table[i].alias, k_tz_table[i].posix);
            return 0;
        }
    }
    /* substring over alias + place */
    int hits[8], nh = 0;
    for (int i = 0; i < k_tz_count && nh < 8; i++) {
        if (ci_contains(k_tz_table[i].alias, argv[1]) ||
            ci_contains(k_tz_table[i].place, argv[1])) {
            hits[nh++] = i;
        }
    }
    if (nh == 1) {
        tz_apply_echo(k_tz_table[hits[0]].alias, k_tz_table[hits[0]].posix);
        return 0;
    }
    if (nh > 1) {
        printf("ambiguous - did you mean:\n");
        for (int i = 0; i < nh; i++) {
            printf("  tz %-13s (%s)\n", k_tz_table[hits[i]].alias,
                   k_tz_table[hits[i]].place);
        }
        return 1;
    }
    /* no table match: treat as raw POSIX */
    tz_apply_echo(NULL, argv[1]);
    return 0;
}

/* ------------------------------------------------------------------ rtc */

static int cmd_rtc(int argc, char **argv)
{
    if (argc < 2) {
        char ts[32];
        if (whm_sensors_read_rtc(ts, sizeof(ts)) == ESP_OK) {
            printf("rtc: %s\n", ts);
        } else {
            printf("rtc: unavailable\n");
        }
        printf("set with: rtc set YYYY-MM-DD HH:MM:SS (local time; "
               "NTP overwrites on next sync when enabled)\n");
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc == 4) {
        unsigned y, mo, d, h, mi, s;
        if (sscanf(argv[2], "%u-%u-%u", &y, &mo, &d) == 3 &&
            sscanf(argv[3], "%u:%u:%u", &h, &mi, &s) == 3) {
            esp_err_t err = whm_sensors_set_rtc((uint16_t)y, (uint8_t)mo,
                                                (uint8_t)d, (uint8_t)h,
                                                (uint8_t)mi, (uint8_t)s);
            if (err == ESP_OK) {
                printf("rtc set to %04u-%02u-%02u %02u:%02u:%02u\n",
                       y, mo, d, h, mi, s);
                return 0;
            }
            printf("rtc set failed: %s\n", esp_err_to_name(err));
            return 1;
        }
    }
    printf("usage: rtc [set YYYY-MM-DD HH:MM:SS]\n");
    return 1;
}

/* -------------------------------------------------------------- sysinfo */

static int cmd_sysinfo(int argc, char **argv)
{
    (void)argc; (void)argv;
    int64_t up = esp_timer_get_time();
    printf("whm:      %s\n", WHM_VERSION_STR);
    printf("idf:      %s\n", esp_get_idf_version());
    printf("uptime:   %lld.%03llds\n", (long long)(up / 1000000),
           (long long)((up / 1000) % 1000));
    printf("reset:    %d\n", (int)esp_reset_reason());
    printf("heap:     internal %u KB free (largest %u KB, min-ever %u KB), "
           "psram %u KB free\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)
                      / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
                      / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("display:  %ux%u %s\n", whm_display_width(), whm_display_height(),
           whm_display_running() ? "running" : "stopped");
    return 0;
}

/* ------------------------------------------------------------------- sd */

static int cmd_sd(int argc, char **argv)
{
    const char *sub = argc >= 2 ? argv[1] : "info";
    if (strcmp(sub, "info") == 0) {
        if (!whm_storage_mounted()) whm_storage_mount();
        whm_storage_info();
        return 0;
    }
    if (strcmp(sub, "mount") == 0) {
        if (whm_storage_mounted()) {
            printf("already mounted\n");
            return 0;
        }
        return whm_storage_mount() == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "unmount") == 0) {
        return whm_storage_unmount() == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "format") == 0) {
        /* Fat-finger-proof, not adversary-proof, by design: a numeric code
         * you must read off this output. No expiry (walk away and come
         * back), but the code is BOUND TO THE CARD SERIAL captured at
         * challenge time - a swapped or remounted-different card cancels,
         * so a stale code can never format the wrong card. Cleared by use,
         * a wrong code, or reboot. */
        static char code[8] = "";
        static uint32_t code_serial = 0;

        if (argc >= 3) {
            if (!code[0]) {
                printf("no pending confirmation - run 'sd format' first\n");
                return 1;
            }
            uint32_t serial_now = 0;
            if (!whm_storage_mounted()) whm_storage_mount();
            if (whm_storage_card_serial(&serial_now) != ESP_OK ||
                serial_now != code_serial) {
                code[0] = '\0';
                printf("card changed or removed since the challenge - "
                       "cancelled; run 'sd format' again\n");
                return 1;
            }
            if (strcmp(argv[2], code) != 0) {
                code[0] = '\0';
                printf("code mismatch - cancelled; run 'sd format' to "
                       "start over\n");
                return 1;
            }
            int midx;
            if (whm_mp3_state(&midx) != WHM_MP3_STOPPED) {
                code[0] = '\0';
                printf("music is playing from this card - 'mp3 stop' "
                       "first\n");
                return 1;
            }
            code[0] = '\0';
            printf("formatting - this can take a minute or two on large or "
                   "slow cards...\n");
            int64_t t0 = esp_timer_get_time();
            esp_err_t err = whm_storage_format();
            if (err == ESP_OK) {
                printf("done in %llds\n",
                       (long long)((esp_timer_get_time() - t0) / 1000000));
                whm_mp3_invalidate();
                printf("(media library cleared - add files, then "
                       "'mp3 scan')\n");
                whm_storage_info();
                return 0;
            }
            printf("format failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        if (!whm_storage_mounted() && whm_storage_mount() != ESP_OK) {
            printf("no card to format\n");
            return 1;
        }
        if (whm_storage_card_serial(&code_serial) != ESP_OK) {
            printf("cannot read card identity\n");
            return 1;
        }
        char desc[64];
        whm_storage_card_desc(desc, sizeof(desc));
        snprintf(code, sizeof(code), "%04lu",
                 (unsigned long)(esp_random() % 10000UL));
        printf("!!! DESTRUCTIVE: this ERASES EVERYTHING on the card and\n");
        printf("!!! reformats the ENTIRE card as FAT (all partitions gone).\n");
        printf("card: %s\n", desc);
        printf("to proceed, type exactly:  sd format %s\n", code);
        printf("(code valid until used, a wrong code, card change, or "
               "reboot)\n");
        return 0;
    }
    if (strcmp(sub, "bench") == 0) {
        if (!whm_storage_mounted() && whm_storage_mount() != ESP_OK) {
            printf("no card\n");
            return 1;
        }
        return whm_storage_benchmark() == ESP_OK ? 0 : 1;
    }
    if (strcmp(sub, "ls") == 0) {
        if (!whm_storage_mounted() && whm_storage_mount() != ESP_OK) {
            printf("no card\n");
            return 1;
        }
        char path[128];
        const char *arg = argc >= 3 ? argv[2] : "/sdcard";
        if (arg[0] == '/') strlcpy(path, arg, sizeof(path));
        else snprintf(path, sizeof(path), "/sdcard/%s", arg);
        return whm_storage_ls(path) == ESP_OK ? 0 : 1;
    }
    printf("usage: sd <info|mount|unmount|bench|ls [path]|format>\n");
    return 1;
}

/* ---------------------------------------------------------------- audio */

static int cmd_audio(int argc, char **argv)
{
    (void)argc; (void)argv;
    return whm_audio_test() == ESP_OK ? 0 : 1;
}

/* ----------------------------------------------------------- brightness */

static int cmd_brightness(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: brightness <0-255>\n");
        return 1;
    }
    int v = atoi(argv[1]);
    if (v < 0 || v > 255) {
        printf("out of range 0-255\n");
        return 1;
    }
    whm_display_set_brightness((uint8_t)v);
    whm_settings_set_u8("bright", (uint8_t)v);
    printf("brightness %d (saved)\n", v);
    if (v > 180) {
        printf("note: solid white near full brightness approaches ~4A on this panel\n");
    }
    return 0;
}

/* -------------------------------------------------------------- pattern */

static int cmd_pattern(int argc, char **argv)
{
    if (argc < 2) {
        const char *cur;
        if (whm_ui_pattern_active(&cur)) {
            printf("pattern: %s (running - 'pattern off' resumes screens)\n",
                   cur);
        } else {
            printf("pattern: off\n");
        }
        printf("usage: pattern <%s|off>\n", whm_ui_pattern_names());
        return 0;
    }
    if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "stop") == 0) {
        whm_ui_pattern_off();
        return 0;
    }
    if (!whm_ui_pattern_set(argv[1])) {
        printf("unknown pattern; one of: %s (or off)\n",
               whm_ui_pattern_names());
        return 1;
    }
    return 0;
}

static int cmd_screen(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: screen <%s>\n", whm_ui_screen_names());
        return 1;
    }
    if (strcmp(argv[1], "list") == 0) {
        printf("%s\n", whm_ui_screen_names());
        return 0;
    }
    if (strcmp(argv[1], "bootmode") == 0) {
        if (argc < 3 || (strcmp(argv[2], "home") && strcmp(argv[2], "auto"))) {
            printf("usage: screen bootmode <home|auto>\n");
            return 1;
        }
        whm_settings_set_u8("boot_auto", strcmp(argv[2], "auto") == 0 ? 1 : 0);
        printf("boot mode: %s (saved)\n", argv[2]);
        return 0;
    }
    if (!whm_ui_screen_set(argv[1])) {
        printf("unknown screen; one of: %s\n", whm_ui_screen_names());
        return 1;
    }
    return 0;
}

/* ----------------------------------------------------------------- name */

static int cmd_name(int argc, char **argv)
{
    char name[17] = "";
    if (argc < 2) {
        if (whm_settings_get_str("node", name, sizeof(name)) == ESP_OK && name[0]) {
            printf("node name: %s\n", name);
        } else {
            printf("node name: (unset, shows WHM)\n");
        }
        return 0;
    }
    if (strcmp(argv[1], "clear") == 0) {
        whm_settings_erase("node");
        printf("node name cleared\n");
        whm_ui_name_dirty();
        prompt_refresh();
        whm_sync_name_refresh();
        if (whm_wifi_ap_stations() >= 0) whm_wifi_softap(NULL, NULL);
        return 0;
    }
    size_t n = strlen(argv[1]);
    if (n < 1 || n > 16) {
        printf("1-16 chars\n");
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        char c = argv[1][i];
        if (!isalnum((unsigned char)c) && c != '-') {
            printf("letters, digits, hyphen only (this becomes the mDNS "
                   "hostname and SoftAP SSID)\n");
            return 1;
        }
    }
    whm_settings_set_str("node", argv[1]);
    printf("node name: %s (saved)\n", argv[1]);
    whm_ui_name_dirty();                    /* home screen, live */
    prompt_refresh();                       /* this console, next line */
    whm_sync_name_refresh();                /* mDNS hostname + instance */
    printf("  mdns: %s.local\n", argv[1]);
    if (whm_wifi_ap_stations() >= 0) {      /* SoftAP SSID IS the name */
        whm_wifi_softap(NULL, NULL);
        printf("  SoftAP restarted as '%s' - members must rejoin\n",
               argv[1]);
    }
    return 0;
}

/* ------------------------------------------------------------------ log */

static int cmd_log(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: log <error|warn|info|debug>\n");
        return 1;
    }
    esp_log_level_t lvl;
    if (strcmp(argv[1], "error") == 0) lvl = ESP_LOG_ERROR;
    else if (strcmp(argv[1], "warn") == 0) lvl = ESP_LOG_WARN;
    else if (strcmp(argv[1], "info") == 0) lvl = ESP_LOG_INFO;
    else if (strcmp(argv[1], "debug") == 0) lvl = ESP_LOG_DEBUG;
    else { printf("unknown level\n"); return 1; }
    esp_log_level_set("*", lvl);
    printf("log level: %s\n", argv[1]);
    return 0;
}

/* --------------------------------------------------------------- reboot */

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting...\n");
    fflush(stdout);
    esp_restart();
    return 0;   /* unreached */
}

/* ----------------------------------------------------------------- mp3 */

static int cmd_mp3(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        int idx;
        whm_mp3_state_t st = whm_mp3_state(&idx);
        const char *names[] = {"stopped", "playing", "paused"};
        if (idx >= 0) {
            char t[48], a[48];
            whm_mp3_track_info(idx, t, sizeof(t), a, sizeof(a));
            uint32_t pos, dur;
            whm_mp3_progress(&pos, &dur);
            printf("mp3: %s [%d] %s - %s  %lu:%02lu/%lu:%02lu  vol %u\n",
                   names[st], idx, t, a[0] ? a : "?",
                   (unsigned long)(pos / 60), (unsigned long)(pos % 60),
                   (unsigned long)(dur / 60), (unsigned long)(dur % 60),
                   (unsigned)whm_mp3_vol());
        } else {
            printf("mp3: %s (%d track(s) indexed)\n", names[st],
                   whm_mp3_count());
        }
        printf("usage: mp3 list|scan|play [n]|stop|pause|next|prev|vol <0-100>\n");
        return 0;
    }
    if (strcmp(argv[1], "scan") == 0 || strcmp(argv[1], "list") == 0) {
        if (whm_mp3_count() == 0 || strcmp(argv[1], "scan") == 0) {
            whm_mp3_scan();
        }
        for (int i = 0; i < whm_mp3_count(); i++) {
            char t[48], a[48];
            whm_mp3_track_info(i, t, sizeof(t), a, sizeof(a));
            printf("  %2d: %s%s%s\n", i, t, a[0] ? " - " : "",
                   a[0] ? a : "");
        }
        if (!whm_mp3_count()) printf("  (none - drop .mp3 into /sdcard/media)\n");
        return 0;
    }
    if (strcmp(argv[1], "layout") == 0 && argc >= 3) {
        uint8_t lay = strcmp(argv[2], "full") == 0 ? 0
                    : strcmp(argv[2], "art") == 0 ? 1
                    : strcmp(argv[2], "info") == 0 ? 2 : 0xFF;
        if (lay == 0xFF) {
            printf("mp3 layout <full|art|info>\n");
            return 1;
        }
        whm_ui_music_layout(lay, -1);
        printf("music layout: %s (saved)\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "overlay") == 0 && argc >= 3) {
        int8_t ov = strcmp(argv[2], "on") == 0 ? 1
                  : strcmp(argv[2], "off") == 0 ? 0 : -1;
        if (ov < 0) { printf("mp3 overlay <on|off>\n"); return 1; }
        whm_ui_music_layout(0xFF, ov);
        printf("art overlay: %s (saved)\n", argv[2]);
        return 0;
    }
    if (strcmp(argv[1], "dj") == 0) {
        if (argc >= 3 && strcmp(argv[2], "stop") == 0) {
            whm_mp3_dj_stop();
            whm_sync_djb_send(false, 48000, 2, "");
            printf("dj: stopped (followers released)\n");
            return 0;
        }
        int n = argc >= 3 ? atoi(argv[2]) : 0;
        if (whm_mp3_dj_start(n) != ESP_OK) {
            printf("dj: start failed\n");
            return 1;
        }
        char t[64] = "";
        whm_mp3_now_title(t, sizeof(t));
        whm_sync_djb_send(true, 48000, 2, t);
        printf("dj: serving '%s' on :7778 - followers tuning in\n", t);
        return 0;
    }
    if (strcmp(argv[1], "fleet") == 0) {
        int64_t t = whm_wifi_tsf_now();
        if (t <= 0) {
            printf("no show clock (wifi TSF) - join a flock first\n");
            return 1;
        }
        if (argc >= 3 && strcmp(argv[2], "stop") == 0) {
            whm_sync_play_send("", NULL, t, true);
            whm_mp3_fleet_play("", NULL, 0, true);
            printf("mode A: stop, fleet-wide\n");
            return 0;
        }
        int idx = argc >= 3 ? atoi(argv[2]) : -1;
        char name[64];
        if (whm_mp3_track_fname(idx, name, sizeof(name)) != ESP_OK) {
            printf("no such track\n");
            return 1;
        }
        char sha[65];
        long sz;
        printf("resolving %s (hashing if first time)...\n", name);
        if (whm_http_media_lookup(name, sha, &sz) != ESP_OK) {
            printf("manifest lookup failed\n");
            return 1;
        }
        int64_t start = t + 750000;
        whm_sync_play_send(name, sha, start, false);
        whm_mp3_fleet_play_m(name, sha, start, false, true, NULL);
        printf("mode A: '%s' fleet-wide, downbeat in 750ms\n", name);
        return 0;
    }
    if (strcmp(argv[1], "play") == 0) {
        if (whm_mp3_count() == 0) whm_mp3_scan();
        if (whm_mp3_count() == 0) { printf("no tracks\n"); return 1; }
        whm_mp3_play(argc >= 3 ? atoi(argv[2]) : -1);
        whm_ui_screen_set("music");
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) { whm_mp3_stop(); return 0; }
    if (strcmp(argv[1], "pause") == 0 || strcmp(argv[1], "resume") == 0 ||
        strcmp(argv[1], "toggle") == 0) { whm_mp3_toggle(); return 0; }
    if (strcmp(argv[1], "next") == 0) { whm_mp3_next(); return 0; }
    if (strcmp(argv[1], "prev") == 0) { whm_mp3_prev(); return 0; }
    if (strcmp(argv[1], "vol") == 0 && argc >= 3) {
        whm_mp3_set_vol((uint8_t)atoi(argv[2]));
        printf("mp3 vol: %d (saved)\n", atoi(argv[2]));
        return 0;
    }
    printf("usage: mp3 list|scan|play [n]|stop|pause|next|prev|vol <0-100>\n");
    return 1;
}

/* ------------------------------------------------------------------ ota */

static int cmd_ota(int argc, char **argv)
{
    if (argc >= 2) {                     /* never pull from yourself */
        char hn[32];
        strlcpy(hn, argv[1], sizeof(hn));
        char *dot = strstr(hn, ".local");
        if (dot) *dot = 0;
        const char *me = whm_sync_node_name();
        if (me && strcasecmp(hn, me) == 0) {
            printf("ota: that's me - I'm the source, nothing to "
                   "pull\n");
            return 0;
        }
    }
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        whm_ota_status_print();
        return 0;
    }
    return whm_ota_from_url(argv[1]) == ESP_OK ? 0 : 1;
}

/* -------------------------------------------------------------- factory */

static int cmd_factory(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "confirm") == 0) {
        printf("erasing ALL settings (NVS) and rebooting...\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        nvs_flash_erase();
        esp_restart();
    }
    printf("factory confirm   - erase every saved setting (name, wifi,\n");
    printf("                    sync mode/prio, volume, layouts, all of\n");
    printf("                    it) and reboot fresh. No undo.\n");
    return 0;
}

/* --------------------------------------------------------------- oracle */

static int cmd_oracle(int argc, char **argv)
{
    (void)argc; (void)argv;
    whm_ui_screen_set("oracle");
    whm_ui_oracle_ask();
    printf("consulting the oracle...\n");
    return 0;
}

/* ---------------------------------------------------------------- fleet */

static int cmd_fleet(int argc, char **argv)
{
    if (whm_sync_role() != WHM_SYNC_CONDUCTOR) {
        printf("fleet: this node is a follower - only the anchor/"
               "conductor commands the fleet\n");
        printf("       ('sync anchor' to claim, or run this from the "
               "anchor's console)\n");
        return 1;
    }
    if (argc < 2) {
        printf("usage: fleet <console line>  (runs on every node, "
               "including this one)\n");
        return 1;
    }
    const char *target = NULL;
    int first = 1;
    if (argv[1][0] == '@' && argv[1][1]) {
        target = argv[1] + 1;
        first = 2;
        if (argc < 3) {
            printf("usage: fleet @<node> <console line>\n");
            return 1;
        }
    }
    if (strcmp(argv[first], "fleet") == 0) {
        printf("no recursive fleet\n");
        return 1;
    }
    char line[160] = "";
    for (int i = first; i < argc; i++) {
        if (line[0]) strlcat(line, " ", sizeof(line));
        strlcat(line, argv[i], sizeof(line));
    }
    esp_err_t err = whm_sync_fleet_send_to(target, line);
    if (err != ESP_OK) printf("fleet send failed (sync off?)\n");
    /* self-defer now happens inside send with the packet's own
       exec_at - one timestamp, two consumers, zero divergence */
    if (!target) {
        return err == ESP_OK ? 0 : 1;
    }
    printf("sent to %s\n", target);
    return err == ESP_OK ? 0 : 1;
}

/* ---------------------------------------------------------------- clock */

static int cmd_clock(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "label") == 0) {
        if (strcmp(argv[2], "off") == 0) {
            whm_settings_set_str("clk_lbl", "-");
            printf("clock label: off\n");
        } else if (strcmp(argv[2], "tz") == 0) {
            whm_settings_erase("clk_lbl");
            printf("clock label: timezone name\n");
        } else {
            char lbl[16] = "";
            for (int i = 2; i < argc && strlen(lbl) < 12; i++) {
                if (lbl[0]) strlcat(lbl, " ", sizeof(lbl));
                strlcat(lbl, argv[i], sizeof(lbl));
            }
            whm_settings_set_str("clk_lbl", lbl);
            printf("clock label: \"%s\"\n", lbl);
        }
        return 0;
    }
    char lbl[16] = "";
    whm_settings_get_str("clk_lbl", lbl, sizeof(lbl));
    printf("clock label: %s\n",
           lbl[0] == '-' ? "off" : lbl[0] ? lbl : "(timezone name)");
    printf("usage: clock label <text> | clock label tz | clock label off\n");
    printf("world-clock recipe: tz tokyo + clock label tz, per unit\n");
    return 0;
}

/* ------------------------------------------------------------------ vol */

static int cmd_walk(int argc, char **argv)
{
    float cur, tgt;
    if (argc >= 3 && strcmp(argv[1], "pure") == 0) {
        whm_ui_walk_pure(strcmp(argv[2], "on") == 0);
        printf("walk: pure mode %s (rewards + ambient rituals "
               "stripped for bisection)\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "speed") == 0) {
        whm_ui_walk_speed(strtof(argv[2], NULL));
        whm_ui_walk_speed_get(&cur, &tgt);
        printf("walk: slewing to %.2fx (tip: 'fleet walk speed' keeps "
           "panels lock-step)\n", tgt);
        return 0;
    }
    whm_ui_walk_speed_get(&cur, &tgt);
    uint32_t fok, fsn, fst;
    whm_ui_walk_stats(&fok, &fsn, &fst);
    printf("walk: refs ok=%lu snap=%lu stale=%lu\n",
           (unsigned long)fok, (unsigned long)fsn,
           (unsigned long)fst);
    printf("walk: %.2fx (target %.2fx), cam=%.2f - "
           "'walk speed <-2..2>', 0 pauses, negative reverses\n",
           cur, tgt, whm_ui_walk_cam());
    return 0;
}

static int cmd_fw(int argc, char **argv)   /* hidden: fireworks test */
{
    if (argc < 2) { whm_ui_fw_test(0, 0); printf("fw: off\n"); return 0; }
    if (strcmp(argv[1], "test") == 0) {
        whm_ui_fw_test(1, 0);
        printf("fw: nightly show armed (walker stages in ~4s, "
               "midnight in ~12s)\n");
    } else if (strcmp(argv[1], "nye") == 0) {
        int fast = argc >= 3 && strcmp(argv[2], "fast") == 0;
        whm_ui_fw_test(2, fast);
        printf("fw: NYE spectacular armed%s - staging now, show "
               "runs the full arc\n", fast ? " (10x)" : "");
    } else { whm_ui_fw_test(0, 0); printf("fw: off\n"); }
    return 0;
}

static int cmd_vol(int argc, char **argv)
{
    if (argc < 2) {
        printf("vol: %u (beep/chime/tone; 'mp3 vol' is separate)\n",
               (unsigned)whm_audio_sys_vol());
        return 0;
    }
    int v = atoi(argv[1]);
    if (whm_audio_set_sys_vol((uint8_t)v) != ESP_OK) {
        printf("usage: vol <1-100>\n");
        return 1;
    }
    printf("vol: %d (saved)\n", v);
    whm_audio_tone_async(660, 160);        /* audible confirmation */
    return 0;
}

/* ---------------------------------------------------------------- tones */

static int cmd_chime(int argc, char **argv)
{
    (void)argc; (void)argv;
    whm_audio_chime_async();
    return 0;
}

static int cmd_tone(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: tone <hz> [ms]   (40-12000 Hz, 20-5000 ms)\n");
        printf("       e.g. tone 440 300\n");
        return 1;
    }
    float hz = strtof(argv[1], NULL);
    int ms = argc >= 3 ? atoi(argv[2]) : 300;
    if (whm_audio_tone_async(hz, ms) != ESP_OK) {
        printf("out of range: 40-12000 Hz, 20-5000 ms\n");
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------- wander */

static int cmd_wander(int argc, char **argv)
{
    uint8_t idx, n;
    uint16_t spd;
    whm_ui_wander_get(&idx, &n, &spd);
    if (argc < 2) {
        printf("wander: panel %u of %u, %u px/s\n", (unsigned)(idx + 1),
               (unsigned)n, (unsigned)spd);
        printf("usage: wander <idx> <n> [px_per_s]   (idx 0-based)\n");
        printf("       wander go                     (start the pattern)\n");
        return 0;
    }
    if (strcmp(argv[1], "go") == 0) {
        whm_ui_pattern_set("wander");
        return 0;
    }
    if (argc >= 3) {
        int i = atoi(argv[1]), nn = atoi(argv[2]);
        int sp = argc >= 4 ? atoi(argv[3]) : spd;
        if (whm_ui_wander_set((uint8_t)i, (uint8_t)nn,
                              (uint16_t)sp) != ESP_OK) {
            printf("range: idx < n, n 1-8, speed 5-240\n");
            return 1;
        }
        printf("wander: panel %d of %d, %d px/s (saved)\n", i + 1, nn, sp);
        return 0;
    }
    printf("usage: wander <idx> <n> [px_per_s] | wander go\n");
    return 1;
}

/* ----------------------------------------------------------------- sync */

static int cmd_ble(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "pair") == 0) {
        uint32_t secs = 120;
        if (argc >= 3) secs = (uint32_t)atoi(argv[2]);
        if (!secs) secs = 120;
        whm_whlink_pair_window(secs);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "feed") == 0) {
        whm_whlink_feed(argc >= 3 && strcmp(argv[2], "on") == 0);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        whm_whlink_forget();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        whm_whlink_off();
        return 0;
    }
    whm_whlink_status_print();
    printf("usage: ble                 link status\n");
    printf("       ble pair [secs]    open pairing window (adv)\n");
    printf("       ble off            close window / drop link\n");
    printf("       ble forget         erase the bond\n");
    printf("       ble feed on|off    tunnel fleet 2/7/9 to peer\n");
    return 0;
}

static int cmd_sync(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        whm_sync_status_print();
        if (argc < 2) {
            printf("usage: sync auto              join elections\n");
            printf("       sync anchor            claim now (prio>=200)\n");
            printf("       sync follow [name]     member; name = pin\n");
            printf("       sync prio <0-255>      election weight\n");
            printf("       sync conduct|join <n>  own-SoftAP flock (no "
                   "elections)\n");
            printf("       sync media <host>      pull tracks from a "
                   "node\n");
            printf("       sync discover | off\n");
        }
        return 0;
    }
    if (strcmp(argv[1], "conduct") == 0) {
        return whm_sync_conduct() == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "auto") == 0) {
        return whm_sync_auto() == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "lead") == 0 && argc >= 3) {
        whm_sync_set_lead_ms((uint32_t)atoi(argv[2]));
        printf("fleet lead set (saved)\n");
        return 0;
    }
    if (strcmp(argv[1], "prio") == 0 && argc >= 3) {
        int v = atoi(argv[2]);
        if (v < 0 || v > 255) { printf("prio 0-255 (0 = never anchor)\n");
                                return 1; }
        whm_sync_set_prio((uint8_t)v);
        printf("sync prio: %d (saved)\n", v);
        return 0;
    }
    if (strcmp(argv[1], "anchor") == 0) {
        return whm_sync_anchor() == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "follow") == 0) {
        return whm_sync_follow(argc >= 3 ? argv[2] : NULL) == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "join") == 0 && argc >= 3) {
        return whm_sync_join(argv[2]) == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        return whm_sync_off() == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "media") == 0 && argc >= 3) {
        {
            char me2[17] = "";
            whm_settings_get_str("node", me2, sizeof(me2));
            char hb[24];
            const char *hh = argv[2];
            while (*hh == '@') hh++;
            strlcpy(hb, hh, sizeof(hb));
            char *dot = strstr(hb, ".local");
            if (dot) *dot = 0;
            if (me2[0] && strcasecmp(hb, me2) == 0) {
                printf("sync media: that's me - nothing to pull\n");
                return 0;
            }
        }
        return whm_media_sync(argv[2]) == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "discover") == 0) {
        whm_sync_discover_print();
        return 0;
    }
    printf("usage: sync [status|discover|media <host>] | conduct | "
           "join <name> | anchor | follow [name] | prio <n> | off\n");
    return 1;
}

/* ----------------------------------------------------------------- life */

static int cmd_life(int argc, char **argv)
{
    if (argc < 2) {
        printf("life palette: %s\n",
               whm_ui_life_pal_name(whm_ui_life_get_pal()));
        printf("usage: life <pal|next|reset>  pals: classic blue green red rainbow purple ember prism\n");
        printf("(on the life screen: 2TAP cycles palette, HOLD reseeds)\n");
        return 0;
    }
    if (strcmp(argv[1], "next") == 0) {
        whm_ui_life_set_pal((whm_ui_life_get_pal() + 1) %
                            whm_ui_life_pal_count());
        printf("life palette: %s\n",
               whm_ui_life_pal_name(whm_ui_life_get_pal()));
        return 0;
    }
    if (strcmp(argv[1], "reset") == 0) {
        whm_ui_life_reseed();
        printf("reseeded\n");
        return 0;
    }
    for (int i = 0; i < whm_ui_life_pal_count(); i++) {
        if (strcmp(argv[1], whm_ui_life_pal_name(i)) == 0) {
            whm_ui_life_set_pal(i);
            whm_ui_screen_set("life");
            printf("life palette: %s\n", whm_ui_life_pal_name(i));
            return 0;
        }
    }
    printf("usage: life <pal|next|reset>  pals: classic blue green red rainbow purple ember prism\n");
    return 1;
}

/* ---------------------------------------------------------------- timer */

static bool parse_duration(const char *s, uint32_t *out_s)
{
    unsigned m = 0, sec = 0;
    if (strchr(s, ':')) {
        if (sscanf(s, "%u:%u", &m, &sec) != 2 || sec > 59) return false;
        *out_s = m * 60 + sec;
    } else {
        if (sscanf(s, "%u", &sec) != 1) return false;
        *out_s = sec;
    }
    return *out_s >= 1 && *out_s <= 5999;
}

static int cmd_timer(int argc, char **argv)
{
    if (argc < 2) {
        uint32_t rem, tot;
        int st;
        whm_timer_status(&rem, &tot, &st);
        const char *names[] = {"idle", "running", "paused", "DONE"};
        uint32_t def = whm_timer_get_default();
        printf("timer:   %02lu:%02lu of %02lu:%02lu (%s)\n",
               (unsigned long)(rem / 60), (unsigned long)(rem % 60),
               (unsigned long)(tot / 60), (unsigned long)(tot % 60),
               names[st & 3]);
        printf("default: %02lu:%02lu (NVS; 'timer default <v>' changes it)\n",
               (unsigned long)(def / 60), (unsigned long)(def % 60));
        printf("usage:   timer start|stop|pause|reset|set <mm:ss|s>|"
               "default <mm:ss|s>\n");
        printf("         stop keeps a custom duration; reset restores "
               "the default\n");
        return 0;
    }
    if (strcmp(argv[1], "stop") == 0) { whm_timer_stop(); return 0; }
    if (strcmp(argv[1], "start") == 0) { whm_timer_start(); whm_ui_screen_set("timer"); return 0; }
    if (strcmp(argv[1], "pause") == 0) { whm_timer_pause(); return 0; }
    if (strcmp(argv[1], "reset") == 0) { whm_timer_reset(); return 0; }
    uint32_t v;
    if (strcmp(argv[1], "set") == 0 && argc >= 3 && parse_duration(argv[2], &v)) {
        whm_timer_set(v);
        whm_ui_screen_set("timer");
        printf("timer set to %02lu:%02lu (one-off)\n",
               (unsigned long)(v / 60), (unsigned long)(v % 60));
        return 0;
    }
    if (strcmp(argv[1], "default") == 0 && argc >= 3 && parse_duration(argv[2], &v)) {
        whm_timer_set_default(v);
        printf("default timer: %02lu:%02lu (saved)\n",
               (unsigned long)(v / 60), (unsigned long)(v % 60));
        return 0;
    }
    printf("usage: timer [start|pause|reset|set <mm:ss|s>|default <mm:ss|s>]\n");
    return 1;
}

static int cmd_stopwatch(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        bool run;
        int64_t el = whm_watch_elapsed_us(&run);
        uint32_t cs = (uint32_t)(el / 10000);
        printf("stopwatch: %02lu:%02lu.%02lu (%s)\n",
               (unsigned long)(cs / 6000), (unsigned long)((cs / 100) % 60),
               (unsigned long)(cs % 100), run ? "running" : "stopped");
        printf("usage:     stopwatch start|stop|reset\n");
        return 0;
    }
    if (strcmp(argv[1], "start") == 0) { whm_watch_start(); whm_ui_screen_set("stopwatch"); return 0; }
    if (strcmp(argv[1], "stop") == 0) { whm_watch_stop(); return 0; }
    if (strcmp(argv[1], "reset") == 0) { whm_watch_reset(); return 0; }
    printf("usage: stopwatch [start|stop|reset]\n");
    return 1;
}

/* ----------------------------------------------------------------- text */

static int cmd_text(int argc, char **argv)
{
    bool span = false, over = false, band = false;
    uint8_t size = 0, scroll = 0, pos = 2;
    uint16_t speed = 30;
    int fi = 1;
    for (; fi < argc && strncmp(argv[fi], "--", 2) == 0; fi++) {
        if (strcmp(argv[fi], "--span") == 0) span = true;
        else if (strcmp(argv[fi], "--over") == 0) over = true;
        else if (strcmp(argv[fi], "--band") == 0) band = true;
        else if (strcmp(argv[fi], "--pos") == 0 && fi + 1 < argc) {
            const char *v = argv[++fi];
            pos = strcmp(v, "top") == 0 ? 0
                : strcmp(v, "mid") == 0 ? 1 : 2;
        }
        else if (strcmp(argv[fi], "--size") == 0 && fi + 1 < argc) {
            const char *v = argv[++fi];
            size = strcmp(v, "auto") == 0 ? 0 : (uint8_t)atoi(v);
            if (size > 2) size = 0;
        } else if (strcmp(argv[fi], "--scroll") == 0 && fi + 1 < argc) {
            const char *v = argv[++fi];
            scroll = strcmp(v, "off") == 0 ? 1
                   : strcmp(v, "read") == 0 ? 2
                   : strcmp(v, "bounce") == 0 ? 3
                   : strcmp(v, "loop") == 0 ? 4 : 0;
        } else if (strcmp(argv[fi], "--speed") == 0 && fi + 1 < argc) {
            int v = atoi(argv[++fi]);
            if (v >= 5 && v <= 120) speed = (uint16_t)v;
        } else {
            printf("usage: text [--span] [--size 1|2|auto] "
                   "[--scroll auto|off|read|bounce|loop] [--speed n] "
                   "<msg>\n");
            printf("  local by default; 'fleet text ...' mirrors; "
                   "'fleet text --span ...' spans the strip\n");
            return 1;
        }
    }
    whm_ui_text_cfg(span, size, scroll, speed);
    argc -= (fi - 1);
    argv += (fi - 1);
    if (argc < 2) {
        printf("usage: text [--flags] <message>  ('text clear' exits)\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        if (whm_ui_overlay_active()) {
            whm_ui_overlay(NULL, 0, 0, 0, 0, false, 0);
            printf("overlay cleared\n");
        } else {
            whm_ui_text_clear();
        }
        return 0;
    }
    char msg[128] = "";
    size_t n = 0;
    for (int i = 1; i < argc && n + 1 < sizeof(msg); i++) {
        n += (size_t)snprintf(msg + n, sizeof(msg) - n, "%s%s",
                              i > 1 ? " " : "", argv[i]);
    }
    if (over) {
        whm_ui_overlay(msg, span, size, scroll, speed, band, pos);
        printf("overlay on (rides above any screen/pattern) - "
               "'text clear' removes\n");
        return 0;
    }
    whm_ui_show_text(msg);
    printf("showing on panel - BOOT button or 'text clear' to exit\n");
    return 0;
}

/* ------------------------------------------------------------- registry */

typedef struct {
    const char *cmd;
    const char *syntax;
    const char *desc;
    esp_console_cmd_func_t fn;
} cmd_ent_t;

static int cmd_help(int argc, char **argv);

static const cmd_ent_t k_cmds[] = {
    { "help",       "help [cmd]",                     "this list",                     cmd_help },
    { "wifi",       "wifi status|scan|join <ssid> [pw]|ap [ssid] [pw]|clear",
                                                      "wifi control (NVS)",            cmd_wifi },
    { "tsf",        "tsf",                            "TSF clock + drift ppm",         cmd_tsf },
    { "sensors",    "sensors",                        "IMU / temp+RH / RTC one-shot",  cmd_sensors },
    { "rtc",        "rtc [set Y-M-D H:M:S]",          "hardware clock (local time)",   cmd_rtc },
    { "ntp",        "ntp [status|on|off|server <h>|interval <m>|sync]",
                                                      "SNTP -> RTC discipline",        cmd_ntp },
    { "tz",         "tz [posix-string|clear]",        "timezone for RTC/clock",        cmd_tz },
    { "sysinfo",    "sysinfo",                        "heap, uptime, versions",        cmd_sysinfo },
    { "sd",         "sd info|mount|unmount|bench|ls|format",
                                                      "card ops; format = full-card, confirmed", cmd_sd },
    { "audio",      "audio",                          "tone + mic RMS test",           cmd_audio },
    { "brightness", "brightness <0-255>",             "live + saved",                  cmd_brightness },
    { "screen",     "screen <name>|next|back|auto|list|bootmode",
                                                      "info screens",                  cmd_screen },
    { "pattern",    "pattern <name|off> (walker, wander, ...)",             "validation patterns",           cmd_pattern },
    { "name",       "name [id|clear]",                "node identity (prompt, SSID)",  cmd_name },
    { "ota",        "ota <host|url> | status",        "over-the-air update (staged)",  cmd_ota },
    { "factory",    "factory confirm",                "erase all settings + reboot",   cmd_factory },
    { "oracle",     "oracle",                         "consult the eight ball",        cmd_oracle },
    { "fleet",      "fleet [@node] <console line>",   "run a command fleet-wide or on one node",   cmd_fleet },
    { "clock",      "clock label <text|tz|off>",      "label above the clock face",    cmd_clock },
    { "vol",        "vol [1-100]",                    "beep/chime/tone volume",        cmd_vol },
        { "walk",       "walk [speed <-2..2>]",           "world scroll: 0 pause, - reverse",  cmd_walk },
{ "fw",         "",                               "",                                  cmd_fw },  /* hidden */
    { "chime",      "chime",                          "play the boot chime",           cmd_chime },
    { "tone",       "tone <hz> [ms]",                 "play one note",                 cmd_tone },
    { "wander",     "wander <idx> <n> [spd] | go",    "cross-panel sprite (P2b)",      cmd_wander },
    { "sync",       "sync [auto|anchor|follow|prio|lead|media|conduct|join|off]", "fleet link (WHM-LINK.md)",      cmd_sync },
    { "ble",        "ble [pair [secs]|off]",          "wh-link BLE (pair window/status)", cmd_ble },
    { "life",       "life [pal|next|reset]",          "game-of-life colors",           cmd_life },
    { "mp3",        "mp3 list|play|fleet|dj [n|stop]|layout|overlay|vol",
                                                      "music from /sdcard/media",      cmd_mp3 },
    { "timer",      "timer [start|stop|pause|reset|set|default]",
                                                      "countdown; beeps + grabs screen", cmd_timer },
    { "stopwatch",  "stopwatch [start|stop|reset]",   "no-arg = status",               cmd_stopwatch },
    { "text",       "text [--over --band --pos|--span|--size|--scroll] <msg>|clear",        "show a message on the panel",   cmd_text },
    { "log",        "log error|warn|info|debug",      "background log level",          cmd_log },
    { "reboot",     "reboot",                         "restart the board",             cmd_reboot },
};
#define N_CMDS (sizeof(k_cmds) / sizeof(k_cmds[0]))

static int cmd_help(int argc, char **argv)
{
    if (argc >= 2) {
        for (size_t i = 0; i < N_CMDS; i++) {
        if (!k_cmds[i].syntax || !k_cmds[i].syntax[0]) continue;
                                     /* empty syntax = hidden command */
            if (strcmp(argv[1], k_cmds[i].cmd) == 0) {
                printf("%s\n  %s\n",
                       k_cmds[i].syntax ? k_cmds[i].syntax : "",
                       k_cmds[i].desc ? k_cmds[i].desc : "");
                return 0;
            }
        }
        printf("unknown command '%s'\n", argv[1]);
        return 1;
    }
    printf("commands:\n");
    for (size_t i = 0; i < N_CMDS; i++) {
        if (strlen(k_cmds[i].syntax) <= 33) {
            printf("  %-33s %s\n", k_cmds[i].syntax,
                   k_cmds[i].desc ? k_cmds[i].desc : "");
        } else {
            printf("  %s\n  %33s %s\n", k_cmds[i].syntax, "",
                   k_cmds[i].desc ? k_cmds[i].desc : "");
        }
    }
    printf("(up/down = history, ctrl-c = clear line; logs are quiet - "
           "'log debug' streams telemetry)\n");
    return 0;
}

/* ------------------------------------------------------------ line editor
 *
 * In-house replacement for linenoise's editing loop. Rationale (verified in
 * IDF source): linenoiseEdit() calls getColumns() on EVERY prompt, which
 * issues two ESC[6n cursor-position queries plus cursor jumps; under
 * forwarding terminals (miniterm) the replies race user keystrokes and the
 * parser eats typed characters. No API disables it short of dumb mode,
 * which loses history. This editor interrogates the terminal never:
 * up/down history (16 deep), backspace, ctrl-c, fixed behavior on any
 * transport. Tab-complete is deliberately dropped for now (roadmap).
 */

#define HIST_N 16
#define WHM_LINE_MAX 256

static const char *s_prompt = "whm> ";

/* Pure-ASCII redraw - no ANSI in either direction (some forwarding
 * terminals print device-side escapes literally, so ESC[K never erased and
 * long->short history recalls left tails on screen). We track how many
 * characters the current line occupies; a redraw reprints, blanks the
 * delta with spaces, then backspaces the cursor home. Caveat: backspace
 * does not cross a terminal line-wrap, so lines longer than the terminal
 * width may misalign until Enter. */
static size_t s_rendered = 0;

/* Full-line redraw with the cursor left at column pos - still zero ANSI:
 * reprint, blank the delta, then walk backspaces from the rightmost
 * column reached to the cursor. */
static void redraw_line(const char *line, int len, int pos)
{
    size_t pl = strlen(s_prompt);
    size_t cur = pl + (size_t)len;
    printf("\r%s%s", s_prompt, line);
    size_t endcol = cur;
    for (size_t i = cur; i < s_rendered; i++) { putchar(' '); endcol++; }
    for (size_t i = endcol; i > pl + (size_t)pos; i--) putchar('\b');
    s_rendered = cur;
    fflush(stdout);
}

static void put_prompt_line(const char *line)
{
    redraw_line(line, (int)strlen(line), (int)strlen(line));
}

static void whm_repl_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "REPL task running (USB-Serial-JTAG)");
    printf("\n==== WHM console - type 'help' ====\n");
    static char line[WHM_LINE_MAX];
    static char hist[HIST_N][WHM_LINE_MAX];
    static char stash[WHM_LINE_MAX];
    int len = 0, pos = 0, h_count = 0, h_head = 0, h_browse = -1;

    printf("\nWHM console - up/down for history, ctrl-c clears the line, "
           "'help' lists commands\n");
    put_prompt_line("");

    while (1) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            static int eofs = 0;
            if (++eofs == 5) {
                ESP_LOGE(TAG, "stdin EOF storm - USJ read path dead?");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (ch == 0x1b) {                       /* escape sequence */
            int c1 = fgetc(stdin);
            if (c1 != '[') continue;            /* bare ESC or alt-key: drop */
            int fin;
            do { fin = fgetc(stdin); } while (fin >= '0' && fin <= '9') ;
            while (fin == ';') { do { fin = fgetc(stdin); } while (fin >= '0' && fin <= '9'); }
            if (fin == 'A') {                   /* up */
                if (h_count == 0) continue;
                if (h_browse == -1) {
                    strlcpy(stash, line, sizeof(stash));
                    h_browse = 0;
                } else if (h_browse < h_count - 1) {
                    h_browse++;
                } else {
                    continue;
                }
                strlcpy(line, hist[(h_head - 1 - h_browse + HIST_N) % HIST_N],
                        sizeof(line));
                len = strlen(line);
                pos = len;
                put_prompt_line(line);
            } else if (fin == 'B') {            /* down */
                if (h_browse < 0) continue;
                h_browse--;
                if (h_browse < 0) strlcpy(line, stash, sizeof(line));
                else strlcpy(line,
                             hist[(h_head - 1 - h_browse + HIST_N) % HIST_N],
                             sizeof(line));
                len = strlen(line);
                pos = len;
                put_prompt_line(line);
            }
            else if (fin == 'C') {              /* right */
                if (pos < len) { putchar(line[pos]); pos++; fflush(stdout); }
            } else if (fin == 'D') {            /* left */
                if (pos > 0) { putchar('\b'); pos--; fflush(stdout); }
            } else if (fin == 'H') {            /* home */
                while (pos > 0) { putchar('\b'); pos--; }
                fflush(stdout);
            } else if (fin == 'F') {            /* end */
                while (pos < len) { putchar(line[pos]); pos++; }
                fflush(stdout);
            } else if (fin == '~') {            /* delete-forward (ESC[3~) */
                if (pos < len) {
                    memmove(line + pos, line + pos + 1, (size_t)(len - pos));
                    len--;
                    redraw_line(line, len, pos);
                }
            }
            continue;
        }

        if (ch == 0x7f || ch == 0x08) {         /* backspace */
            if (pos > 0) {
                memmove(line + pos - 1, line + pos, (size_t)(len - pos) + 1);
                len--;
                pos--;
                if (pos == len) {               /* fast path at end */
                    printf("\b \b");
                    if (s_rendered) s_rendered--;
                    fflush(stdout);
                } else {
                    redraw_line(line, len, pos);
                }
            }
            continue;
        }

        if (ch == 0x03) {                       /* ctrl-c */
            printf("^C\n");
            len = 0; pos = 0; line[0] = 0; h_browse = -1;
            s_rendered = 0;                     /* fresh row */
            put_prompt_line("");
            continue;
        }

        if (ch == '\n' || ch == '\r') {
            printf("\n");
            fflush(stdout);
            if (len > 0) {
                int last = (h_head - 1 + HIST_N) % HIST_N;
                if (h_count == 0 || strcmp(hist[last], line) != 0) {
                    strlcpy(hist[h_head], line, sizeof(hist[0]));
                    h_head = (h_head + 1) % HIST_N;
                    if (h_count < HIST_N) h_count++;
                }
                int ret = 0;
                /* shell-style chaining: ; && || at top level (quote-
                 * aware). ';' always runs the next segment; '&&' only on
                 * success (ret==0, no error); '||' only on failure. */
                esp_err_t err = ESP_OK;
                {
                    char *seg = line;
                    char op_prev = ';';
                    int last_rc = 0;
                    esp_err_t last_err = ESP_OK;
                    char *p = line;
                    bool q = false;
                    bool done_any = false;
                    for (;; p++) {
                        char ch2 = *p;
                        char op_next = 0;
                        int oplen = 0;
                        if (ch2 == '"') q = !q;
                        if (!q) {
                            if (ch2 == ';') { op_next = ';'; oplen = 1; }
                            else if (ch2 == '&' && p[1] == '&') {
                                op_next = 'A'; oplen = 2;
                            } else if (ch2 == '|' && p[1] == '|') {
                                op_next = 'O'; oplen = 2;
                            }
                        }
                        if (op_next || ch2 == 0) {
                            char saved = *p;
                            *p = 0;
                            char *s2 = seg;
                            while (*s2 == ' ') s2++;
                            bool ok_prev = (last_err == ESP_OK &&
                                            last_rc == 0);
                            bool run = (op_prev == ';') ||
                                       (op_prev == 'A' && ok_prev) ||
                                       (op_prev == 'O' && !ok_prev);
                            if (*s2 && run) {
                                last_err = esp_console_run(s2, &ret);
                                last_rc = ret;
                                done_any = true;
                                if (last_err == ESP_ERR_NOT_FOUND) {
                                    printf("unknown command - 'help' "
                                           "lists them\n");
                                }
                            } else if (*s2 && !run) {
                                last_rc = last_rc;   /* skipped: rc holds */
                            }
                            *p = saved;
                            if (ch2 == 0) break;
                            op_prev = op_next;
                            seg = p + oplen;
                            p += oplen - 1;
                        }
                    }
                    ret = last_rc;
                    /* segment loop already reported NOT_FOUND */
                    err = (last_err == ESP_ERR_NOT_FOUND) ? ESP_OK
                                                          : last_err;
                    (void)done_any;
                }
                if (err == ESP_ERR_NOT_FOUND) {
                    printf("unknown command - 'help' lists them\n");
                } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
                    printf("console error: %s\n", esp_err_to_name(err));
                }
            }
            len = 0; pos = 0; line[0] = 0; h_browse = -1;
            s_rendered = 0;                     /* fresh row */
            put_prompt_line("");
            continue;
        }

        if (ch == 0x01) {                       /* ctrl-a: home */
            while (pos > 0) { putchar('\b'); pos--; }
            fflush(stdout);
            continue;
        }
        if (ch == 0x05) {                       /* ctrl-e: end */
            while (pos < len) { putchar(line[pos]); pos++; }
            fflush(stdout);
            continue;
        }
        if (ch == '\t') continue;              /* no completion v1 */

        if (ch >= 0x20 && ch <= 0x7e && len < WHM_LINE_MAX - 1) {
            if (pos == len) {                   /* fast path at end */
                line[len++] = (char)ch;
                line[len] = 0;
                pos = len;
                putchar(ch);
                s_rendered++;
                fflush(stdout);
            } else {
                memmove(line + pos + 1, line + pos, (size_t)(len - pos) + 1);
                line[pos] = (char)ch;
                len++;
                pos++;
                redraw_line(line, len, pos);
            }
        }
    }
}

static char s_prompt_store[24] = "whm> ";

static void prompt_refresh(void)
{
    char node[17] = "";
    whm_settings_get_str("node", node, sizeof(node));
    snprintf(s_prompt_store, sizeof(s_prompt_store), "%s> ",
             node[0] ? node : "whm");
}

esp_err_t whm_console_start(void)
{
    prompt_refresh();
    s_prompt = s_prompt_store;

    esp_console_config_t ccfg = ESP_CONSOLE_CONFIG_DEFAULT();
    ccfg.max_cmdline_length = WHM_LINE_MAX;
    ccfg.max_cmdline_args = 12;
    ESP_RETURN_ON_ERROR(esp_console_init(&ccfg), TAG, "console init");

    for (size_t i = 0; i < N_CMDS; i++) {
        const esp_console_cmd_t cc = {
            .command = k_cmds[i].cmd,
            .help = k_cmds[i].desc,
            .func = k_cmds[i].fn,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cc));
    }

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_driver_config_t dcfg = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 512,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&dcfg), TAG, "usj drv");
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);
#else
#error "WHM console requires USB-Serial-JTAG (UART0 pins are I2S/SD)"
#endif

    BaseType_t trc = xTaskCreatePinnedToCore(whm_repl_task, "whm_repl",
                                             8192, NULL, 4, NULL, 0);
    if (trc != pdPASS) {
        ESP_LOGE(TAG, "REPL task creation FAILED (rc=%d) - no console",
                 (int)trc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "console ready (in-house editor: history/arrows, no "
                  "terminal probing)");
    return ESP_OK;
}
