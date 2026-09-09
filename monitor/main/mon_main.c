#include <stdio.h>
#include "nvs_flash.h"
#include "mon_config.h"
void mon_net_start(void);
void mon_lcd_init(void);
void mon_touch_init(void);
void mon_ui_start(void);
void app_main(void)
{
    printf("==== %s ====\n", MON_VERSION);
    printf("passive fleet monitor - wire proof first, display next\n");
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES ||
        r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    mon_lcd_init();
    mon_touch_init();
    mon_ui_start();
    mon_net_start();
}
