#include <string.h>
#include "esp_timer.h"
#include "mon_parse.h"
mon_state_t g_mon;
static const char *STN[16] = {
    "WALK","CLMB","LADR","SLID","FALL","IDLE","CRCH","JUMP",
    "LAND","GOCH","SETP","SIT","PACK","SHIM","POP","BASE" };
const char *mon_st_name(uint8_t st)
{
    return st < 16 ? STN[st] : "????";
}
static int len_ok(int n, int base)
{
    return n == base || n == base + 16;   /* sealed tail ignored */
}
void mon_parse_pkt(const uint8_t *b, int n)
{
    if (n < 8 || memcmp(b, "WHML", 4) != 0 || b[4] != 2) {
        g_mon.n_drop++; return;
    }
    switch (b[5]) {
    case 9:
        if (!len_ok(n, (int)sizeof(mon_wkb_t))) { g_mon.n_drop++; return; }
        {
            mon_wkb_t k; memcpy(&k, b, sizeof(k));
            g_mon.step = k.step; g_mon.x = k.x;
            g_mon.y = (float)k.yq1 * 0.5f;
            g_mon.vx = k.vx; g_mon.vy = k.vy; g_mon.spd = k.spd;
            g_mon.tgt = k.tgt; g_mon.st = k.st; g_mon.owner = k.owner;
            g_mon.dir = k.dir; g_mon.sdir = k.sdir;
            g_mon.phase = k.phase; g_mon.timer = k.timer;
            memcpy(g_mon.from, k.from, 16); g_mon.from[15] = 0;
            if (k.st != g_mon.last_st) {
                g_mon.last_st = k.st;
                g_mon.tr[g_mon.tr_n].step = k.step;
                g_mon.tr[g_mon.tr_n].st = k.st;
                g_mon.tr[g_mon.tr_n].owner = k.owner;
                g_mon.tr_n = (uint8_t)((g_mon.tr_n + 1) % 5);
            }
            g_mon.n_kf++; g_mon.last_kf_us = esp_timer_get_time();
        }
        break;
    case 10:
        if (!len_ok(n, (int)sizeof(mon_wkp_t))) { g_mon.n_drop++; return; }
        {
            mon_wkp_t p; memcpy(&p, b, sizeof(p));
            g_mon.cam = p.cam; g_mon.strips = p.strips;
            g_mon.anchor = p.anchor; g_mon.n_wkp++;
        }
        break;
    case 16:
        if (!len_ok(n, (int)sizeof(mon_org_t))) { g_mon.n_drop++; return; }
        {
            mon_org_t o; memcpy(&o, b, sizeof(o));
            g_mon.rung_step = o.step; g_mon.rung_h = o.h;
            g_mon.rung_hs = o.hs; g_mon.n_org++;
        }
        break;
    default: break;                        /* other types: not ours */
    }
}
