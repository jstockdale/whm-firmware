idf_component_register(SRCS "mon_main.c" "mon_net.c" "mon_parse.c" "mon_lcd.c" "mon_touch.c" "mon_ui.c" "gfx5x7.c" "mon_wifi.c" "mon_con.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi nvs_flash esp_netif
                                     lwip esp_event esp_timer driver esp_lcd esp_driver_usb_serial_jtag)
