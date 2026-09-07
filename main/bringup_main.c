/*
 * bringup_main.c - WHM Phase 0 staged hardware validation.
 *
 * Boot sequence (each stage logs a clear banner and never blocks the next
 * on failure - a dead sensor should not hide a working display):
 *
 *   0. chip/flash/PSRAM/heap inventory
 *   1. HUB75 display up (esp-hub75, FM6126A, double-buffered) + green blink
 *   2. WiFi (STA or SoftAP per Kconfig), power save OFF, TSF observer task
 *   3. I2C scan + QMI8658 / SHTC3 / PCF85063 init
 *   4. SD card mount (SDMMC 1-bit) + 4MB r/w throughput benchmark
 *   5. audio: 1kHz speaker burst + 1s dual-mic RMS capture (Kconfig gate)
 *   6. forever: test-pattern task (core 1) + 5s telemetry loop (core 0)
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif

#include "whm_board.h"
#include "display_hal.h"
#include "ui.h"
#include "storage_test.h"
#include "sensors_test.h"
#include "audio_test.h"
#include "wifi_tsf.h"
#include "console.h"
#include "mp3_player.h"
#include "sync.h"
#include "http_svc.h"
#include "esp_heap_caps.h"
#include "settings.h"
#include "timesync.h"

static const char *TAG = "whm_main";

#define STAGE(name) ESP_LOGI(TAG, "==== [%s] ====", name)

static void banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "WHM bring-up | IDF %s | ESP32-S3 rev v%d.%d | %d cores",
             esp_get_idf_version(), chip.revision / 100, chip.revision % 100,
             chip.cores);
    ESP_LOGI(TAG, "flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "PSRAM: %u MB", (unsigned)(esp_psram_get_size() / (1024 * 1024)));
#else
    ESP_LOGW(TAG, "PSRAM: disabled in sdkconfig?!");
#endif
    ESP_LOGI(TAG, "heap free: internal=%u KB, psram=%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void telemetry_once(void)
{
    float ax, ay, az, gx, gy, gz;
    if (whm_sensors_read_imu(&ax, &ay, &az, &gx, &gy, &gz) == ESP_OK) {
        ESP_LOGD(TAG, "IMU  accel[m/s2] %+.2f %+.2f %+.2f  gyro[rad/s] %+.3f %+.3f %+.3f",
                 ax, ay, az, gx, gy, gz);
    }
    float t, rh;
    if (whm_sensors_read_th(&t, &rh) == ESP_OK) {
        ESP_LOGD(TAG, "ENV  %.1f C  %.0f %%RH", t, rh);
    }
    char ts[32];
    if (whm_sensors_read_rtc(ts, sizeof(ts)) == ESP_OK) {
        ESP_LOGD(TAG, "RTC  %s", ts);
    }
    ESP_LOGD(TAG, "HEAP internal=%u KB psram=%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void app_main(void)
{
    STAGE("0: inventory");
    banner();
    whm_settings_init();
    whm_timesync_apply_tz();      /* localtime valid before anything uses it */

    /* Console FIRST: it needs only USB-Serial-JTAG. Boot order was the
       assassin - by stage 6 the internal heap was spent (display DMA
       ~115K + wifi + feature-task stacks) and the REPL's 8K allocation
       failed rc=-1, on both units, deterministically. Front of the
       line, forever. */
    if (whm_console_start() != ESP_OK) {
        ESP_LOGE(TAG, "console failed to start - see errors above");
    }

    STAGE("1: display");
    uint8_t bright = CONFIG_WHM_TEST_BRIGHTNESS;
    if (whm_settings_get_u8("bright", &bright) == ESP_OK) {
        ESP_LOGI(TAG, "brightness %u from NVS (console 'brightness' to change)",
                 (unsigned)bright);
    }
    printf("display: brightness %u\n", (unsigned)bright);
    if (whm_display_init(bright)) {
        /* visible proof-of-life before anything slow happens */
        whm_display_fill(0, 160, 0);
        whm_display_flip();
        vTaskDelay(pdMS_TO_TICKS(400));
        whm_display_clear();
        whm_display_flip();
    } else {
        ESP_LOGE(TAG, "display init FAILED - check FM6126A vs GENERIC in "
                      "display_hal.cpp and panel power");
    }

    STAGE("2: wifi + TSF");
    whm_wifi_start();

    STAGE("3: i2c + sensors");
    whm_i2c_scan();
    whm_sensors_init();
    whm_timesync_seed_from_rtc(); /* system clock sane pre-SNTP / off-grid */

    STAGE("4: sd card");
    whm_storage_mount();           /* card + FAT info only; 'sd bench' is
                                      on-demand (a slow card once cost 17s
                                      of boot here) */
    whm_storage_start_watcher();   /* hot-swap: mounts on insert, no reboot */
    whm_sync_init();
    whm_http_start();                   /* fleet link resumes its role */
    /* chime now fires from Ignition (ui task) for A/V phase lock */
    whm_mp3_init();
    if (whm_storage_mounted()) whm_mp3_scan();

#if CONFIG_WHM_RUN_AUDIO_TEST
    STAGE("5: audio");
#else
    ESP_LOGI(TAG, "stage 5: audio test disabled by Kconfig");
#endif

    STAGE("6: screens + telemetry");
    {
        multi_heap_info_t hi;
        heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "internal heap after bring-up: %u KB free, "
                      "largest block %u KB",
                 (unsigned)(hi.total_free_bytes / 1024),
                 (unsigned)(hi.largest_free_block / 1024));
    }
    xTaskCreatePinnedToCore(whm_ui_task, "whm_ui", 6144, NULL, 4, NULL, 1);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        telemetry_once();
    }
}
