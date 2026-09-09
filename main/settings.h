/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t whm_settings_init(void);
esp_err_t whm_settings_get_str(const char *key, char *out, size_t out_len);
esp_err_t whm_settings_set_str(const char *key, const char *val);
esp_err_t whm_settings_get_u8(const char *key, uint8_t *out);
esp_err_t whm_settings_set_u8(const char *key, uint8_t val);
esp_err_t whm_settings_get_u32(const char *key, uint32_t *out);
esp_err_t whm_settings_set_u32(const char *key, uint32_t val);
esp_err_t whm_settings_erase(const char *key);
#ifdef __cplusplus
}
#endif
