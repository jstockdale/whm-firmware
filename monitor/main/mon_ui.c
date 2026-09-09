#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "mon_pins.h"
#include "mon_lcd.h"
#include "mon_touch.h"
#include "mon_parse.h"
#include "gfx5x7.h"
#include "display_hal.h"
static uint16_t *s_fb;
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
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
static void ui_task(void *arg)
{
    char ln[64];
    uint32_t pk = 0, kfs = 0; int tick = 0, flash = 0;
    uint32_t tseq = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));     /* 20 fps first light */
        if (++tick >= 20) { tick = 0;
            kfs = g_mon.n_kf - pk; pk = g_mon.n_kf; }
        if (g_touch.seq != tseq) { tseq = g_touch.seq; flash = 4;
            printf("touch: x=%u y=%u\n", g_touch.x, g_touch.y); }
        memset(s_fb, 0, LCD_W * LCD_H * 2);
        uint8_t hr = flash ? 255 : 65,
                hg = flash ? 255 : 208,
                hb = flash ? 255 : 255;
        if (flash) flash--;
        gfx_text(8, 6, "WHM MONITOR 0.2.0 - FIRST LIGHT", 2,
                 hr, hg, hb);
        snprintf(ln, sizeof ln, "step %lu   %s   own %u",
                 (unsigned long)g_mon.step,
                 mon_st_name(g_mon.st), g_mon.owner);
        gfx_text(8, 40, ln, 2, 232, 232, 240);
        snprintf(ln, sizeof ln, "x %.1f  y %.1f", g_mon.x, g_mon.y);
        gfx_text(8, 66, ln, 2, 180, 190, 205);
        snprintf(ln, sizeof ln, "cam %.1f", g_mon.cam);
        gfx_text(8, 92, ln, 2, 180, 190, 205);
        snprintf(ln, sizeof ln, "kf/s %lu   drop %lu",
                 (unsigned long)kfs, (unsigned long)g_mon.n_drop);
        gfx_text(8, 118, ln, 2, 140, 150, 168);
        snprintf(ln, sizeof ln, "rung @%lu",
                 (unsigned long)g_mon.rung_step);
        gfx_text(8, 144, ln, 2, 200, 180, 69);
        snprintf(ln, sizeof ln, "from %s", g_mon.from);
        gfx_text(8, 170, ln, 2, 140, 150, 168);
        /* owner bar, right edge - the fleet's color language */
        if (g_mon.owner)
            whm_display_fill_rect(LCD_W - 10, 0, 10, LCD_H,
                                  255, 92, 200);
        else
            whm_display_fill_rect(LCD_W - 10, 0, 10, LCD_H,
                                  65, 208, 255);
        if (g_touch.pressed)
            whm_display_fill_rect(g_touch.x > 6 ? g_touch.x - 6 : 0,
                                  g_touch.y > 6 ? g_touch.y - 6 : 0,
                                  13, 13, 124, 224, 201);
        mon_lcd_push_full(s_fb);
    }
}
void mon_ui_start(void)
{
    s_fb = heap_caps_calloc(LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);
    xTaskCreate(ui_task, "mon_ui", 6144, NULL, 7, NULL);
}
