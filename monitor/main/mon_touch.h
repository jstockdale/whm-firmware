#pragma once
#include <stdint.h>
typedef struct { volatile uint8_t pressed;
                 volatile uint16_t x, y;    /* landscape coords */
                 volatile uint32_t seq; } mon_touch_t;
extern mon_touch_t g_touch;
void mon_touch_init(void);
