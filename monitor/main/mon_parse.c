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
            static uint32_t last_step;
            if (k.step <= last_step && last_step - k.step < 1000)
                return;
            last_step = k.step;
            {   /* step-keyed pose ring: bank this frame; the
                   served pose is the banked step maxstep-5. */
                int sl9 = (int)(k.step & 15u);
                g_mon.ring16[sl9] = k;
                g_mon.ring16_step[sl9] = k.step;
                uint32_t want = k.step >= 5 ? k.step - 5 : k.step;
                int ws = (int)(want & 15u);
                if (g_mon.ring16_step[ws] == want)
                    k = g_mon.ring16[ws];   /* serve aligned */
            }
            g_mon.step = k.step; g_mon.x = k.x;
            g_mon.y = (float)k.yq1 * 0.5f;
            g_mon.vx = k.vx; g_mon.vy = k.vy; g_mon.spd = k.spd;
            g_mon.tgt = k.tgt; g_mon.st = k.st; g_mon.owner = k.owner;
            g_mon.dir = k.dir; g_mon.sdir = k.sdir;
            g_mon.phase = k.phase; g_mon.timer = k.timer;
            memcpy(g_mon.from, k.from, 16); g_mon.from[15] = 0;
            {   /* PASSIVE TSF SLAVE (owner: birds lag the
                   panels): fleet cosmetics run on the SHARED
                   clock; the monitor ran on boot-relative
                   uptime, so every pure-f(t) animation - birds,
                   cloud drift, scarf rainbow, sun breathing -
                   was phase-shifted by the boot delta. Each kf
                   carries the sender's tsf: EMA the offset and
                   the monitor joins the fleet's timeline without
                   transmitting a byte. */
                int64_t off = k.tsf - esp_timer_get_time();
                if (!g_mon.tsf_ok) { g_mon.tsf_off = off;
                    g_mon.tsf_ok = 1; }
                else g_mon.tsf_off += (off - g_mon.tsf_off) / 8;
            }
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
