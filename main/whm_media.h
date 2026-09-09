/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t whm_media_play(const char *path, int64_t start_tsf, int loop_ovr);
void      whm_media_stop(void);
bool      whm_media_active(void);           /* SD or LIVE engaged */
void      whm_media_tick(int64_t tf);       /* called by P_MEDIA @60fps */
int       whm_media_list(void);             /* prints /sdcard/media *.whm */

/* LIVE ingest (anchor HTTP + follower type-15) */
void whm_media_live_begin(void);
void whm_media_live_rx(uint16_t seq, uint8_t frag_i, uint8_t frag_n,
                       uint16_t w, uint16_t h, uint32_t pts_ms,
                       const uint8_t *payload, uint16_t plen,
                       int64_t rx_tsf);
void whm_media_live_local(uint16_t w, uint16_t h, uint32_t pts_ms,
                          const uint8_t *rle, uint32_t len);
