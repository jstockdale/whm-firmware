idf_component_register(SRCS "mon_main.c" "mon_net.c" "mon_parse.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi nvs_flash esp_netif
                                     lwip esp_event esp_timer driver)
