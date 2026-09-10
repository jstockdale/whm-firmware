#include <stdio.h>
#include "nvs_flash.h"
#include "mon_config.h"
#include "mon_pins.h"
void mon_net_start(void);
void mon_lcd_init(void);
void mon_touch_init(void);
void mon_ui_start(void);
void mon_con_start(void);
void mon_http_start(void);
void mon_ws_start(void);
void mon_http_start(void);
void mon_sky_init(void);
void app_main(void)
{
    printf("==== %s ====\n", MON_VERSION);
    printf("board: %s\n", MON_BOARD_NAME);
    printf("passive fleet monitor - wire proof first, display next\n");
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES ||
        r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    mon_sky_init();
    mon_lcd_init();
    mon_touch_init();
    mon_ui_start();
    mon_net_start();
    mon_con_start();
    mon_http_start();
    mon_ws_start();
    mon_http_start();
}
