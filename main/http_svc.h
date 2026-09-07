#pragma once
#include "esp_err.h"
esp_err_t whm_http_start(void);
esp_err_t whm_media_sync(const char *host);
void      whm_http_media_dirty(void);
esp_err_t whm_http_media_lookup(const char *name, char sha_out[65],
                                long *size);
