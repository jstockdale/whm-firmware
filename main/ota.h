#pragma once
#include "esp_err.h"
esp_err_t whm_ota_from_url(const char *url_or_host);
void      whm_ota_status_print(void);
