#pragma once
#include <stdint.h>
void mon_ws_start(void);
void mon_ws_feed(const uint8_t *b, int n, uint32_t src_ip);
