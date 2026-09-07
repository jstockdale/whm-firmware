/*
 * storage_test.c - SDMMC 1-bit mount + throughput benchmark.
 *
 * Mount sequence mirrors Waveshare's bsp_sdcard_mount(): enable internal
 * pullups on CMD/D0, settle 20ms, SDMMC slot 0, 1-bit width, FATFS mount.
 *
 * Benchmark answers the Phase 0 question: is SDMMC 1-bit fast enough for the
 * media pipeline? (Target: comfortably above 0.36 MB/s = 64x64@30fps RGB888.)
 */
#include "storage_test.h"
#include "mp3_player.h"
#include "whm_board.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"

static const char *TAG = "whm_sd";
static sdmmc_card_t *s_card = NULL;
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_maint = false;   /* long op (format) owns the volume */

static void lock_init(void) { if (!s_lock) s_lock = xSemaphoreCreateMutex(); }
static bool lock_take(uint32_t ms)
{
    lock_init();
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(ms)) == pdTRUE;
}
static void lock_give(void) { xSemaphoreGive(s_lock); }

/* the noisy tags the sdmmc stack error-logs on every empty-slot probe */
static const char *k_sd_tags[] = {"sdmmc_common", "sdmmc_cmd", "sdmmc_sd",
                                  "vfs_fat_sdmmc"};

static esp_err_t mount_locked(bool quiet)
{
    if (s_card) return ESP_OK;

    esp_log_level_t saved[4];
    if (quiet) {
        for (int i = 0; i < 4; i++) {
            saved[i] = esp_log_level_get(k_sd_tags[i]);
            esp_log_level_set(k_sd_tags[i], ESP_LOG_NONE);
        }
    }

    gpio_pullup_en(WHM_SD_CMD);
    gpio_pullup_en(WHM_SD_D0);
    vTaskDelay(pdMS_TO_TICKS(20));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* negotiate down if unsupported */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = WHM_SD_CLK;
    slot.cmd   = WHM_SD_CMD;
    slot.d0    = WHM_SD_D0;
    slot.width = WHM_SD_WIDTH;
    slot.cd    = SDMMC_SLOT_NO_CD;
    slot.wp    = SDMMC_SLOT_NO_WP;
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,   /* media-friendly; halves FAT
                                                size on big cards */
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(WHM_SD_MOUNT, &host, &slot,
                                            &mount_cfg, &s_card);
    if (quiet) {
        for (int i = 0; i < 4; i++) esp_log_level_set(k_sd_tags[i], saved[i]);
    }
    if (err != ESP_OK) {
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(WHM_SD_MOUNT, &total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "FAT: %llu MB total, %llu MB free",
                 total / (1024ULL * 1024ULL), freeb / (1024ULL * 1024ULL));
    }
    return ESP_OK;
}

static void unmount_locked(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(WHM_SD_MOUNT, s_card);
    s_card = NULL;
}

esp_err_t whm_storage_mount(void)
{
    if (!lock_take(5000)) return ESP_ERR_TIMEOUT;
    esp_err_t err = mount_locked(false);
    lock_give();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) - card absent or unreadable",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t whm_storage_unmount(void)
{
    if (!lock_take(5000)) return ESP_ERR_TIMEOUT;
    unmount_locked();
    lock_give();
    ESP_LOGI(TAG, "SD unmounted - safe to swap the card");
    return ESP_OK;
}

/* Hot-swap watcher: no card-detect pin exists on this board, so poll.
 * Unmounted: quiet mount attempt every 3s (sdmmc error spam suppressed
 * during probes only). Mounted: liveness check; on failure, auto-unmount
 * so a reinserted card comes back cleanly. */
static void sd_watcher(void *arg)
{
    (void)arg;
    bool was_mounted = (s_card != NULL);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (!lock_take(0)) continue;      /* console op in progress; skip */
        if (!s_card) {
            if (mount_locked(true) == ESP_OK) {
                ESP_LOGI(TAG, "SD card inserted and mounted");
                whm_mp3_invalidate();      /* old card's index is dead */
                whm_mp3_scan();            /* index the new card */
                was_mounted = true;
            }
        } else {
            if (sdmmc_get_status(s_card) != ESP_OK) {
                ESP_LOGW(TAG, "SD card removed or failing - unmounting");
                whm_mp3_invalidate();
                unmount_locked();
                was_mounted = false;
            } else {
                was_mounted = true;
            }
        }
        (void)was_mounted;
        lock_give();
    }
}

void whm_storage_start_watcher(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreatePinnedToCore(sd_watcher, "whm_sdwatch", 3072, NULL, 2, NULL, 0);
    ESP_LOGI(TAG, "SD hot-swap watcher running (3s poll; no CD pin on board)");
}

bool whm_storage_mounted(void)
{
    return s_card != NULL;
}

void whm_storage_info(void)
{
    if (!s_card) {
        printf("sd: not mounted\n");
        return;
    }
    uint64_t total = 0, freeb = 0;
    printf("sd: mounted at %s, %s, %d-bit\n", WHM_SD_MOUNT, s_card->cid.name,
           WHM_SD_WIDTH);
    if (esp_vfs_fat_info(WHM_SD_MOUNT, &total, &freeb) == ESP_OK) {
        printf("    %llu MB total, %llu MB free\n",
               (unsigned long long)(total / (1024ULL * 1024ULL)),
               (unsigned long long)(freeb / (1024ULL * 1024ULL)));
    }
}

