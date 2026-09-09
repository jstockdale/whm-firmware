# NOTE: no REQUIRES on 'main' -> IDF gives main an implicit dependency on every
# component in the build, including managed components. Keep it that way.
idf_component_register(
    SRCS
        "bringup_main.c"
        "display_hal.cpp"
        "ui.c"
        "whlink_glue.c"
        "gfx5x7.c"
        "storage_test.c"
        "sensors_test.c"
        "audio_test.c"
        "wifi_tsf.c"
        "console.c"
        "settings.c"
        "timesync.c"
        "mp3_player.c" "crypto_id.c" "whm_media.c" "monocypher.c"
        "tz_table.c"
        "sync.c"
        "http_svc.c"
        "ota.c"
    INCLUDE_DIRS "."    PRIV_REQUIRES
        bt whlink
        spi_flash esp_wifi esp_netif esp_event nvs_flash esp_timer
        esp_http_client esp_http_server app_update esp_app_format
        driver sdmmc fatfs vfs esp_hw_support esp_system mdns
        esp_adc esp_psram console esp_codec_dev json mbedtls lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE WHM_VERSION_STR="${PROJECT_VER}")
