/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_check.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t whm_storage_mount(void);
esp_err_t whm_storage_unmount(void);
void      whm_storage_start_watcher(void);
bool      whm_storage_mounted(void);
esp_err_t whm_storage_benchmark(void);
void      whm_storage_info(void);
esp_err_t whm_storage_ls(const char *path);   /* NULL -> card root */
esp_err_t whm_storage_stats(uint32_t *total_mb, uint32_t *free_mb);
void      whm_storage_card_desc(char *out, size_t out_len);
esp_err_t whm_storage_card_serial(uint32_t *out);
esp_err_t whm_storage_format(void);   /* destructive - confirm first */
bool      whm_storage_maintenance(void); /* long op in progress (format) */
#ifdef __cplusplus
}
#endif
