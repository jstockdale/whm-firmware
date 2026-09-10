/* CST226SE raw driver - registers per Arduino_DriveBus's
 * Arduino_CST2xxSE (GPL-3, same as us): 12-bit coords,
 * X1H=0x01 Y1H=0x02, SHARED low byte 0x03 (X=high nibble,
 * Y=low nibble), finger count 0x05 & 0x0F. Note: the vendor
 * source carries a C precedence bug ('& 0xF0 >> 4'); we
 * implement the correct nibble split - if axes misbehave on
 * glass, try the vendor's accidental form. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "mon_pins.h"
#include "mon_touch.h"
static const char *TAG = "mon_touch(cst226)";
mon_touch_t g_touch;
static i2c_master_dev_handle_t s_dev;
static void touch_task(void *arg)
{
    uint8_t reg = 0x00, b[6];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(33));
        if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 6,
                                        50) != ESP_OK) continue;
        uint8_t n = b[5] & 0x0F;
        if (n && n <= 5) {
            uint16_t rx = ((uint16_t)b[1] << 4) | (b[3] >> 4);
            uint16_t ry = ((uint16_t)b[2] << 4) | (b[3] & 0x0F);
            /* native portrait 222x480 -> landscape */
            g_touch.x = ry;
            g_touch.y = (LCD_H - 1) - rx;
            if (!g_touch.pressed) g_touch.seq++;
            g_touch.pressed = 1;
        } else g_touch.pressed = 0;
    }
}
void mon_touch_init(void)
{
    gpio_config_t r = { .pin_bit_mask = 1ULL << MP_TOUCH_RST,
                        .mode = GPIO_MODE_OUTPUT };
    gpio_config(&r);
    gpio_set_level(MP_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(MP_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(80));
    i2c_master_bus_config_t bc = {
        .i2c_port = 0,
        .sda_io_num = MP_TOUCH_SDA,
        .scl_io_num = MP_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                               .device_address = MP_TOUCH_ADDR,
                               .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &s_dev));
    xTaskCreate(touch_task, "mon_touch", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "CST226SE @0x%02X sda=%d scl=%d",
             MP_TOUCH_ADDR, MP_TOUCH_SDA, MP_TOUCH_SCL);
}
