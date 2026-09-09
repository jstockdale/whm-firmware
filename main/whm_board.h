/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/*
 * whm_board.h - Waveshare ESP32-S3-RGB-Matrix board definition
 *
 * Single source of truth for board wiring. Every value below was extracted
 * from Waveshare's official repo (waveshareteam/ESP32-S3-RGB-Matrix):
 *   - HUB75 pins:   example/idf_v5.5.2/sdkconfig.defaults (matches the
 *                   Arduino sketch's explicit pin struct, E=9)
 *   - I2C/I2S/SD/PA: example/idf_v5.5.2/components/bsp/.../include/bsp/config.h
 *
 * Chip: ESP32-S3-N32R16 (32MB flash, 16MB octal PSRAM)
 */
#pragma once

#include "driver/gpio.h"

/* ---------------- HUB75 (fixed board routing, via 74HC245 buffer) -------- */
#define WHM_HUB75_R1    4
#define WHM_HUB75_G1    5
#define WHM_HUB75_B1    6
#define WHM_HUB75_R2    7
#define WHM_HUB75_G2    15
#define WHM_HUB75_B2    16
#define WHM_HUB75_A     18
#define WHM_HUB75_B     8
#define WHM_HUB75_C     3
#define WHM_HUB75_D     42
#define WHM_HUB75_E     9      /* required: 1/32-scan 64x64 HUB75E panels   */
#define WHM_HUB75_LAT   40
#define WHM_HUB75_OE    2
#define WHM_HUB75_CLK   41

/* Panel: Adafruit #3649-class, P2.5 64x64, 1/32 scan, FM6126A-family driver.
 * FM6126A init sequence is what the known-good Waveshare Arduino demo uses. */
#define WHM_PANEL_W     64
#define WHM_PANEL_H     64

/* ---------------- I2C bus (sensors + codec control) ----------------------- */
#define WHM_I2C_PORT    0
#define WHM_I2C_SDA     GPIO_NUM_47
#define WHM_I2C_SCL     GPIO_NUM_48

/* On this bus: QMI8658 IMU, SHTC3 temp/RH, PCF85063 RTC, ES8311, ES7210 */

/* ---------------- I2S (shared duplex: ES8311 DAC out, ES7210 ADC in) ------ */
#define WHM_I2S_PORT    I2S_NUM_0
#define WHM_I2S_MCLK    GPIO_NUM_12
#define WHM_I2S_BCLK    GPIO_NUM_43
#define WHM_I2S_WS      GPIO_NUM_38
#define WHM_I2S_DOUT    GPIO_NUM_21   /* -> ES8311 DAC                       */
#define WHM_I2S_DIN     GPIO_NUM_39   /* <- ES7210 mic ADC                   */

#define WHM_PA_ENABLE   GPIO_NUM_11   /* speaker amp enable (active high)    */
#define WHM_PA_REVERTED false

/* ---------------- SD card: SDMMC 1-bit ------------------------------------ */
#define WHM_SD_CLK      GPIO_NUM_1
#define WHM_SD_CMD      GPIO_NUM_44
#define WHM_SD_D0       GPIO_NUM_17
#define WHM_SD_WIDTH    1
#define WHM_SD_MOUNT    "/sdcard"

/* ---------------- Misc ----------------------------------------------------- */
#define WHM_BUTTON_BOOT GPIO_NUM_0
