/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#ifdef __cplusplus
extern "C" {
#endif
i2c_master_bus_handle_t whm_i2c_bus_handle(void);
void      whm_sensors_status(bool *imu, bool *env, bool *rtc);
void      whm_i2c_scan(void);
esp_err_t whm_sensors_init(void);
esp_err_t whm_sensors_read_imu(float *ax, float *ay, float *az,
                               float *gx, float *gy, float *gz);
esp_err_t whm_sensors_read_th(float *temp_c, float *rh);
esp_err_t whm_sensors_read_rtc(char *out, size_t out_len);
esp_err_t whm_sensors_set_rtc(uint16_t y, uint8_t mo, uint8_t d,
                              uint8_t h, uint8_t mi, uint8_t sec);
esp_err_t whm_sensors_read_rtc_parts(uint16_t *y, uint8_t *mo, uint8_t *d,
                                     uint8_t *h, uint8_t *mi, uint8_t *s);
#ifdef __cplusplus
}
#endif
