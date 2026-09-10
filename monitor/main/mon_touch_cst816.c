#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "mon_pins.h"
#include "mon_touch.h"
static const char *TAG = "mon_touch";
mon_touch_t g_touch;
static i2c_master_dev_handle_t s_dev;
static void touch_task(void *arg)
{
    uint8_t reg = 0x00, b[7];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(33));
        if (i2c_master_transmit_receive(s_dev, &reg, 1, b, 7,
                                        50) != ESP_OK) continue;
        uint8_t n = b[2] & 0x0F;
        if (n) {
            uint16_t rx = ((b[3] & 0x0F) << 8) | b[4];   /* 0..239 */
            uint16_t ry = ((b[5] & 0x0F) << 8) | b[6];   /* 0..535 */
            g_touch.x = ry;                    /* landscape map */
            g_touch.y = (LCD_H - 1) - rx;
            if (!g_touch.pressed) g_touch.seq++;
            g_touch.pressed = 1;
        } else g_touch.pressed = 0;
    }
}
void mon_touch_init(void)
{
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
                               .device_address = 0x15,
                               .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &s_dev));
    xTaskCreate(touch_task, "mon_touch", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "CST816T on I2C sda=%d scl=%d (poll)",
             MP_TOUCH_SDA, MP_TOUCH_SCL);
}
