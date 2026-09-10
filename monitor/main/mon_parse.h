#pragma once
#include <stdint.h>
/* Wire structs per WHM-LINK.md as-built addendum (2026-09-09).
   Passive listener: seal tail (+16) is length-accepted, ignored. */
typedef struct __attribute__((packed)) {
    char magic[4]; uint8_t ver; uint8_t type;      /* 9 */
    uint8_t owner; uint8_t st; int8_t dir; int8_t yq1;
    uint16_t timer; uint32_t step; float x;
    char from[16]; int64_t tsf;
    float tgt, vx, vy, spd;
    int8_t sdir; uint8_t phase; uint8_t turn_cd; uint8_t fresh;
} mon_wkb_t;
typedef struct __attribute__((packed)) {
    char magic[4]; uint8_t ver; uint8_t type;      /* 10 */
    uint16_t rsv; int64_t anchor; float cam;
    uint8_t wver; uint8_t strips; uint16_t scroll_q8;
} mon_wkp_t;
typedef struct __attribute__((packed)) {
    char magic[4]; uint8_t ver; uint8_t type;      /* 17 */
    uint8_t f_q8; uint8_t rsv; uint16_t minod; uint16_t rsv2;
} mon_day_t;
_Static_assert(sizeof(mon_day_t) == 12, "wkday wire");
typedef struct __attribute__((packed)) {
    char magic[4]; uint8_t ver; uint8_t type;      /* 16 */
    uint8_t a8; uint8_t rsv; uint32_t step;
    uint64_t h; uint16_t hs; uint16_t rsv2;
} mon_org_t;
_Static_assert(sizeof(mon_wkb_t) == 64, "wkb wire size");
_Static_assert(sizeof(mon_wkp_t) == 24, "wkp wire size");
_Static_assert(sizeof(mon_org_t) == 24, "org wire size");
typedef struct {
    /* latest pose */
    uint32_t step; float x, y, vx, vy, spd, tgt;
    uint8_t st, owner, phase; int8_t dir, sdir; uint16_t timer;
    char from[16];
    /* cam + fleet */
    float cam; uint8_t strips; int64_t anchor;
    /* dayclock (type 17): fleet-authoritative sky */
    uint8_t day_fq8; uint16_t day_min; int64_t day_us;
    uint32_t n_real, n_promise;
    mon_wkb_t ring16[16]; uint32_t ring16_step[16];
    uint32_t n_rx9; uint32_t raw8[8]; uint8_t raw8_i;
    uint32_t dh[4];   /* accept deltas: 1,2,3,>3 */
    /* oracle */
    uint32_t rung_step; uint64_t rung_h; uint16_t rung_hs;
    /* counters */
    int64_t tsf_off; uint8_t tsf_ok;    /* passive fleet clock */
    uint32_t n_kf, n_wkp, n_org, n_drop;
    struct { uint32_t step; uint8_t st, owner; } tr[5];
    uint8_t tr_n, last_st;
    struct { uint32_t step; uint16_t h16; } rung[8];
    uint8_t rung_n;
    uint8_t kfs_ring[120], rxs_ring[120]; uint8_t ring_i;
    int64_t last_kf_us;
} mon_state_t;
extern mon_state_t g_mon;
void mon_parse_pkt(const uint8_t *b, int n);
int64_t mon_now(void);   /* fleet-shared time (TSF slaved via kf) */
const char *mon_st_name(uint8_t st);
