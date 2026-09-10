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
            /* TWO-LANE INGEST (owner's kf/s 170): shadows carry
               NO marker - one sender serves real and promise,
               distinguished only by step (+4..+6 ahead under the
               Near Horizon). Classify by cadence: REAL chains at
               +1 (a +2 skip tolerated); +2..+8 ahead = a PROMISE,
               counted, never posed; forty straight ahead-frames =
               resync (wifi gap or we joined late). Epoch turns
               reset. Both units' real frames dedupe first-wins. */
            static uint32_t last_real; static int miss9;
            if (last_real &&
                (k.step > last_real + 1000 ||
                 last_real > k.step + 1000)) {
                last_real = 0; miss9 = 0;      /* epoch turn */
            }
            if (!last_real) { last_real = k.step; }
            else if (k.step <= last_real) { return; }
            else if (k.step - last_real <= 2) {
                last_real = k.step; miss9 = 0;
            } else if (k.step - last_real <= 8) {
                g_mon.n_promise++; return;     /* the shadow lane */
            } else {
                if (++miss9 < 40) return;
                last_real = k.step; miss9 = 0; /* resync */
            }
            g_mon.n_real++;
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
