#pragma once
#include <stdint.h>
#include <stdbool.h>
/* Pure world functions reproduced VERBATIM from main/ui.c
 * (read 2026-09-09). Second native consumer - RSK rule-of-three.
 * NYE plaza window is the sole omission (not simulated here).
 * Golden-hash lock arrives with RSK M2. */
#define WK_GROUND 57
#define WK_REACH  21
enum { WK_WALK, WK_CLIMB, WK_LADDER, WK_SLIDE, WK_FALL, WK_IDLE,
       WK_CROUCH, WK_JUMP, WK_LAND,
       WK_GOCHAIR, WK_SETUP, WK_SIT, WK_PACK,
       WK_SHIMMY, WK_POP, WK_BASE,
       WK_GOFIRE, WK_FIREB, WK_FIRES, WK_FIRED,
       WK_GOHAM, WK_HAMS, WK_HAMI, WK_HAMD };
typedef struct { int32_t x; uint8_t w, y; } wplat_t;
typedef struct { int32_t x; uint8_t ytop, ybot; } wlad_t;
typedef struct { int32_t x; uint8_t ytop, ybot; int8_t dir; } wsli_t;
typedef struct {
    int32_t id;
    wplat_t p[5]; uint8_t np;
    wlad_t  l[3]; uint8_t nl;
    wsli_t  s[2]; uint8_t ns;
    int32_t gap_x; uint8_t gap_w;
    bool bridged;
    int32_t house_x;
} wchunk_t;
uint32_t mw_h(uint32_t v);
const wchunk_t *mw_chunk(int32_t id);
void mw_hsv(uint16_t h, uint8_t s, uint8_t v,
            uint8_t *r, uint8_t *g, uint8_t *b);
