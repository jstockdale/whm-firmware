/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 5x8 font (5x7 + descender row), rendered PROPORTIONALLY: per-glyph ink
 * width + 1*scale gap; digits are tabular (full 5-cell) so numbers do not
 * jiggle. Line pitch >= 8*scale. gfx_char returns the advance used. */
void     gfx_set_clip(uint16_t x0, uint16_t x1);  /* [x0,x1) window */
void     gfx_clear_clip(void);
uint16_t gfx_char(int16_t x, uint16_t y, char c, uint8_t scale,
                  uint8_t r, uint8_t g, uint8_t b);
void     gfx_text(int16_t x, uint16_t y, const char *s, uint8_t scale,
                  uint8_t r, uint8_t g, uint8_t b);
uint16_t gfx_text_width(const char *s, uint8_t scale);
void     gfx_text_center(uint16_t cx, uint16_t y, const char *s, uint8_t scale,
                         uint8_t r, uint8_t g, uint8_t b);
#ifdef __cplusplus
}
#endif
