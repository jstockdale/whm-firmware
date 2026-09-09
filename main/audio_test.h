/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include "esp_err.h"
#include "esp_check.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t whm_audio_test(void);
esp_err_t whm_audio_mic_start(void);
esp_err_t whm_audio_mic_read_rms(float *l, float *r);
void      whm_audio_mic_stop(void);
bool      whm_audio_mic_active(void);
void      whm_audio_beep_async(void);   /* 3 short beeps, own task */
void      whm_audio_chime_async(void);  /* boot chime, ~1.1s, own task */
esp_err_t whm_audio_tone_async(float hz, int ms);  /* one enveloped note */
uint8_t   whm_audio_sys_vol(void);        /* beep/chime/tone volume (NVS) */
esp_err_t whm_audio_set_sys_vol(uint8_t v);
/* single-owner speaker path (music player holds it during playback) */
esp_err_t whm_audio_spk_open(uint32_t rate, uint8_t ch, uint8_t vol_pct);
esp_err_t whm_audio_spk_write(const void *data, size_t len);
void      whm_audio_spk_setvol(uint8_t vol_pct);
void      whm_audio_spk_close(void);
bool      whm_audio_spk_busy(void);
#ifdef __cplusplus
}
#endif