esp_err_t whm_storage_ls(const char *path)
{
    if (!s_card) {
        printf("sd: not mounted\n");
        return ESP_ERR_INVALID_STATE;
    }
    char full[128];
    if (path && path[0] == '/') {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        snprintf(full, sizeof(full), "%s%s%s", WHM_SD_MOUNT,
                 path ? "/" : "", path ? path : "");
    }
    DIR *d = opendir(full);
    if (!d) {
        printf("cannot open %s\n", full);
        return ESP_FAIL;
    }
    struct dirent *e;
    int n = 0;
    errno = 0;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        char fp[400];
        snprintf(fp, sizeof(fp), "%s/%s", full, e->d_name);
        long sz = (stat(fp, &st) == 0) ? (long)st.st_size : -1;
        printf("  %-10s %8ld  %s\n",
               e->d_type == DT_DIR ? "<dir>" : "", sz, e->d_name);
        n++;
    }
    int rd_errno = errno;
    closedir(d);
    printf("%d entries in %s\n", n, full);
    if (n == 0 && rd_errno) {
        printf("  (readdir errno=%d - if ENOMEM, FATFS LFN buffers "
               "are starving)\n", rd_errno);
    }
    return ESP_OK;
}

esp_err_t whm_storage_card_serial(uint32_t *out)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    *out = (uint32_t)s_card->cid.serial;
    return ESP_OK;
}

void whm_storage_card_desc(char *out, size_t out_len)
{
    if (!s_card) {
        snprintf(out, out_len, "(no card)");
        return;
    }
    uint64_t mb = ((uint64_t)s_card->csd.capacity * 512ULL) / (1024ULL * 1024ULL);
    snprintf(out, out_len, "%s, %llu MB, serial 0x%08lx",
             s_card->cid.name, (unsigned long long)mb,
             (unsigned long)s_card->cid.serial);
}

/* Formats the ENTIRE card as FAT (reclaims odd partitioning). Caller is
 * responsible for having confirmed with the user - see console challenge. */
esp_err_t whm_storage_format(void)
{
    if (!lock_take(5000)) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (!s_card) err = mount_locked(false);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "FORMATTING card - do not power off or pull the card");
        s_maint = true;
        /* belt to the busy-flag suspenders: any straggler f_getfree during
           mkfs would E-spam under the vfs_fat tag */
        esp_log_level_t fat_lvl = esp_log_level_get("vfs_fat");
        esp_log_level_set("vfs_fat", ESP_LOG_NONE);
        err = esp_vfs_fat_sdcard_format(WHM_SD_MOUNT, s_card);
        esp_log_level_set("vfs_fat", fat_lvl);
        s_maint = false;
        if (err == ESP_OK) {
            uint64_t total = 0, freeb = 0;
            if (esp_vfs_fat_info(WHM_SD_MOUNT, &total, &freeb) == ESP_OK) {
                ESP_LOGI(TAG, "format complete: %llu MB total, %llu MB free",
                         total / (1024ULL * 1024ULL),
                         freeb / (1024ULL * 1024ULL));
            }
        } else {
            ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        }
    }
    lock_give();
    return err;
}

bool whm_storage_maintenance(void) { return s_maint; }

/* Polled by the UI - MUST stay silent on failure. During maintenance the
 * FatFs volume is legitimately unavailable (FR_NOT_ENABLED); callers just
 * see BUSY and render accordingly. */
esp_err_t whm_storage_stats(uint32_t *total_mb, uint32_t *free_mb)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    if (s_maint) return ESP_ERR_INVALID_STATE;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(WHM_SD_MOUNT, &total, &freeb) != ESP_OK) {
        return ESP_FAIL;
    }
    *total_mb = (uint32_t)(total / (1024ULL * 1024ULL));
    *free_mb = (uint32_t)(freeb / (1024ULL * 1024ULL));
    return ESP_OK;
}

esp_err_t whm_storage_benchmark(void)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;

    const size_t chunk = 64 * 1024;
    const size_t total = 4 * 1024 * 1024;
    const char *path = WHM_SD_MOUNT "/whm_bench.bin";

    uint8_t *buf = malloc(chunk);
    if (!buf) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < chunk; i++) buf[i] = (uint8_t)(i * 7 + 13);

    /* ---- write ---- */
    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return ESP_FAIL; }
    int64_t t0 = esp_timer_get_time();
    size_t written = 0;
    while (written < total) {
        if (fwrite(buf, 1, chunk, f) != chunk) break;
        written += chunk;
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    int64_t t1 = esp_timer_get_time();

    /* ---- read ---- */
    f = fopen(path, "rb");
    if (!f) { free(buf); return ESP_FAIL; }
    int64_t t2 = esp_timer_get_time();
    size_t rd = 0;
    while (rd < written) {
        size_t got = fread(buf, 1, chunk, f);
        if (got == 0) break;
        rd += got;
    }
    fclose(f);
    int64_t t3 = esp_timer_get_time();
    unlink(path);
    free(buf);

    double wr_mbs = (double)written / 1e6 / ((double)(t1 - t0) / 1e6);
    double rd_mbs = (double)rd / 1e6 / ((double)(t3 - t2) / 1e6);
    ESP_LOGI(TAG, "bench: wrote %u KB @ %.2f MB/s, read @ %.2f MB/s "
                  "(pipeline needs 0.36 MB/s @ 64x64/30fps -> headroom %.0fx)",
             (unsigned)(written / 1024), wr_mbs, rd_mbs, rd_mbs / 0.36);
    return ESP_OK;
}
