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
#include "mon_pins.h"
#include "esp_heap_caps.h"
#include "mon_wifi.h"
#include "mon_parse.h"
#include "mon_lcd.h"
#include "mon_http.h"
#include "mbedtls/base64.h"
#include <stdarg.h>
#define LN 96
#define HIST 8
static char s_hist[HIST][LN]; static int s_hn, s_hview;
static char s_buf[LN]; static int s_len, s_cur, s_started;
static void redraw(void)
{
    printf("\r\x1b[KMon> %s", s_buf);
    for (int i = s_len; i > s_cur; i--) printf("\b");
    fflush(stdout);
}
static void con_write_line(const char *msg)
{
    if (!s_started) { printf("%s", msg); return; }
    printf("\r\x1b[K%s", msg);
    size_t l = strlen(msg);
    if (!l || msg[l - 1] != '\n') printf("\n");
    redraw();
}
static int mon_con_vlog(const char *fmt, va_list ap)
{
    /* THE SYSTEMIC CURE: every ESP-IDF log (wifi driver
       included) now flows through the repaint-safe path, so
       no async line can shred the half-typed command. */
    char tmp[200];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    con_write_line(tmp);
    return n;
}
void mon_con_printf(const char *fmt, ...)
{
    /* repaint-safe async print: clears the edit line, prints,
       then redraws the half-typed command underneath - so the
       [M] stream can no longer shred line editing. */
    char tmp[160];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    con_write_line(tmp);
}
static int tokenize(char *s, char **argv, int maxv)
{
    /* quotes + backslash escapes, in-place: SSIDs with spaces
       work as "Sloth Country Manor" or Sloth\ Country\ Manor */
    int argc = 0; char *w = s;
    while (*s && argc < maxv) {
        while (*s == ' ') s++;
        if (!*s) break;
        argv[argc++] = w;
        int q = 0;
        while (*s) {
            char ch = *s;
            if (ch == '\\' && s[1]) { s++; *w++ = *s++; continue; }
            if (ch == '"') { q = !q; s++; continue; }
            if (ch == ' ' && !q) break;
            *w++ = ch; s++;
        }
        if (*s) s++;
        *w++ = 0;
    }
    return argc;
}
static void exec_line(char *line)
{
    char *argv[6];
    int argc = tokenize(line, argv, 6);
    if (!argc) return;
    if (!strcmp(argv[0], "version")) {
        printf("%s  board=%s\n", MON_VERSION, MON_BOARD_NAME);
    } else if (!strcmp(argv[0], "help")) {
        printf("version        which firmware is this port?\n"
               "wifi join <ssid> [pw] | wifi clear | wifi status\n"
               "snap           framebuffer -> base64 (see tools/)\n"
               "tz <+-min>     local-time offset for daylight\n"
               "mon            live snapshot\n"
               "stats          wire counters\n"
               "bright <0-255> panel brightness (saved)\n"
               "reboot\n");
    } else if (!strcmp(argv[0], "wifi") && argc == 1) {
        printf("wifi join <ssid> [pw]   (quote or \\-escape "
               "spaces)\nwifi clear | wifi status\n");
    } else if (!strcmp(argv[0], "wifi") && argc >= 2) {
        if (!strcmp(argv[1], "join") && argc >= 3)
            printf(mon_wifi_join(argv[2], argc >= 4 ? argv[3] : "")
                   == ESP_OK ? "joining \"%s\" (saved)\n"
                             : "join failed\n", argv[2]);
        else if (!strcmp(argv[1], "clear")) mon_wifi_clear();
        else if (!strcmp(argv[1], "status")) mon_wifi_status();
        else printf("wifi join|clear|status\n");
    } else if (!strcmp(argv[0], "mon") && argc == 2 &&
               !strcmp(argv[1], "stream")) {
        extern volatile int g_mon_stream;
        g_mon_stream = !g_mon_stream;
        printf("[M] stream %s\n", g_mon_stream ? "on" : "off");
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
            /* unswap to LE for the decoder (fb is
               pre-swapped since 0.9.0) - and the hardcoded
               536x240 was a latent multi-board bug. */
            static uint16_t *s_le;
            if (!s_le) s_le = heap_caps_malloc(
                LCD_W * LCD_H * 2, MALLOC_CAP_SPIRAM);
            {
                const uint16_t *q = mon_snap_buf();
                for (int i = 0; i < LCD_W * LCD_H; i++)
                    s_le[i] = __builtin_bswap16(q[i]);
            }
            const uint8_t *p = (const uint8_t *)s_le;
            size_t total = (size_t)LCD_W * LCD_H * 2, done = 0;
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
    int esc = 0;
    s_started = 1;
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
                if (s_hview == s_hn) s_buf[0] = 0;
                else strlcpy(s_buf, s_hist[s_hview], LN);
                s_len = s_cur = strlen(s_buf);
                redraw();
            } else if (c == 'D' && s_cur > 0) { s_cur--;
                redraw();
            } else if (c == 'C' && s_cur < s_len) { s_cur++;
                redraw();
            }
            continue;
        }
        if (c == 0x1b) { esc = 1; continue; }
        if (c == '\r' || c == '\n') {
            printf("\n");
            if (s_len) {
                if (s_hn == HIST) {
                    memmove(s_hist[0], s_hist[1],
                            (HIST - 1) * LN);
                    s_hn--;
                }
                strlcpy(s_hist[s_hn++], s_buf, LN);
                char tmp[LN]; strlcpy(tmp, s_buf, LN);
                exec_line(tmp);
            }
            s_len = s_cur = 0; s_buf[0] = 0; s_hview = s_hn;
            printf("Mon> "); fflush(stdout);
        } else if (c == 0x7f || c == 0x08) {
            if (s_cur > 0) {
                memmove(s_buf + s_cur - 1, s_buf + s_cur, s_len - s_cur + 1);
                s_len--; s_cur--; redraw();
            }
        } else if (c == 0x03) {                   /* ctrl-c */
            s_len = s_cur = 0; s_buf[0] = 0; s_hview = s_hn;
            printf("\nMon> "); fflush(stdout);
        } else if (c >= 0x20 && c < 0x7f && s_len < LN - 1) {
            memmove(s_buf + s_cur + 1, s_buf + s_cur, s_len - s_cur + 1);
            s_buf[s_cur++] = (char)c; s_len++;
            redraw();
        }
    }
}
#include "esp_log.h"
void mon_con_start(void)
{
    esp_log_set_vprintf(mon_con_vlog);
    usb_serial_jtag_driver_config_t cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    xTaskCreate(con_task, "mon_con", 4096, NULL, 5, NULL);
}
