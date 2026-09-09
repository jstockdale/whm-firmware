/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/*
 * settings.c - NVS-backed settings store (namespace "whm"), now fronted
 * by an internal-stacked BROKER task so PSRAM-stacked tasks (sync rx,
 * mp3) can read/write settings without touching spi_flash themselves:
 * any NVS access walks flash through a cache-disabled window in which
 * a PSRAM stack is unreachable (the 0.24.2 bootloop).
 *
 * Mechanics: public whm_settings_* become RPC clients - request on the
 * caller's stack, queued to the broker, caller blocks on a semaphore.
 * The broker STAGES everything through its own internal locals before
 * the NVS op (NVS reads your pointers DURING the flash op - handing it
 * PSRAM just moves the crash), and copies results back only after
 * flash settles. Direct path: before broker-up, or from the broker.
 * Hot-key cache: "node" is read once per announce per peer - served
 * from RAM after first load.
 */
#include "settings.h"

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "whm_set";
#define NS "whm"
#define VAL_MAX 144   /* 0.55.2: identity hex strings (sk=128) must
                         ride the broker - undersizing this silently
                         sent them down the DIRECT path from the sync
                         rx task and asserted the cache guard */

enum { OP_GSTR, OP_SSTR, OP_GU8, OP_SU8, OP_GU32, OP_SU32, OP_ERASE };

typedef struct {
    uint8_t op;
    char key[24];
    char sval[VAL_MAX];
    uint32_t uval;
    char *out;
    size_t out_len;
    uint8_t *out8;
    uint32_t *out32;
    SemaphoreHandle_t done;
    esp_err_t rc;
} nvsreq_t;

static QueueHandle_t s_q;
static TaskHandle_t s_broker;
static char s_name_cache[17];
static volatile bool s_name_valid;

/* ---- direct ops (broker context, or pre-broker boot) --------------- */

static esp_err_t d_get_str(const char *key, char *out, size_t out_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = out_len;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    if (err != ESP_OK && out_len) out[0] = 0;
    return err;
}

static esp_err_t d_set_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t d_get_u8(const char *key, uint8_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, key, out);
    nvs_close(h);
    return err;
}

static esp_err_t d_set_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t d_get_u32(const char *key, uint32_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u32(h, key, out);
    nvs_close(h);
    return err;
}

static esp_err_t d_set_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t d_erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---- the broker ---------------------------------------------------- */

static void broker_task(void *arg)
{
    (void)arg;
    nvsreq_t *r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        /* stage through INTERNAL locals - never hand NVS a PSRAM ptr */
        char key[16], sval[VAL_MAX];
        strlcpy(key, r->key, sizeof(key));
        uint8_t u8tmp = 0;
        uint32_t u32tmp = 0;
        esp_err_t rc = ESP_FAIL;
        switch (r->op) {
        case OP_GSTR: {
            char tmp[VAL_MAX];
            size_t want = r->out_len < VAL_MAX ? r->out_len : VAL_MAX;
            rc = d_get_str(key, tmp, want);
            /* LEGACY SEMANTICS: write only what direct wrote - the
               unconditional copy clobbered caller defaults with junk
               (and *r->out8=0 below turned brightness into BLACK). */
            if (rc == ESP_OK) {
                memcpy(r->out, tmp, strnlen(tmp, want - 1) + 1);
            } else if (r->out_len) {
                r->out[0] = 0;                  /* as d_get_str does */
            }
            break;
        }
        case OP_SSTR:
            strlcpy(sval, r->sval, sizeof(sval));
            rc = d_set_str(key, sval);
            break;
        case OP_GU8:
            rc = d_get_u8(key, &u8tmp);
            if (rc == ESP_OK) *r->out8 = u8tmp;  /* NOT_FOUND: default
                                                    stays, as always */
            break;
        case OP_SU8:
            rc = d_set_u8(key, (uint8_t)r->uval);
            break;
        case OP_GU32:
            rc = d_get_u32(key, &u32tmp);
            if (rc == ESP_OK) *r->out32 = u32tmp;
            break;
        case OP_SU32:
            rc = d_set_u32(key, r->uval);
            break;
        case OP_ERASE:
            rc = d_erase(key);
            break;
        }
        r->rc = rc;
        xSemaphoreGive(r->done);
    }
}

static bool use_direct(void)
{
    return !s_q || xTaskGetCurrentTaskHandle() == s_broker;
}

