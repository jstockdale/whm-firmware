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
void mon_render(uint32_t kfs);
static void ui_task(void *arg)
{
    uint32_t pk = 0, kfs = 0; int tick = 0;
    printf("world twin online - second native consumer\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(33));
        if (++tick >= 30) { tick = 0;
            kfs = g_mon.n_kf - pk; pk = g_mon.n_kf; }
        memset(s_fb, 0, LCD_W * LCD_H * 2);
        mon_render(kfs);
        if (g_touch.pressed)
            printf("touch: x=%u y=%u\n", g_touch.x, g_touch.y);
        mon_snap_service(s_fb);        /* the eye, atomically */
        mon_lcd_push_full(s_fb);
    }
}
void mon_ui_start(void)
{
    s_fb = heap_caps_calloc(LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);
    xTaskCreate(ui_task, "mon_ui", 6144, NULL, 7, NULL);
}
