/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

float       whm_ui_walk_cam(void);
void        whm_ui_ota_begin(const char *cur, const char *inc,
                             uint32_t total_kb);
void        whm_ui_ota_target(const char *inc);
void        whm_ui_ota_progress(uint32_t kb);
void        whm_ui_ota_phase(uint8_t ph, const char *why);
void        whm_ui_ota_count(uint8_t n);
bool        whm_ui_ota_active(void);
void        whm_ui_walk_params_poke(void);
uint32_t    whm_ui_walk_step(void);
void        whm_ui_walk_input_rx(uint8_t act, uint32_t exec_step,
                                 float arg);
void        whm_ui_sas_show(const char *digits);
void        whm_ui_sas_done(bool ok);
void        whm_ui_walk_pure(bool on);
void        whm_ui_walk_stats(uint32_t *ok, uint32_t *snap,
                              uint32_t *stale);
void        whm_ui_wkb_rx(uint8_t owner, float x, int8_t y,
                          uint8_t st, int8_t dir, uint16_t timer,
                          uint32_t step, int64_t btsf, float tgt,
                          float vx);
void        whm_ui_walk_speed(float v);
void        whm_ui_walk_speed_get(float *cur, float *tgt);
void        whm_ui_nye_join(const char *from, const char *tz,
                            int year, int64_t start_tsf);
void        whm_ui_fw_test(int mode, int fast);
void        whm_ui_fw_sparkle(int secs);
void        whm_ui_cam_set(float c);
void        whm_ui_scroll_q8(uint8_t *now8, uint8_t *tgt8);
void        whm_ui_walk_status(uint32_t *step, int *own,
                               float *cam2, float *x2);
void        whm_ui_task(void *arg);

/* "auto" resumes rotation, "next" advances, or a screen name to hold */
bool        whm_ui_screen_set(const char *name);
const char *whm_ui_screen_names(void);

/* console `text` payload; BOOT button or screen commands exit */
void        whm_ui_show_text(const char *msg);
void        whm_ui_text_cfg(bool span, uint8_t size, uint8_t scroll,
                            uint16_t speed);
void        whm_ui_text_clear(void);          /* resume prior screen */
void        whm_ui_overlay(const char *msg, bool span, uint8_t size,
                           uint8_t scroll, uint16_t speed, bool band,
                           uint8_t pos);      /* msg NULL/"" = clear */
bool        whm_ui_overlay_active(void);

/* pattern name holds it; "cycle" runs the validation tour */
bool        whm_ui_pattern_set(const char *name);
void        whm_ui_pattern_off(void);         /* resume prior screen+mode */
bool        whm_ui_pattern_active(const char **name);
const char *whm_ui_pattern_names(void);

/* countdown timer (runs across screens; expiry beeps + pulls focus) */
void     whm_timer_start(void);
void     whm_timer_pause(void);
void     whm_timer_stop(void);
void     whm_timer_reset(void);
void     whm_timer_set(uint32_t seconds);          /* one-off */
void     whm_timer_set_default(uint32_t seconds);  /* saved to NVS */
void     whm_timer_status(uint32_t *remain_s, uint32_t *total_s, int *state);
uint32_t whm_timer_get_default(void);
int         whm_ui_life_pal_count(void);
const char *whm_ui_life_pal_name(int i);
int         whm_ui_life_get_pal(void);
esp_err_t   whm_ui_life_set_pal(int i);
void        whm_ui_life_reseed(void);
void        whm_ui_flash_ok(void);
void        whm_ui_name_dirty(void);
void        whm_ui_oracle_ask(void);
void        whm_ui_music_layout(uint8_t lay, int8_t ovl);          /* re-read node name */            /* brief green success bar */
void        whm_ui_wander_get(uint8_t *idx, uint8_t *n, uint16_t *spd);
esp_err_t   whm_ui_wander_set(uint8_t idx, uint8_t n, uint16_t spd);

/* stopwatch */
void     whm_watch_start(void);
void     whm_watch_stop(void);
void     whm_watch_reset(void);
int64_t  whm_watch_elapsed_us(bool *running);

#ifdef __cplusplus
}
#endif

void whm_ui_pattern_media(void);
uint8_t whm_ui_strip_idx(void);