static esp_err_t rpc(nvsreq_t *r)
{
    StaticSemaphore_t sb;
    r->done = xSemaphoreCreateBinaryStatic(&sb);
    r->rc = ESP_FAIL;
    nvsreq_t *p = r;
    if (xQueueSend(s_q, &p, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    xSemaphoreTake(r->done, portMAX_DELAY);
    return r->rc;
}

/* ---- public API (signatures unchanged) ----------------------------- */

esp_err_t whm_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase (%s)",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK && !s_q) {
        s_q = xQueueCreate(4, sizeof(nvsreq_t *));
        if (s_q && xTaskCreate(broker_task, "nvs_broker", 3072, NULL, 9,
                               &s_broker) != pdPASS) {
            s_broker = NULL;
            ESP_LOGE(TAG, "broker task failed - PSRAM tasks must not "
                          "touch settings!");
        } else {
            printf("settings: NVS broker up (internal 3K) - "
                   "PSRAM-stacked tasks are flash-safe\n");
        }
    }
    return err;
}

esp_err_t whm_settings_get_str(const char *key, char *out, size_t out_len)
{
    if (strcmp(key, "node") == 0 && s_name_valid) {
        strlcpy(out, s_name_cache, out_len);
        return out[0] ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
    }
    esp_err_t rc;
    if (use_direct() || out_len > VAL_MAX) {
        rc = d_get_str(key, out, out_len);
    } else {
        nvsreq_t r = { .op = OP_GSTR, .out = out, .out_len = out_len };
        strlcpy(r.key, key, sizeof(r.key));
        rc = rpc(&r);
    }
    if (strcmp(key, "node") == 0) {
        strlcpy(s_name_cache, rc == ESP_OK ? out : "",
                sizeof(s_name_cache));
        s_name_valid = true;
    }
    return rc;
}

esp_err_t whm_settings_set_str(const char *key, const char *val)
{
    esp_err_t rc;
    if (use_direct() || strlen(val) >= VAL_MAX) {
        if (!use_direct())
            printf("settings: OVERSIZE set_str('%s', %u chars) "
                   "took the DIRECT path from a non-broker task - "
                   "raise VAL_MAX\n", key, (unsigned)strlen(val));
        rc = d_set_str(key, val);
    } else {
        nvsreq_t r = { .op = OP_SSTR };
        strlcpy(r.key, key, sizeof(r.key));
        strlcpy(r.sval, val, sizeof(r.sval));
        rc = rpc(&r);
    }
    if (rc == ESP_OK && strcmp(key, "node") == 0) {
        strlcpy(s_name_cache, val, sizeof(s_name_cache));
        s_name_valid = true;
    }
    return rc;
}

esp_err_t whm_settings_get_u8(const char *key, uint8_t *out)
{
    if (use_direct()) return d_get_u8(key, out);
    nvsreq_t r = { .op = OP_GU8, .out8 = out };
    strlcpy(r.key, key, sizeof(r.key));
    return rpc(&r);
}

esp_err_t whm_settings_set_u8(const char *key, uint8_t val)
{
    if (use_direct()) return d_set_u8(key, val);
    nvsreq_t r = { .op = OP_SU8, .uval = val };
    strlcpy(r.key, key, sizeof(r.key));
    return rpc(&r);
}

esp_err_t whm_settings_get_u32(const char *key, uint32_t *out)
{
    if (use_direct()) return d_get_u32(key, out);
    nvsreq_t r = { .op = OP_GU32, .out32 = out };
    strlcpy(r.key, key, sizeof(r.key));
    return rpc(&r);
}

esp_err_t whm_settings_set_u32(const char *key, uint32_t val)
{
    if (use_direct()) return d_set_u32(key, val);
    nvsreq_t r = { .op = OP_SU32, .uval = val };
    strlcpy(r.key, key, sizeof(r.key));
    return rpc(&r);
}

esp_err_t whm_settings_erase(const char *key)
{
    esp_err_t rc;
    if (use_direct()) {
        rc = d_erase(key);
    } else {
        nvsreq_t r = { .op = OP_ERASE };
        strlcpy(r.key, key, sizeof(r.key));
        rc = rpc(&r);
    }
    if (rc == ESP_OK && strcmp(key, "node") == 0) {
        s_name_cache[0] = 0;
        s_name_valid = true;
    }
    return rc;
}
