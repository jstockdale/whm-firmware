/* The pages: STATS and WIRE, fullscreen. Tap cycles
 * WORLD -> FULL -> STATS -> WIRE -> WORLD. */
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "mon_pins.h"
#include "mon_parse.h"
#include "mon_wifi.h"
#include "gfx5x7.h"
#include "display_hal.h"
static void spark(int x0, int y0, int w, int h,
                  const uint8_t *ring, uint8_t idx,
                  uint8_t r, uint8_t g, uint8_t b)
{
    whm_display_fill_rect(x0, y0 + h, w, 1, 60, 68, 84);
    uint8_t mx = 1;
    for (int i = 0; i < 120; i++)
        if (ring[i] > mx) mx = ring[i];
    for (int i = 0; i < 120 && i * 4 < w; i++) {
        uint8_t v = ring[(idx + i) % 120];
        int bh = (int)v * h / mx;
        if (bh > 0)
            whm_display_fill_rect(x0 + i * 4, y0 + h - bh,
                                  3, bh, r, g, b);
    }
}
void mon_page_stats(uint32_t kfs)
{
    char ln[56];
    uint8_t oc_r = g_mon.owner ? 255 : 65,
            oc_g = g_mon.owner ? 92 : 208,
            oc_b = g_mon.owner ? 200 : 255;
    gfx_text(10, 8, "STATS", 2, 110, 120, 138);
    gfx_text(10, 34, (char *)mon_st_name(g_mon.st), 4,
             oc_r, oc_g, oc_b);
    snprintf(ln, sizeof ln, "step %lu",
             (unsigned long)g_mon.step);
    gfx_text(10, 74, ln, 3, 232, 232, 240);
    snprintf(ln, sizeof ln, "x %.1f  y %.1f", g_mon.x, g_mon.y);
    gfx_text(10, 106, ln, 2, 180, 190, 205);
    snprintf(ln, sizeof ln, "vx %+.2f vy %+.2f spd %.2f",
             g_mon.vx, g_mon.vy, g_mon.spd);
    gfx_text(10, 128, ln, 2, 180, 190, 205);
    snprintf(ln, sizeof ln, "cam %.1f   strip %u   %s",
             g_mon.cam, g_mon.owner, g_mon.from);
    gfx_text(10, 150, ln, 2, 160, 170, 185);
    int rssi; char ip[20];
    if (mon_wifi_info(&rssi, ip, sizeof ip) == 0)
        snprintf(ln, sizeof ln, "wifi %d dBm  %s", rssi, ip);
    else snprintf(ln, sizeof ln, "wifi: not connected");
    gfx_text(10, 172, ln, 2, 140, 150, 168);
    snprintf(ln, sizeof ln, "kf/s %lu   up %llus",
             (unsigned long)kfs,
             (unsigned long long)(esp_timer_get_time() / 1000000));
    gfx_text(10, 194, ln, 2, 140, 150, 168);
    gfx_text(LCD_W - 236, 150, "kf/s (2 min)", 1, 110, 120, 138);
    spark(LCD_W - 236, 162, 230, 56, g_mon.kfs_ring, g_mon.ring_i,
          oc_r, oc_g, oc_b);
}
void mon_page_wire(void)
{
    char ln[56];
    gfx_text(10, 8, "WIRE", 2, 110, 120, 138);
    {
        int64_t age9 = g_mon.last_kf_us
            ? (esp_timer_get_time() - g_mon.last_kf_us) / 1000
            : -1;
        snprintf(ln, sizeof ln, "age %lld ms", (long long)age9);
        gfx_text(160, 8, ln, 2, 232, 180, 100);
    }
    snprintf(ln, sizeof ln,
             "rx9 %lu  acc %lu  d:%lu/%lu/%lu/%lu",
             (unsigned long)g_mon.n_rx9,
             (unsigned long)g_mon.n_kf,
             (unsigned long)g_mon.dh[0], (unsigned long)g_mon.dh[1],
             (unsigned long)g_mon.dh[2], (unsigned long)g_mon.dh[3]);
    gfx_text(10, 148, ln, 1, 232, 180, 100);
    {
        char r2[52]; int o2 = 0;
        o2 += snprintf(r2 + o2, sizeof r2 - o2, "raw ");
        for (int i = 0; i < 8; i++)
            o2 += snprintf(r2 + o2, (size_t)(sizeof r2 - o2),
                "%lu ", (unsigned long)
                (g_mon.raw8[(g_mon.raw8_i + i) % 8] % 1000));
        gfx_text(10, 160, r2, 1, 232, 180, 100);
    }
    {
        char r2[52]; int o2 = 0;
        o2 += snprintf(r2 + o2, sizeof r2 - o2, "drw ");
        for (int i = 0; i < 8; i++)
            o2 += snprintf(r2 + o2, (size_t)(sizeof r2 - o2),
                "%lu ", (unsigned long)
                (g_mon.drawn8[(g_mon.drawn8_i + i) % 8] % 1000));
        gfx_text(10, 172, r2, 1, 124, 224, 201);
    }
    snprintf(ln, sizeof ln, "kf %lu   wkp %lu",
             (unsigned long)g_mon.n_kf,
             (unsigned long)g_mon.n_wkp);
    gfx_text(10, 34, ln, 2, 232, 232, 240);
    snprintf(ln, sizeof ln, "org %lu   drop %lu",
             (unsigned long)g_mon.n_org,
             (unsigned long)g_mon.n_drop);
    gfx_text(10, 56, ln, 2, 232, 232, 240);
    snprintf(ln, sizeof ln, "tsf offset %+lld us %s",
             (long long)(g_mon.tsf_ok ? g_mon.tsf_off : 0),
             g_mon.tsf_ok ? "(slaved)" : "(free)");
    gfx_text(10, 78, ln, 2, 124, 224, 201);
    gfx_text(10, 104, "rx/s (2 min)", 1, 110, 120, 138);
    spark(10, 116, 230, 44, g_mon.rxs_ring, g_mon.ring_i,
          65, 208, 255);
    gfx_text(LCD_W - 180, 8, "rung history", 1, 200, 180, 69);
    int shown = 0;
    for (int i = 0; i < 8; i++) {
        int idx = (g_mon.rung_n + 7 - i) % 8;
        if (!g_mon.rung[idx].step) continue;
        snprintf(ln, sizeof ln, "@%lu  %04x",
                 (unsigned long)g_mon.rung[idx].step,
                 g_mon.rung[idx].h16);
        gfx_text(LCD_W - 180, 22 + shown * 14, ln, 1, 200, 180, 69);
        shown++;
    }
    if (!shown)
        gfx_text(LCD_W - 180, 22, "none heard (0.62+?)", 1,
                 110, 120, 138);
    snprintf(ln, sizeof ln, "the eye: /  /fb.png  /viewer");
    gfx_text(10, 190, ln, 1, 110, 120, 138);
    gfx_text(10, 204, "relay: ws://<ip>:8777", 1,
             110, 120, 138);
}
