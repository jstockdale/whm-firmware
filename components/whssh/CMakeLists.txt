idf_component_register(
    SRCS "wh_ssh.c" "wh_lineedit.c" "wh_conlog.c" "whssh_log.c"
    INCLUDE_DIRS "include"
    REQUIRES wolfssl__wolfssh wolfssl__wolfssl console nvs_flash
             esp_timer esp_app_format esp_netif esp_wifi
    PRIV_REQUIRES esp_driver_usb_serial_jtag)
target_compile_definitions(${COMPONENT_LIB} PRIVATE WH_HAVE_WOLFSSH WOLFSSL_USER_SETTINGS WOLFSSH_USER_SETTINGS)
