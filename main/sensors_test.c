/*
 * sensors_test.c - I2C bus + sensor bring-up.
 *
 * Uses the same registry components as Waveshare's IDF example, with the same
 * call sequences (waveshare/qmi8658, pedrominatel/shtc3, waveshare/pcf85063a).
 * Adds a full bus scan first so a dead device is obvious immediately.
 */
#include "sensors_test.h"
#include "whm_board.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"

#include "qmi8658.h"
#include "shtc3.h"
#include "pcf85063a.h"

static const char *TAG = "whm_sens";

static i2c_master_bus_handle_t s_bus = NULL;
static qmi8658_dev_t s_qmi;
static bool s_qmi_ok = false;
static i2c_master_dev_handle_t s_shtc3 = NULL;
static pcf85063a_dev_t s_rtc;
static bool s_rtc_ok = false;
static float s_gbias[3] = {0, 0, 0};
static bool s_gbias_ok = false;

i2c_master_bus_handle_t whm_i2c_bus_handle(void)
{
    return s_bus;
}

static esp_err_t bus_init(void)
{
    if (s_bus) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = WHM_I2C_PORT,
        .sda_io_num = WHM_I2C_SDA,
        .scl_io_num = WHM_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_bus);
}

void whm_i2c_scan(void)
{
    if (bus_init() != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return;
    }
    char line[128];
    int n = 0;
    n += snprintf(line + n, sizeof(line) - n, "i2c devices:");
    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            n += snprintf(line + n, sizeof(line) - n, " 0x%02X", addr);
            found++;
            if (n > (int)sizeof(line) - 8) break;
        }
    }
    ESP_LOGI(TAG, "%s (%d found; expect QMI8658 0x6B, SHTC3 0x70, "
                  "PCF85063 0x51, ES8311 0x18, ES7210 0x40/0x41)", line, found);
}

