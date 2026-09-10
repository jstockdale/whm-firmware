#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "mon_pins.h"
#include "rm67162_init.h"
#include "mon_lcd.h"
static const char *TAG = "mon_lcd";
#include "freertos/semphr.h"
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_bounce;                 /* internal DMA chunk */
static SemaphoreHandle_t s_txdone;
static bool tx_done_cb(esp_lcd_panel_io_handle_t io,
                       esp_lcd_panel_io_event_data_t *e, void *u)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_txdone, &hp);
    return hp == pdTRUE;
}
#define CHUNK_LINES 12
#define OP_CMD   0x02
#define OP_COLOR 0x32
static void rm_cmd(uint8_t reg, const uint8_t *d, int n, int delay80)
{
    esp_lcd_panel_io_tx_param(s_io,
        ((uint32_t)OP_CMD << 24) | ((uint32_t)reg << 8), d, n);
    if (delay80) vTaskDelay(pdMS_TO_TICKS(120));
}
static void rm_window(uint16_t x1, uint16_t y1,
                      uint16_t x2, uint16_t y2)
{
    uint8_t ca[4] = { x1 >> 8, x1, x2 >> 8, x2 };
    uint8_t ra[4] = { y1 >> 8, y1, y2 >> 8, y2 };
    rm_cmd(0x2A, ca, 4, 0);
    rm_cmd(0x2B, ra, 4, 0);
}
void mon_lcd_brightness(uint8_t level)
{
    rm_cmd(0x51, &level, 1, 0);
}
void mon_lcd_init(void)
{
    gpio_config_t en = { .pin_bit_mask = 1ULL << MP_PMIC_EN,
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&en);
    gpio_set_level(MP_PMIC_EN, 1);         /* the GPIO 38 law */
    gpio_config_t rst = { .pin_bit_mask = 1ULL << MP_LCD_RST,
                          .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(MP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(MP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    spi_bus_config_t bus = {
        .sclk_io_num = MP_QSPI_SCK,
        .data0_io_num = MP_QSPI_D0,
        .data1_io_num = MP_QSPI_D1,
        .data2_io_num = MP_QSPI_D2,
        .data3_io_num = MP_QSPI_D3,
        .max_transfer_sz = LCD_W * CHUNK_LINES * 2 + 64,
        .flags = SPICOMMON_BUSFLAG_MASTER |
                 SPICOMMON_BUSFLAG_GPIO_PINS |
                 SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus,
                                       SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = MP_QSPI_CS,
        .dc_gpio_num = -1,
        .pclk_hz = 40 * 1000 * 1000,       /* conservative first light */
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = tx_done_cb,
        .flags = { .quad_mode = 1 },
    };
    s_txdone = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io));
    for (int i = 0; i < (int)(sizeof(rm67162_cmd) /
                              sizeof(rm67162_cmd[0])); i++)
        rm_cmd(rm67162_cmd[i].cmd, rm67162_cmd[i].data,
               rm67162_cmd[i].len & 0x7F,
               rm67162_cmd[i].len & 0x80);
    s_bounce = heap_caps_malloc(LCD_W * CHUNK_LINES * 2,
                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    {   /* saved brightness, if any */
        nvs_handle_t h; uint8_t bv;
        if (nvs_open("mon", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, "bright", &bv) == ESP_OK)
                mon_lcd_brightness(bv);
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "RM67162 up: %dx%d QSPI, brightness %d",
             LCD_W, LCD_H, AMOLED_DEFAULT_BRIGHTNESS);
}
void mon_lcd_push_full(const uint16_t *fb)
{
    for (int y = 0; y < LCD_H; y += CHUNK_LINES) {
        int lines = LCD_H - y;
        if (lines > CHUNK_LINES) lines = CHUNK_LINES;
        int n = LCD_W * lines;
        /* RGB565 big-endian on the wire */
        const uint16_t *src = fb + y * LCD_W;
        for (int i = 0; i < n; i++)
            s_bounce[i] = __builtin_bswap16(src[i]);
        rm_window(0, y, LCD_W - 1, y + lines - 1);
        esp_lcd_panel_io_tx_color(s_io,
            ((uint32_t)OP_COLOR << 24) | ((uint32_t)0x2C << 8),
            s_bounce, n * 2);
        /* THE BOUNCE RACE (owner's glass, 0.4.0): tx_color QUEUES
           the DMA and returns; the loop then overwrote s_bounce
           while the controller was still reading it - bands landed
           with mixed rows, intermittently, worse under load. Wait
           for the transfer before touching the buffer again. */
        xSemaphoreTake(s_txdone, pdMS_TO_TICKS(100));
    }
}
