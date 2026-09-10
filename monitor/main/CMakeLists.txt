idf_component_register(SRCS "mon_main.c" "mon_net.c" "mon_parse.c" "mon_lcd.c" "mon_touch.c" "mon_ui.c" "gfx5x7.c" "mon_wifi.c" "mon_con.c" "mon_world.c" "mon_render.c" "mon_http.c" "mon_ws.c" "mon_pages.c" "mon_sky.c" "mon_http.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi nvs_flash esp_netif
                                     lwip esp_event esp_timer driver esp_lcd esp_driver_usb_serial_jtag esp_http_server mbedtls esp_http_server mbedtls)

target_add_binary_data(${COMPONENT_LIB}
    "${CMAKE_CURRENT_SOURCE_DIR}/whm_viewer.html" TEXT)
