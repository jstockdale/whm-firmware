/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_check.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t whm_wifi_start(void);

/* runtime control (persists to NVS, applies immediately, no reboot) */
esp_err_t whm_wifi_join(const char *ssid, const char *pass);
esp_err_t whm_wifi_softap(const char *ssid, const char *pass); /* NULLs -> Kconfig defaults */
esp_err_t whm_wifi_clear(void);

/* introspection */
bool    whm_wifi_is_sta(void);
bool    whm_wifi_connected(void);
void    whm_wifi_status_print(void);
int     whm_wifi_scan_print(void);          /* returns AP count, -1 on error */
int64_t whm_wifi_tsf_now(void);
const char *whm_wifi_ssid(void);
int     whm_wifi_rssi(void);                 /* 127 when n/a */
esp_err_t whm_wifi_ip_str(char *out, size_t out_len);
int     whm_wifi_ap_stations(void);          /* -1 when not SoftAP */
bool    whm_wifi_drift(double *ppm);
const char *whm_wifi_ap_default_psk(void);

#ifdef __cplusplus
}
#endif
