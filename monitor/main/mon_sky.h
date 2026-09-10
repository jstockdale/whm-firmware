#pragma once
#include <stdint.h>
void  mon_sky_init(void);          /* loads tz from NVS */
float mw_daylight(void);
void  mw_sky(int64_t t, float cam, float f);
void  mw_flora(float cam, float f, int64_t t);
void  mw_birds(int64_t t, float cam, float f);
