#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "mon_pins.h"
#include "mon_lcd.h"
#include "mon_touch.h"
#include "mon_parse.h"
#include "mon_http.h"
#include "esp_timer.h"
#include "mon_http.h"
#include "gfx5x7.h"
#include "display_hal.h"
static uint16_t *s_fb;
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* THE FAST PATH: the framebuffer stores RGB565 already
       byte-swapped (panel byte order), so the push loop is
       pure DMA - no bounce buffer, no quarter-million-swap
       loop per frame. One choke point: every producer comes
       through here. The Eye's encoders unswap on read. */
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));
}
void whm_display_fill_rect(uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h,
                           uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= LCD_W || y >= LCD_H) return;
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    uint16_t c = rgb(r, g, b);
    for (uint16_t yy = 0; yy < h; yy++) {
        uint16_t *p = s_fb + (y + yy) * LCD_W + x;
        for (uint16_t xx = 0; xx < w; xx++) p[xx] = c;
    }
}
void mon_render(uint32_t kfs);
void mon_render_full(void);
void mon_page_stats(uint32_t kfs);
void mon_page_wire(void);
static const char *PGN[4] = { "WORLD", "FULL", "STATS", "WIRE" };
uint32_t s_upds;
static void player_adopt(int sl)
{
    /* the renderer is the SOLE pose writer now - ordered
       adoption from the bank; every drawn pose is a real
       received pose, in order, once. */
    mon_wkb_t *k = &g_mon.ring16[sl];
    g_mon.step = k->step; g_mon.x = k->x;
    g_mon.y = (float)k->yq1 * 0.5f;
    g_mon.vx = k->vx; g_mon.vy = k->vy; g_mon.spd = k->spd;
    g_mon.tgt = k->tgt; g_mon.st = k->st; g_mon.owner = k->owner;
    g_mon.dir = k->dir; g_mon.sdir = k->sdir;
    g_mon.phase = k->phase; g_mon.timer = k->timer;
    memcpy(g_mon.from, k->from, 16); g_mon.from[15] = 0;
    if (k->st != g_mon.last_st) {
        g_mon.last_st = k->st;
        g_mon.tr[g_mon.tr_n].step = k->step;
        g_mon.tr[g_mon.tr_n].st = k->st;
        g_mon.tr[g_mon.tr_n].owner = k->owner;
        g_mon.tr_n = (uint8_t)((g_mon.tr_n + 1) % 5);
    }
}                    /* distinct steps drawn /s */
static void boot_flourish(void)
{
    /* POP sparkle -> scarf-rainbow wipe -> version card.
       ~2.6 s of ceremony; any tap skips. */
    uint32_t t0seq = g_touch.seq;
    for (int fr = 0; fr < 78; fr++) {
        if (g_touch.seq != t0seq) break;
        int64_t t = esp_timer_get_time();
        memset(s_fb, 0, LCD_W * LCD_H * 2);
        if (fr < 24) {                      /* arrival sparkle */
            int cxp = LCD_W / 2, cyp = LCD_H / 2;
            for (int i = 0; i < 10; i++) {
                uint8_t r2, g2, b2;
                extern void mw_hsv(uint16_t, uint8_t, uint8_t,
                    uint8_t *, uint8_t *, uint8_t *);
                mw_hsv((uint16_t)((t / 15000 + i * 36) % 360),
                       255, 255, &r2, &g2, &b2);
                int a = (fr + i * 7) & 15;
                whm_display_fill_rect(
                    cxp + ((i & 1) ? a * 5 : -a * 5),
                    cyp + (((i >> 1) & 1) ? a * 2 : -a * 2),
                    4, 4, r2, g2, b2);
            }
        } else if (fr < 54) {               /* the scarf wipe */
            int wx = (fr - 24) * (LCD_W + 40) / 30 - 20;
            for (int b9 = 0; b9 < 24; b9++) {
                uint8_t r2, g2, b2;
                extern void mw_hsv(uint16_t, uint8_t, uint8_t,
                    uint8_t *, uint8_t *, uint8_t *);
                mw_hsv((uint16_t)((t / 90000 + b9 * 15) % 360),
                       230, 255, &r2, &g2, &b2);
                int x9 = wx - b9;
                if (x9 >= 0 && x9 < LCD_W)
                    whm_display_fill_rect(x9, 0, 1, LCD_H,
                                          r2, g2, b2);
            }
        } else {                            /* version card */
            gfx_text_center(LCD_W / 2, 92, "WHM MONITOR", 3,
                            232, 232, 240);
            gfx_text_center(LCD_W / 2, 128,
                            "passive realtime telemetry", 1,
                            140, 150, 168);
        }
        mon_lcd_push_full(s_fb);
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}
static void ui_task(void *arg)
{
    uint32_t pk = 0, kfs = 0; int tick = 0;
    printf("world twin online - passive realtime telemetry\n");
    boot_flourish();
    uint8_t page = 0; uint32_t tseq = g_touch.seq;
    int64_t toast_until = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MP_UI_MS));
        if (++tick >= MP_UI_FPS) { tick = 0;
            kfs = g_mon.n_kf - pk; pk = g_mon.n_kf;
            static uint32_t prx;
            uint32_t rx = g_mon.n_kf + g_mon.n_wkp + g_mon.n_org;
            g_mon.kfs_ring[g_mon.ring_i] =
                kfs > 255 ? 255 : (uint8_t)kfs;
            g_mon.rxs_ring[g_mon.ring_i] =
                (rx - prx) > 255 ? 255 : (uint8_t)(rx - prx);
            prx = rx;
            g_mon.ring_i = (uint8_t)((g_mon.ring_i + 1) % 120);
        }
        if (g_touch.seq != tseq) {          /* tap: next page */
            tseq = g_touch.seq;
            page = (uint8_t)((page + 1) % 4);
            toast_until = esp_timer_get_time() + 1500000;
        }
        {   /* THE ORDERED PLAYER: adopt cursor+1 when banked
               (late UDP arrivals land in their slot and still
               get played); on genuine loss (>6 ahead banked,
               next missing) skip to the smallest banked step
               beyond the cursor; drain max 2/frame when
               behind. */
            if (!g_mon.play_cursor && g_mon.max_banked)
                g_mon.play_cursor = g_mon.max_banked - 1;
            int adv = (g_mon.max_banked >
                       g_mon.play_cursor + 4) ? 2 : 1;
            while (adv-- > 0 &&
                   g_mon.play_cursor < g_mon.max_banked) {
                uint32_t nxt = g_mon.play_cursor + 1;
                int sl = (int)(nxt & 15u);
                if (g_mon.ring16_step[sl] == nxt) {
                    player_adopt(sl);
                    g_mon.play_cursor = nxt;
                } else if (g_mon.max_banked >
                           g_mon.play_cursor + 6) {
                    uint32_t best = 0;
                    for (int i = 0; i < 16; i++) {
                        uint32_t s9 = g_mon.ring16_step[i];
                        if (s9 > g_mon.play_cursor &&
                            (!best || s9 < best)) best = s9;
                    }
                    if (!best) break;
                    player_adopt((int)(best & 15u));
                    g_mon.play_cursor = best;
                    break;
                } else break;
            }
        }
        {   /* RENDER-SIDE truth (owner: counters watch the
               door, not the stage): count DISTINCT steps the
               ui actually observes, ring the last 8 drawn. */
            static uint32_t pstep; static uint32_t updc;
            static uint8_t sec9;
            if (g_mon.step != pstep) {
                pstep = g_mon.step; updc++;
                g_mon.drawn8[g_mon.drawn8_i] = g_mon.step;
                g_mon.drawn8_i =
                    (uint8_t)((g_mon.drawn8_i + 1) % 8);
            }
            if (++sec9 >= MP_UI_FPS) { sec9 = 0;
                s_upds = updc; updc = 0; }
        }
        memset(s_fb, 0, LCD_W * LCD_H * 2);
        if (page == 0) mon_render(kfs);
        else if (page == 1) mon_render_full();
        else if (page == 2) mon_page_stats(kfs);
        else mon_page_wire();
        if (esp_timer_get_time() < toast_until)
            gfx_text(LCD_W - 8 -
                     gfx_text_width(PGN[page], 2),
                     LCD_H - 22, (char *)PGN[page], 2,
                     124, 224, 201);
        mon_snap_service(s_fb);
        mon_lcd_push_full(s_fb);
    }
}
void mon_ui_start(void)
{
    s_fb = heap_caps_calloc(LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);
    xTaskCreate(ui_task, "mon_ui", 6144, NULL, 7, NULL);
}
