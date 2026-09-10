#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "mon_pins.h"
#include "mon_lcd.h"
/* ST7796 init sequence transcribed VERBATIM from Arduino_GFX
 * (moononournation) src/display/Arduino_ST7796.h - order kept,
 * incl. the F0 command-set unlocks, gamma pair, idle-off 0x38.
 * INVON added because the vendor ctor runs ips=true (Arduino_GFX
 * sends it separately). Colors inverted on glass => drop 0x21. */
static const char *TAG = "mon_lcd(st7796)";
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_txdone;
#define CHUNK_LINES 12
typedef struct { uint8_t cmd, len, delay_ms;
                 uint8_t data[14]; } st_cmd_t;
static const st_cmd_t st_init[] = {
    { 0x3A, 1, 0, { 0x55 } },
    { 0xF0, 1, 0, { 0xC3 } },
    { 0xF0, 1, 0, { 0x96 } },
    { 0xB4, 1, 0, { 0x01 } },
    { 0xB6, 3, 0, { 0x80, 0x22, 0x3B } },
    { 0xE8, 8, 0, { 0x40, 0x8A, 0x00, 0x00, 0x29,
                    0x19, 0xA5, 0x33 } },
    { 0xC1, 1, 0, { 0x06 } },
    { 0xC2, 1, 0, { 0xA7 } },
    { 0xC5, 1, 0, { 0x18 } },
    { 0xE0, 14, 0, { 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F,
                     0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B } },
    { 0xE1, 14, 0, { 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B,
                     0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B } },
    { 0xF0, 1, 0, { 0x3C } },
    { 0xF0, 1, 0, { 0x69 } },
    { 0x21, 0, 0, { 0 } },              /* INVON (IPS) */
    { 0x11, 0, 120, { 0 } },            /* SLPOUT */
    { 0x38, 0, 0, { 0 } },              /* idle off */
    { 0x36, 1, 0, { MP_MADCTL_L } },    /* landscape */
    { 0x29, 0, 120, { 0 } },            /* DISPON */
};
static bool tx_done_cb(esp_lcd_panel_io_handle_t io,
                       esp_lcd_panel_io_event_data_t *e, void *u)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_txdone, &hp);
    return hp == pdTRUE;
}
static void st_cmd(uint8_t reg, const uint8_t *d, int n)
{
    esp_lcd_panel_io_tx_param(s_io, reg, d, n);
}
static void st_window(uint16_t x1, uint16_t y1,
                      uint16_t x2, uint16_t y2)
{
    x1 += MP_GAP_X; x2 += MP_GAP_X;
    y1 += MP_GAP_Y; y2 += MP_GAP_Y;
    uint8_t ca[4] = { x1 >> 8, x1, x2 >> 8, x2 };
    uint8_t ra[4] = { y1 >> 8, y1, y2 >> 8, y2 };
    st_cmd(0x2A, ca, 4);
    st_cmd(0x2B, ra, 4);
}
void mon_lcd_brightness(uint8_t level)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
void mon_lcd_init(void)
{
    ledc_timer_config_t lt = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = {
        .gpio_num = MP_TFT_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 200,
    };
    ledc_channel_config(&lc);
    gpio_config_t rst = { .pin_bit_mask = 1ULL << MP_TFT_RST,
                          .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(MP_TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(MP_TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    spi_bus_config_t bus = {
        .sclk_io_num = MP_TFT_SCK,
        .mosi_io_num = MP_TFT_MOSI,
        .miso_io_num = MP_TFT_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * CHUNK_LINES * 2 + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus,
                                       SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = MP_TFT_CS,
        .dc_gpio_num = MP_TFT_DC,
        .pclk_hz = 80 * 1000 * 1000  /* ST7796 fast write; drop to 40 if glass disagrees */,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = tx_done_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io));
    s_txdone = xSemaphoreCreateBinary();
    for (int i = 0; i < (int)(sizeof(st_init) /
                              sizeof(st_init[0])); i++) {
        st_cmd(st_init[i].cmd, st_init[i].data, st_init[i].len);
        if (st_init[i].delay_ms)
            vTaskDelay(pdMS_TO_TICKS(st_init[i].delay_ms));
    }
    {
        nvs_handle_t h; uint8_t bv;
        if (nvs_open("mon", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, "bright", &bv) == ESP_OK)
                mon_lcd_brightness(bv);
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "ST7796 up: %dx%d SPI, gap %d/%d, board %s",
             LCD_W, LCD_H, MP_GAP_X, MP_GAP_Y, MON_BOARD_NAME);
}
void mon_lcd_push_full(const uint16_t *fb)
{
    for (int y = 0; y < LCD_H; y += CHUNK_LINES) {
        int lines = LCD_H - y;
        if (lines > CHUNK_LINES) lines = CHUNK_LINES;
        int n = LCD_W * lines;
        /* fb pre-swapped - pure DMA, no bounce. */
        const uint16_t *src = fb + y * LCD_W;
        st_window(0, y, LCD_W - 1, y + lines - 1);
        esp_lcd_panel_io_tx_color(s_io, 0x2C,
                                  (const void *)src, n * 2);
        xSemaphoreTake(s_txdone, pdMS_TO_TICKS(100));
    }
}
