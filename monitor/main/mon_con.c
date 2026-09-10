/* Lean monitor console - the fleet's UX contract (in-house editor:
 * history, arrows, no terminal probing) without its 2,000 fleet
 * commands. USB-Serial-JTAG. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_system.h"
#include "nvs.h"
#include "mon_config.h"
#include "mon_wifi.h"
#include "mon_parse.h"
#include "mon_lcd.h"
#include "mon_http.h"
#include "mbedtls/base64.h"
#define LN 96
#define HIST 8
static char s_hist[HIST][LN]; static int s_hn, s_hview;
static void redraw(const char *buf, int len, int cur)
{
    printf("\r\x1b[KMon> %s", buf);
    for (int i = len; i > cur; i--) printf("\b");
    fflush(stdout);
}
static void exec_line(char *line)
{
    char *argv[6]; int argc = 0;
    for (char *t = strtok(line, " "); t && argc < 6;
         t = strtok(NULL, " ")) argv[argc++] = t;
    if (!argc) return;
    if (!strcmp(argv[0], "help")) {
        printf("wifi join <ssid> [pw] | wifi clear | wifi status\n"
               "snap           framebuffer -> base64 (see tools/)\n"
               "tz <+-min>     local-time offset for daylight\n"
               "mon            live snapshot\n"
               "stats          wire counters\n"
               "bright <0-255> panel brightness (saved)\n"
               "reboot\n");
    } else if (!strcmp(argv[0], "wifi") && argc >= 2) {
        if (!strcmp(argv[1], "join") && argc >= 3)
            printf(mon_wifi_join(argv[2], argc >= 4 ? argv[3] : "")
                   == ESP_OK ? "joining \"%s\" (saved)\n"
                             : "join failed\n", argv[2]);
        else if (!strcmp(argv[1], "clear")) mon_wifi_clear();
        else if (!strcmp(argv[1], "status")) mon_wifi_status();
        else printf("wifi join|clear|status\n");
    } else if (!strcmp(argv[0], "mon")) {
        printf("step=%lu st=%s own=%u x=%.1f y=%.1f cam=%.1f "
               "rung@%lu from=%s\n",
               (unsigned long)g_mon.step, mon_st_name(g_mon.st),
               g_mon.owner, g_mon.x, g_mon.y, g_mon.cam,
               (unsigned long)g_mon.rung_step, g_mon.from);
    } else if (!strcmp(argv[0], "stats")) {
        printf("kf=%lu wkp=%lu org=%lu drop=%lu\n",
               (unsigned long)g_mon.n_kf, (unsigned long)g_mon.n_wkp,
               (unsigned long)g_mon.n_org,
               (unsigned long)g_mon.n_drop);
    } else if (!strcmp(argv[0], "bright") && argc == 2) {
        int v = atoi(argv[1]);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        mon_lcd_brightness((uint8_t)v);
        nvs_handle_t h;
        if (nvs_open("mon", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "bright", (uint8_t)v);
            nvs_commit(h); nvs_close(h);
        }
        printf("brightness %d (saved)\n", v);
    } else if (!strcmp(argv[0], "snap")) {
        if (mon_snap_take() != 0) { printf("snap timeout\n"); }
        else {
            const uint8_t *p = (const uint8_t *)mon_snap_buf();
            size_t total = 536 * 240 * 2, done = 0;
            printf("-----SNAP BEGIN 536x240 RGB565-----\n");
            unsigned char line[97]; size_t ol;
            while (done < total) {
                size_t take = total - done;
                if (take > 72) take = 72;   /* 72B -> 96 b64 chars */
                mbedtls_base64_encode(line, sizeof line, &ol,
                                      p + done, take);
                line[ol] = 0; printf("%s\n", line);
                done += take;
            }
            printf("-----SNAP END-----\n");
        }
    } else if (!strcmp(argv[0], "tz") && argc == 2) {
        int mins = atoi(argv[1]);
        nvs_handle_t h;
        if (nvs_open("mon", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "tz_min", mins);
            nvs_commit(h); nvs_close(h);
        }
        printf("tz offset %+d min (saved; takes effect now)\n",
               mins);
    } else if (!strcmp(argv[0], "reboot")) {
        esp_restart();
    } else {
        printf("? 'help'\n");
    }
}
static void con_task(void *arg)
{
    char buf[LN] = ""; int len = 0, cur = 0, esc = 0;
    printf("\n==== WHM monitor console - 'help' ====\nMon> ");
    fflush(stdout);
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1,
                pdMS_TO_TICKS(100)) != 1) continue;
        if (esc == 1) { esc = (c == '[') ? 2 : 0; continue; }
        if (esc == 2) { esc = 0;
            if (c == 'A' || c == 'B') {          /* history */
                if (!s_hn) continue;
                s_hview += (c == 'A') ? -1 : 1;
                if (s_hview < 0) s_hview = 0;
                if (s_hview > s_hn) s_hview = s_hn;
                if (s_hview == s_hn) buf[0] = 0;
                else strlcpy(buf, s_hist[s_hview], LN);
                len = cur = strlen(buf);
                redraw(buf, len, cur);
            } else if (c == 'D' && cur > 0) { cur--;
                redraw(buf, len, cur);
            } else if (c == 'C' && cur < len) { cur++;
                redraw(buf, len, cur);
            }
            continue;
        }
        if (c == 0x1b) { esc = 1; continue; }
        if (c == '\r' || c == '\n') {
            printf("\n");
            if (len) {
                if (s_hn == HIST) {
                    memmove(s_hist[0], s_hist[1],
                            (HIST - 1) * LN);
                    s_hn--;
                }
                strlcpy(s_hist[s_hn++], buf, LN);
                char tmp[LN]; strlcpy(tmp, buf, LN);
                exec_line(tmp);
            }
            len = cur = 0; buf[0] = 0; s_hview = s_hn;
            printf("Mon> "); fflush(stdout);
        } else if (c == 0x7f || c == 0x08) {
            if (cur > 0) {
                memmove(buf + cur - 1, buf + cur, len - cur + 1);
                len--; cur--; redraw(buf, len, cur);
            }
        } else if (c == 0x03) {                   /* ctrl-c */
            len = cur = 0; buf[0] = 0; s_hview = s_hn;
            printf("\nMon> "); fflush(stdout);
        } else if (c >= 0x20 && c < 0x7f && len < LN - 1) {
            memmove(buf + cur + 1, buf + cur, len - cur + 1);
            buf[cur++] = (char)c; len++;
            redraw(buf, len, cur);
        }
    }
}
void mon_con_start(void)
{
    usb_serial_jtag_driver_config_t cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    xTaskCreate(con_task, "mon_con", 4096, NULL, 5, NULL);
}
