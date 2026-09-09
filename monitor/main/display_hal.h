#pragma once
#include <stdint.h>
#include <stdbool.h>
/* Monitor-side shim: gfx5x7.c (verbatim from main/) plots through
 * this one hook; here it lands in the PSRAM framebuffer. */
void whm_display_fill_rect(uint16_t x, uint16_t y,
                           uint16_t w, uint16_t h,
                           uint8_t r, uint8_t g, uint8_t b);