esp_err_t whm_sensors_init(void)
{
    ESP_RETURN_ON_ERROR(bus_init(), TAG, "bus init");

    /* IMU */
    esp_err_t err = qmi8658_init(&s_qmi, s_bus, QMI8658_ADDRESS_HIGH);
    if (err == ESP_OK) {
        qmi8658_set_accel_unit_mps2(&s_qmi, true);
        qmi8658_set_gyro_unit_rads(&s_qmi, true);
        err = qmi8658_enable_sensors(&s_qmi, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    }
    s_qmi_ok = (err == ESP_OK);
    ESP_LOGI(TAG, "QMI8658 IMU: %s", s_qmi_ok ? "OK" : esp_err_to_name(err));

    if (s_qmi_ok) {
        /* zero-rate gyro calibration: board assumed still at boot.
           ~300ms; bench unit showed -0.2 rad/s raw bias on Y. */
        vTaskDelay(pdMS_TO_TICKS(60));
        double acc[3] = {0, 0, 0};
        int got = 0;
        for (int i = 0; i < 60; i++) {
            qmi8658_data_t d = {0};
            if (qmi8658_read_sensor_data(&s_qmi, &d) == ESP_OK) {
                acc[0] += d.gyroX; acc[1] += d.gyroY; acc[2] += d.gyroZ;
                got++;
            }
            vTaskDelay(pdMS_TO_TICKS(4));
        }
        if (got > 30) {
            for (int i = 0; i < 3; i++) s_gbias[i] = (float)(acc[i] / got);
            s_gbias_ok = true;
            ESP_LOGI(TAG, "gyro zero-rate bias: %+.3f %+.3f %+.3f rad/s "
                          "(subtracted; slow-tracked when still)",
                     s_gbias[0], s_gbias[1], s_gbias[2]);
        }
    }

    /* SHTC3: dedicated device handle at reduced clock (per vendor middleware) */
    i2c_device_config_t shtc3_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_I2C_ADDR,
        .scl_speed_hz = 40000,
        .scl_wait_us = 10000,
    };
    err = i2c_master_bus_add_device(s_bus, &shtc3_cfg, &s_shtc3);
    ESP_LOGI(TAG, "SHTC3 T/RH: %s", err == ESP_OK ? "OK" : esp_err_to_name(err));

    /* RTC */
    err = pcf85063a_init(&s_rtc, s_bus, PCF85063A_ADDRESS);
    s_rtc_ok = (err == ESP_OK);
    ESP_LOGI(TAG, "PCF85063 RTC: %s", s_rtc_ok ? "OK" : esp_err_to_name(err));

    if (s_rtc_ok) {
        pcf85063a_datetime_t t;
        if (pcf85063a_get_time_date(&s_rtc, &t) == ESP_OK) {
            if (t.year < 2025 || t.year > 2099) {
                /* Battery-less or first boot: seed something sane so
                   scheduling code downstream has a monotonic-ish wall clock */
                pcf85063a_datetime_t seed = {
                    .sec = 0, .min = 0, .hour = 12,
                    .day = 1, .dotw = 4, .month = 1, .year = 2026,
                };
                pcf85063a_set_time_date(&s_rtc, seed);
                ESP_LOGW(TAG, "RTC time invalid (year=%u), seeded 2026-01-01 12:00",
                         (unsigned)t.year);
            } else {
                ESP_LOGI(TAG, "RTC reads %04u-%02u-%02u %02u:%02u:%02u",
                         (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
                         (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
            }
        }
    }
    return ESP_OK;
}

void whm_sensors_status(bool *imu, bool *env, bool *rtc)
{
    if (imu) *imu = s_qmi_ok;
    if (env) *env = (s_shtc3 != NULL);
    if (rtc) *rtc = s_rtc_ok;
}

esp_err_t whm_sensors_read_imu(float *ax, float *ay, float *az,
                               float *gx, float *gy, float *gz)
{
    if (!s_qmi_ok) return ESP_ERR_INVALID_STATE;
    qmi8658_data_t d = {0};
    esp_err_t err = qmi8658_read_sensor_data(&s_qmi, &d);
    if (err != ESP_OK) return err;
    float g[3] = {d.gyroX, d.gyroY, d.gyroZ};
    if (s_gbias_ok) {
        bool still = true;
        for (int i = 0; i < 3; i++) {
            if (__builtin_fabsf(g[i] - s_gbias[i]) > 0.06f) still = false;
        }
        if (still) {
            for (int i = 0; i < 3; i++) {
                s_gbias[i] += 0.02f * (g[i] - s_gbias[i]);
            }
        }
        for (int i = 0; i < 3; i++) g[i] -= s_gbias[i];
    }
    *ax = d.accelX; *ay = d.accelY; *az = d.accelZ;
    *gx = g[0]; *gy = g[1]; *gz = g[2];
    return ESP_OK;
}

esp_err_t whm_sensors_read_th(float *temp_c, float *rh)
{
    if (!s_shtc3) return ESP_ERR_INVALID_STATE;
    return shtc3_get_th(s_shtc3, SHTC3_REG_T_CSE_NM, temp_c, rh);
}

/* Sakamoto's day-of-week, 0 = Sunday */
static uint8_t day_of_week(uint16_t y, uint8_t m, uint8_t d)
{
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (uint8_t)((y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7);
}

esp_err_t whm_sensors_set_rtc(uint16_t y, uint8_t mo, uint8_t d,
                              uint8_t h, uint8_t mi, uint8_t sec)
{
    if (!s_rtc_ok) return ESP_ERR_INVALID_STATE;
    if (y < 2000 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
        h > 23 || mi > 59 || sec > 59) return ESP_ERR_INVALID_ARG;
    pcf85063a_datetime_t t = {
        .sec = sec, .min = mi, .hour = h,
        .day = d, .dotw = day_of_week(y, mo, d), .month = mo, .year = y,
    };
    return pcf85063a_set_time_date(&s_rtc, t);
}

esp_err_t whm_sensors_read_rtc_parts(uint16_t *y, uint8_t *mo, uint8_t *d,
                                     uint8_t *h, uint8_t *mi, uint8_t *s)
{
    if (!s_rtc_ok) return ESP_ERR_INVALID_STATE;
    pcf85063a_datetime_t t;
    ESP_RETURN_ON_ERROR(pcf85063a_get_time_date(&s_rtc, &t), TAG, "rtc read");
    *y = t.year; *mo = t.month; *d = t.day;
    *h = t.hour; *mi = t.min; *s = t.sec;
    return ESP_OK;
}

esp_err_t whm_sensors_read_rtc(char *out, size_t out_len)
{
    if (!s_rtc_ok) return ESP_ERR_INVALID_STATE;
    pcf85063a_datetime_t t;
    ESP_RETURN_ON_ERROR(pcf85063a_get_time_date(&s_rtc, &t), TAG, "rtc read");
    snprintf(out, out_len, "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
             (unsigned)t.hour, (unsigned)t.min, (unsigned)t.sec);
    return ESP_OK;
}
