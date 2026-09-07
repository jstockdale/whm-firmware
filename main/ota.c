/* ota.c - over-the-air updates riding the P2c HTTP layer.
 *
 *   ota <url|host>   pull an app image, validate, stage it as the boot
 *                    partition. Reboot is YOURS - staging and switching
 *                    are deliberately separate acts ('ota X && reboot'
 *                    fuses them via the chain operators).
 *   ota status       running partition + version, and what's staged.
 *
 * Host sugar: 'ota one.local' -> http://one.local/fw - any node's /fw
 * endpoint streams its own running partition, so one cable-flashed
 * unit seeds the fleet.
 *
 * Safety: esp_ota_end performs the full image validation (magic,
 * segments, appended SHA-256) and set_boot happens ONLY after it
 * passes; the staged image's embedded version is printed first so you
 * see what you're about to run. No signing/rollback yet - LAN bench
 * tool; hardening is P4.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "ota.h"

static const char *TAG = "whm_ota";

esp_err_t whm_ota_from_url(const char *arg)
{
    char url[160];
    if (strstr(arg, "://")) {
        strlcpy(url, arg, sizeof(url));
    } else if (strchr(arg, '/')) {
        snprintf(url, sizeof(url), "http://%s", arg);
    } else {
        snprintf(url, sizeof(url), "http://%s/fw", arg);
    }

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) {
        printf("ota: no update partition?!\n");
        return ESP_FAIL;
    }
    printf("ota: %s -> %s (keep power on; ~10-60s)\n", url, dst->label);

    esp_http_client_config_t cc = { .url = url, .timeout_ms = 15000 };
    esp_http_client_handle_t h = esp_http_client_init(&cc);
    if (!h) return ESP_FAIL;

    esp_ota_handle_t oh = 0;
    esp_err_t err = ESP_FAIL;
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc(4096);
    do {
        if (!buf) break;
        if (esp_http_client_open(h, 0) != ESP_OK) {
            printf("ota: connect failed\n");
            break;
        }
        int64_t cl = esp_http_client_fetch_headers(h);
        int status = esp_http_client_get_status_code(h);
        if (status != 200) {
            printf("ota: http %d\n", status);
            break;
        }
        err = esp_ota_begin(dst, OTA_SIZE_UNKNOWN, &oh);
        if (err != ESP_OK) {
            printf("ota: begin failed: %s\n", esp_err_to_name(err));
            oh = 0;
            break;
        }
        long got = 0, mark = 0;
        int n;
        err = ESP_OK;
        while ((n = esp_http_client_read(h, buf, 4096)) > 0) {
            err = esp_ota_write(oh, buf, (size_t)n);
            if (err != ESP_OK) {
                printf("ota: write failed at %ld: %s\n", got,
                       esp_err_to_name(err));
                break;
            }
            got += n;
            if (got - mark >= 262144) {
                if (cl > 0) {
                    printf("ota: %ld/%lld KB\n", got / 1024, cl / 1024);
                } else {
                    printf("ota: %ld KB\n", got / 1024);
                }
                mark = got;
            }
        }
        if (err != ESP_OK) break;
        if (got == 0) {
            printf("ota: empty response\n");
            err = ESP_FAIL;
            break;
        }
        printf("ota: %ld KB received, validating...\n", got / 1024);
        err = esp_ota_end(oh);
        oh = 0;                          /* consumed either way */
        if (err != ESP_OK) {
            printf("ota: image INVALID: %s (boot unchanged)\n",
                   esp_err_to_name(err));
            break;
        }
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(dst, &d) == ESP_OK) {
            printf("ota: validated %s v%s (built %s %s)\n",
                   d.project_name, d.version, d.date, d.time);
        }
        err = esp_ota_set_boot_partition(dst);
        if (err != ESP_OK) {
            printf("ota: set_boot failed: %s\n", esp_err_to_name(err));
            break;
        }
        printf("ota: staged on %s - 'reboot' when ready\n", dst->label);
        ESP_LOGI(TAG, "staged %s on %s", d.version, dst->label);
        err = ESP_OK;
    } while (0);

    if (oh) esp_ota_abort(oh);
    free(buf);
    esp_http_client_cleanup(h);
    return err;
}

void whm_ota_status_print(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *me = esp_app_get_description();
    printf("running:  %s  %s v%s (built %s %s)\n",
           run ? run->label : "?", me->project_name, me->version,
           me->date, me->time);
    if (boot && run && boot != run) {
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(boot, &d) == ESP_OK) {
            printf("staged:   %s  v%s - reboots into this\n",
                   boot->label, d.version);
        } else {
            printf("staged:   %s (unreadable?)\n", boot->label);
        }
    } else {
        printf("staged:   (none - boot == running)\n");
    }
    printf("next slot: %s\n", next ? next->label : "?");
    printf("usage: ota <host|url>   ('ota one.local' pulls that node's "
           "running image)\n");
}
