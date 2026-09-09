#pragma once
#include "esp_err.h"
esp_err_t mon_wifi_join(const char *ssid, const char *pass);
void      mon_wifi_clear(void);
void      mon_wifi_status(void);
void      mon_wifi_boot(void);   /* apply saved creds if present */
