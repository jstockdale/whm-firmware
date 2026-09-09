#pragma once
#include <stdint.h>
void mon_lcd_init(void);
void mon_lcd_push_full(const uint16_t *fb);
void mon_lcd_brightness(uint8_t level);
