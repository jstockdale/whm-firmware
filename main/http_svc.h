/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include "esp_err.h"
esp_err_t whm_http_start(void);
esp_err_t whm_media_sync(const char *host);
void      whm_http_media_dirty(void);
esp_err_t whm_http_media_lookup(const char *name, char sha_out[65],
                                long *size);
