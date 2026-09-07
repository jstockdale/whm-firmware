/* display_hal.h - thin C API over the esp-hub75 C++ driver */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool     whm_display_init(uint8_t initial_brightness);
void     whm_display_clear(void);
void     whm_display_fill(uint8_t r, uint8_t g, uint8_t b);
void     whm_display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint8_t r, uint8_t g, uint8_t b);
void     whm_display_set_pixel(uint16_t x, uint16_t y,
                               uint8_t r, uint8_t g, uint8_t b);
/* buf: tightly packed RGB888, w*h*3 bytes */
void     whm_display_blit_rgb888(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                 const uint8_t *buf);
void     whm_display_flip(void);
void     whm_display_set_brightness(uint8_t b);
uint16_t whm_display_width(void);
uint16_t whm_display_height(void);
bool     whm_display_running(void);

#ifdef __cplusplus
}
#endif
