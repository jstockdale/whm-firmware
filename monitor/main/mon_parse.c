#include <string.h>
#include "esp_timer.h"
#include "mon_parse.h"
mon_state_t g_mon;
int64_t mon_now(void)
{
    return esp_timer_get_time() +
           (g_mon.tsf_ok ? g_mon.tsf_off : 0);
}
static const char *STN[16] = {
    "WALK","CLMB","LADR","SLID","FALL","IDLE","CRCH","JUMP",
    "LAND","GOCH","SETP","SIT","PACK","SHIM","POP","BASE",
    "GOFR","FIRB","FIRS","FIRD","GOHM","HAMS","HAMI","HAMD" };
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
            /* THE INVERTED MODEL, confessed: there is ONE
               type-9 send site in the fleet, and it fires AFTER
               the sandbox restore with future-snapshot variables
               - EVERY keyframe is a promise, fanned across the
               horizon each tick (the honest 170/s). The
               "two-lane" classifier hunted a +1 real chain that
               does not exist and starved every display to the
               resync valve. Restored law: first frame per step
               wins (dedups the fan, kf/s ~30); a step-keyed ring
               below serves maxstep-5 so observers render ON
               glass time instead of ~0.17 s ahead. */
            g_mon.n_rx9++;
            g_mon.raw8[g_mon.raw8_i] = k.step;
            g_mon.raw8_i = (uint8_t)((g_mon.raw8_i + 1) % 8);
            /* THE ORDERED BANK (owner's UDP directive): never
               discard a frame for arrival order. The old
               monotonic gate rode the LEADING EDGE of
               step-space - under UDP reordering of the fan
               bursts a late lower step was thrown away
               forever, so accepted steps advanced in jumps
               of ~3 and the figure strobed at a third of its
               poses (upd/s 23, eyes 3-5). Now: every valid
               frame BANKS into its step slot, first-wins per
               exact step (dual-sender dedupe preserved
               without ordering loss); the ui-side player
               adopts steps IN ORDER. */
            if (g_mon.max_banked &&
                (k.step > g_mon.max_banked + 1000 ||
                 g_mon.max_banked > k.step + 1000)) {
                memset(g_mon.ring16_step, 0,
                       sizeof(g_mon.ring16_step));
                g_mon.max_banked = 0;
                g_mon.play_cursor = 0;      /* epoch turn */
            }
            {
                int sl9 = (int)(k.step & 15u);
                if (g_mon.ring16_step[sl9] == k.step)
                    return;                  /* dup: first wins */
                g_mon.ring16[sl9] = k;
                g_mon.ring16_step[sl9] = k.step;
                static uint32_t prev_b;
                if (prev_b && k.step > prev_b) {
                    uint32_t d9 = k.step - prev_b;
                    g_mon.dh[d9 >= 4 ? 3 : d9 - 1]++;
                }
                if (k.step > prev_b) prev_b = k.step;
                if (k.step > g_mon.max_banked)
                    g_mon.max_banked = k.step;
            }
            {   /* TSF slave rides every banked frame */
                int64_t off = k.tsf - esp_timer_get_time();
                if (!g_mon.tsf_ok) { g_mon.tsf_off = off;
                    g_mon.tsf_ok = 1; }
                else g_mon.tsf_off += (off - g_mon.tsf_off) / 8;
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
    case 17:
        if (!len_ok(n, (int)sizeof(mon_day_t))) { g_mon.n_drop++;
            return; }
        {
            mon_day_t d; memcpy(&d, b, sizeof(d));
            g_mon.day_fq8 = d.f_q8; g_mon.day_min = d.minod;
            g_mon.day_us = esp_timer_get_time();
        }
        break;
    case 16:
        if (!len_ok(n, (int)sizeof(mon_org_t))) { g_mon.n_drop++; return; }
        {
            mon_org_t o; memcpy(&o, b, sizeof(o));
            g_mon.rung_step = o.step; g_mon.rung_h = o.h;
            g_mon.rung_hs = o.hs; g_mon.n_org++;
            g_mon.rung[g_mon.rung_n].step = o.step;
            g_mon.rung[g_mon.rung_n].h16 = (uint16_t)o.h;
            g_mon.rung_n = (uint8_t)((g_mon.rung_n + 1) % 8);
        }
        break;
    default: break;                        /* other types: not ours */
    }
}
