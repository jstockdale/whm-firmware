/* The background stack, VERBATIM from main/ui.c (dumped 2026-09-09).
 * DEVIATIONS REGISTER (each deliberate, monitor-only):
 *  - single pass: idx=0, W -> WCOLS(128); parallax bases keep the
 *    k*cam form (64*idx term vanishes at idx 0 - same world math)
 *  - strip culls widened 64->128: stars >=W -> >=WCOLS; moon
 *    W+12 -> WCOLS+12; birds/pigeon 70 -> WCOLS+6; flora large
 *    84 -> WCOLS+20, medium 78 -> WCOLS+14, accents 72 -> WCOLS+8
 *  - sw (synth energy) pinned 0.0f: no audio here; branches kept
 *  - wall clock: SNTP + NVS tz_min (console 'tz'); no-clock
 *    fallback matches fleet (daylight 1.0f)
 *  - omitted, noted: wk_comet, wk_wonders (landmark pack),
 *    NYE plaza veto, fw background sparkle (console-state driven) */
#include <math.h>
#include <time.h>
#include <string.h>
#include "nvs.h"
#include "mon_world.h"
#include "mon_draw.h"
#define WCOLS 128
#define W WCOLS
static int32_t s_tz_min = -420;            /* console 'tz' overrides */
void mon_sky_init(void)
{
    nvs_handle_t h;
    if (nvs_open("mon", NVS_READONLY, &h) == ESP_OK) {
        int32_t v;
        if (nvs_get_i32(h, "tz_min", &v) == ESP_OK) s_tz_min = v;
        nvs_close(h);
    }
}
static bool mon_wall(struct tm *lt)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;    /* SNTP not yet synced */
    now += (time_t)s_tz_min * 60;
    gmtime_r(&now, lt);
    return true;
}
#include "mon_parse.h"
#include "esp_timer.h"
float mw_daylight(void)
{
    if (g_mon.day_us &&
        esp_timer_get_time() - g_mon.day_us < 5000000)
        return (float)g_mon.day_fq8 / 255.0f;   /* wire truth */

    struct tm lt;
    if (!mon_wall(&lt)) return 1.0f;
    float hm = (g_mon.day_us && esp_timer_get_time() -
              g_mon.day_us < 5000000)
             ? (float)g_mon.day_min / 60.0f
             : (float)lt.tm_hour + (float)lt.tm_min / 60.0f;
    if (hm < 6.0f || hm >= 20.0f) return 0.0f;
    if (hm < 8.0f)  return (hm - 6.0f) / 2.0f;
    if (hm < 17.0f) return 1.0f;
    return 1.0f - (hm - 17.0f) / 3.0f;
}
void mw_sky(int64_t t, float cam, float f)
{
    const float sw = 0.0f;
    uint8_t sr = (uint8_t)(0 + f * 16.0f);
    uint8_t sg = (uint8_t)(0 + f * 42.0f);
    uint8_t sb = (uint8_t)(2 + f * 100.0f);
    if (sw > 0.0f) {
        for (int y = 0; y < WK_GROUND; y++) {
            float g2 = (float)y / (float)WK_GROUND;
            uint8_t rr = (uint8_t)((1 - sw) * sr +
                                   sw * (26.0f + g2 * 120.0f));
            uint8_t gg = (uint8_t)((1 - sw) * sg + sw * (4.0f +
                                   g2 * 18.0f));
            uint8_t bb = (uint8_t)((1 - sw) * sb +
                                   sw * (48.0f + g2 * 60.0f));
            for (int x = 0; x < W; x++) mw_px(x, y, rr, gg, bb);
        }
    } else {
        mw_fill(sr, sg, sb);
    }
    float glow = f * (1.0f - f) * 4.0f;
    if (glow > 1.0f) glow = 1.0f;
    if (glow > 0.05f && sw < 0.5f) {
        for (int y = 46; y < WK_GROUND; y++) {
            float g2 = glow * (float)(y - 45) / 12.0f;
            uint8_t rr = (uint8_t)(sr + g2 * 90.0f);
            uint8_t gg = (uint8_t)(sg + g2 * 24.0f);
            for (int x = 0; x < W; x++) mw_px(x, y, rr, gg, sb / 2);
        }
    }
    float base_slow = cam * 0.04f;
    float base_star = cam * 0.10f;
    float base_cld  = cam * 0.15f;
    if (f < 0.3f) {
        float dim = 1.0f - f / 0.3f;
        for (int i = 0; i < 18; i++) {
            uint32_t h2 = mw_h(0x51A2u + (uint32_t)i * 747u);
            float wx = (float)(h2 % 512u);
            int sx = ((int)(wx - base_star) % 512 + 512) % 512;
            if (sx >= WCOLS) continue;
            int sy = 2 + (int)((h2 >> 12) % 26u);
            float tw = 0.7f + 0.3f * sinf((float)t / 3.0e5f +
                                          (float)(h2 & 63));
            uint8_t v = (uint8_t)(dim * tw * 150.0f);
            if (sw > 0.3f) {
                if (h2 & 1) mw_px(sx, sy, (uint8_t)(v / 3),
                                  (uint8_t)(v * 0.9f), v);
                else mw_px(sx, sy, v, (uint8_t)(v / 4),
                           (uint8_t)(v * 0.8f));
            } else {
                mw_px(sx, sy, v, v, (uint8_t)(v / 2 + 40 * dim));
            }
        }
        int mxi = (((int)(300.0f - base_slow)) % 900 + 900) % 900
                  - 100;
        if (mxi > -12 && mxi < WCOLS + 12) {
            for (int dy2 = -9; dy2 <= 9; dy2++)
                for (int dx2 = -9; dx2 <= 9; dx2++) {
                    if (dx2 * dx2 + dy2 * dy2 > 81) continue;
                    if ((dx2 - 6) * (dx2 - 6) + dy2 * dy2 <= 55)
                        continue;
                    uint8_t mr = (uint8_t)(120 * dim);
                    uint8_t mg = (uint8_t)(120 * dim);
                    uint8_t mb = (uint8_t)(88 * dim);
                    if (sw > 0.3f) { mr = (uint8_t)(160 * dim);
                        mg = (uint8_t)(60 * dim);
                        mb = (uint8_t)(150 * dim); }
                    mw_px(mxi + dx2, 12 + dy2, mr, mg, mb);
                }
            for (int ai = 0; ai < 28; ai++) {
                float ga = 6.2832f * (float)ai / 28.0f;
                int gx = (int)(cosf(ga) * 9.0f);
                int gy = (int)(sinf(ga) * 9.0f);
                if ((gx - 6) * (gx - 6) + gy * gy <= 55) continue;
                float run = 0.5f + 0.5f *
                            cosf(ga - (float)t / 1.6e6f);
                float lum = dim * (0.8f + 0.5f * run);
                if (lum > 1.0f) lum = 1.0f;
                mw_px(mxi + gx, 12 + gy, (uint8_t)(150 * lum),
                      (uint8_t)(150 * lum), (uint8_t)(120 * lum));
            }
        }
    }
    if (f > 0.02f || sw > 0.3f) {
        struct tm lt2;
        if (mon_wall(&lt2)) {
            float hm = (float)lt2.tm_hour + (float)lt2.tm_min / 60.0f;
            if (hm >= 6.0f && hm < 20.0f) {
                float p = (hm - 6.0f) / 14.0f;
                float sunw = 30.0f + p * 80.0f;
                int sxi = (((int)(sunw - (cam * 0.006f)))
                           % 900 + 900) % 900 - 100;
                float dusk = hm > 17.0f ? (hm - 17.0f) / 3.0f : 0.0f;
                float dawn = hm < 8.0f ? (8.0f - hm) / 2.0f : 0.0f;
                float lowr = dusk > dawn ? dusk : dawn;
                int sy = 12 + (int)(sinf((p - 0.5f) * 3.1416f) *
                                    sinf((p - 0.5f) * 3.1416f) *
                                    6.0f) + (int)(lowr * 30.0f);
                int rr2 = 11 + (int)(dusk * 4.0f);
                uint8_t sunr = (uint8_t)(220 - dusk * 30.0f);
                uint8_t sung = (uint8_t)(185 - dusk * 125.0f);
                uint8_t sunb = (uint8_t)(70 - dusk * 62.0f);
                for (int dy2 = -rr2; dy2 <= rr2; dy2++)
                    for (int dx2 = -rr2; dx2 <= rr2; dx2++) {
                        int q2 = dx2 * dx2 + dy2 * dy2;
                        if (q2 > rr2 * rr2 + 1) continue;
                        int py2 = sy + dy2;
                        if (py2 >= WK_GROUND) continue;
                        if (sw > 0.3f && ((py2 & 3) == 3) &&
                            dy2 > -2) continue;
                        if (q2 > (rr2 - 1) * (rr2 - 1)) {
                            float ang = atan2f((float)dy2,
                                               (float)dx2);
                            float g1 = 0.5f + 0.5f *
                                cosf(ang - (float)t / 8.0e6f);
                            float g2 = 0.5f + 0.5f *
                                cosf(ang + (float)t / 8.0e6f);
                            float brth = 0.86f + 0.14f *
                                sinf((float)t / 9.0e6f);
                            float lum = brth * (0.52f +
                                0.42f * g1 + 0.42f * g2);
                            if (lum > 1.30f) lum = 1.30f;
                            mw_px(sxi + dx2, py2,
                                  (uint8_t)(sunr * lum > 255 ? 255
                                   : sunr * lum),
                                  (uint8_t)(sung * lum > 255 ? 255
                                   : sung * lum),
                                  (uint8_t)(sunb * lum));
                        } else {
                            mw_px(sxi + dx2, py2, sunr, sung, sunb);
                        }
                    }
                {   /* CROWN v4 - mirrors main/ui.c verbatim */
                    float gp = sinf((float)t / 4.5e6f);
                    float Lb = 2.6f + 3.4f * lowr;
                    for (int k9 = 0; k9 < 16; k9++) {
                        float lk = Lb * (0.72f + 0.38f *
                            (0.5f + 0.5f * sinf((float)t / 7.0e6f
                             + (float)k9 * 0.9f)));
                        int L9 = (int)lk;
                        if (L9 < 2) L9 = 2;
                        float c9 = (lk - 2.0f) /
                                   (Lb * 1.1f - 1.99f);
                        if (c9 < 0.0f) c9 = 0.0f;
                        if (c9 > 1.0f) c9 = 1.0f;
                        float pers = sinf((float)t / 4.5e6f +
                                          (float)k9 * 2.4f);
                        float sway = 0.10f *
                            (c9 * gp + (1.0f - c9) * pers);
                        float ang9 = (float)k9 * 0.3927f + sway;
                        float cs = cosf(ang9), sn = sinf(ang9);
                        float tw = 0.78f + 0.34f *
                            sinf((float)t / 2.2e6f +
                                 (float)k9 * 1.7f);
                        for (int e2 = 1; e2 <= L9; e2++) {
                            int rx2 = sxi + (int)(cs *
                                       (float)(rr2 + e2));
                            int ry2 = sy + (int)(sn *
                                       (float)(rr2 + e2));
                            float m9 = tw * sunr > 255.0f
                                       ? 255.0f : tw * sunr;
                            float n9 = tw * sung > 255.0f
                                       ? 255.0f : tw * sung;
                            if (ry2 < WK_GROUND && ry2 >= 0)
                                mw_px(rx2, ry2, (uint8_t)m9,
                                      (uint8_t)n9,
                                      (uint8_t)(tw * sunb > 40 ?
                                       40 : tw * sunb));
                        }
                    }
                }
            }
        }
    }
    /* wk_comet: omitted on the monitor (noted) */
    if (f > 0.35f || sw > 0.3f) {
        float cf = f > 0.35f ? (f - 0.35f) / 0.65f : sw * 0.5f;
        for (int i = 0; i < 4; i++) {
            uint32_t h2 = mw_h(0xC10Du + (uint32_t)i * 991u);
            float wvar = -0.25f - 0.1f * (float)(h2 & 3) / 3.0f;
            float wx = (float)(h2 % 640u) +
                       (float)((double)t / 1e6) * wvar;
            int bob = (int)(sinf((float)t / 4.0e6f +
                                 (float)(h2 & 15)) * 1.2f);
            int sx = (((int)(wx - base_cld)) % 640 + 640) % 640 - 60;
            int sy = 4 + (int)((h2 >> 10) % 12u) + bob;
            uint8_t cv = (uint8_t)(cf * 120.0f);
            uint8_t cr = cv + 45, cg = cv + 47, cb = cv + 55;
            if (sw > 0.3f) { cr = cv / 2 + 40; cg = cv / 4 + 8;
                             cb = cv / 2 + 46; }
            int kind = (int)((h2 >> 20) % 3u);
            int elen = kind == 0 ? 10 : (kind == 1 ? 14 : 22);
            int epos = (int)((t / 220000 + (int64_t)i * 7) %
                             (int64_t)elen);
            int bpos = elen - 1 -
                       (int)((t / 300000 + (int64_t)i * 5) %
                             (int64_t)elen);
            #define CLD_BOT(px2, py2, ex)                             \
                do { int d2 = (ex) - bpos;                            \
                     if (d2 < 0) d2 = -d2;                            \
                     if (d2 <= 2)                                     \
                         mw_px((px2), (py2),                          \
                               (uint8_t)(cr + 26 - 8 * d2),           \
                               (uint8_t)(cg + 25 - 8 * d2),           \
                               (uint8_t)(cb + 22 - 7 * d2));          \
                } while (0)
            #define CLD_EDGE(px2, py2, ex)                            \
                do { int d2 = (ex) - epos;                            \
                     if (d2 < 0) d2 = -d2;                            \
                     if (d2 <= 1)                                     \
                         mw_px((px2), (py2),                          \
                               (uint8_t)(cr + 60 - 20 * d2),          \
                               (uint8_t)(cg + 58 - 20 * d2),          \
                               (uint8_t)(cb + 50 - 18 * d2));         \
                } while (0)
            if (kind == 0) {
                for (int dx2 = 0; dx2 < 10; dx2++) {
                    mw_px(sx + dx2, sy, cr, cg, cb);
                    CLD_EDGE(sx + dx2, sy - 1, dx2);
                }
            } else if (kind == 1) {
                for (int dx2 = 0; dx2 < 16; dx2++)
                    mw_px(sx + dx2, sy + 1, cr, cg, cb);
                for (int dx2 = 1; dx2 < 7; dx2++)
                    mw_px(sx + dx2, sy, cr + 20, cg + 20, cb + 20);
                for (int dx2 = 9; dx2 < 15; dx2++)
                    mw_px(sx + dx2, sy, cr + 20, cg + 20, cb + 20);
                for (int dx2 = 1; dx2 < 15; dx2++)
                    CLD_EDGE(sx + dx2, dx2 < 7 || dx2 > 8 ? sy - 1
                                                          : sy,
                             dx2 - 1);
                for (int dx2 = 2; dx2 < 14; dx2++) {
                    mw_px(sx + dx2, sy + 2, cr - 22, cg - 22,
                          cb - 18);
                    CLD_BOT(sx + dx2, sy + 2, dx2 - 2);
                }
            } else {
                for (int dx2 = 0; dx2 < 24; dx2++)
                    mw_px(sx + dx2, sy + 2, cr, cg, cb);
                for (int dx2 = 2; dx2 < 9; dx2++)
                    mw_px(sx + dx2, sy + 1, cr + 18, cg + 18,
                          cb + 18);
                for (int dx2 = 8; dx2 < 17; dx2++) {
                    mw_px(sx + dx2, sy, cr + 26, cg + 26, cb + 26);
                    mw_px(sx + dx2, sy + 1, cr + 18, cg + 18,
                          cb + 18);
                }
                for (int dx2 = 16; dx2 < 22; dx2++)
                    mw_px(sx + dx2, sy + 1, cr + 18, cg + 18,
                          cb + 18);
                for (int dx2 = 1; dx2 < 23; dx2++) {
                    int ey = dx2 >= 8 && dx2 <= 16 ? sy - 1 :
                             (dx2 >= 2 && dx2 <= 21 ? sy : sy + 1);
                    CLD_EDGE(sx + dx2, ey, dx2 - 1);
                }
                for (int dx2 = 1; dx2 < 23; dx2++) {
                    mw_px(sx + dx2, sy + 3, cr - 26, cg - 26,
                          cb - 20);
                    CLD_BOT(sx + dx2, sy + 3, dx2 - 1);
                }
            }
            #undef CLD_BOT
            #undef CLD_EDGE
        }
    }
}
void mw_birds(int64_t t, float cam, float f)
{
    const float sw = 0.0f;
    bool day = f > 0.5f && sw < 0.3f;
    bool syn = sw > 0.3f;
    if (!day && !syn) return;
    float base = cam * 0.10f;
    double ts9 = (double)t / 1e6;
    if (day) {
        static const struct { float off; int dy; } F9[4] = {
            { 0.0f, 0 }, { 9.0f, 1 }, { 18.0f, 0 }, { 330.0f, 1 }
        };
        float head = 610.0f - (float)fmod(ts9 * 4.5, 640.0);
        for (int i = 0; i < 4; i++) {
            float wx = head + F9[i].off;
            int sx = (((int)(wx - base)) % 640 + 640) % 640 - 20;
            if (sx < -6 || sx > WCOLS + 6) continue;
            int sy = 9 + ((i * 5) % 11) + F9[i].dy;
            int flap = (int)(t / 380000 + i * 3) & 1;
            uint8_t v = 205, g2 = 205, b2 = 210;
            if (flap) {
                mw_px(sx, sy - 1, v, g2, b2);
                mw_px(sx + 1, sy, v, g2, b2);
                mw_px(sx + 2, sy + 1, 160, 160, 170);
                mw_px(sx + 3, sy, v, g2, b2);
                mw_px(sx + 4, sy - 1, v, g2, b2);
            } else {
                mw_px(sx, sy, v, g2, b2);
                mw_px(sx + 1, sy, v, g2, b2);
                mw_px(sx + 2, sy + 1, 160, 160, 170);
                mw_px(sx + 3, sy, v, g2, b2);
                mw_px(sx + 4, sy, v, g2, b2);
            }
        }
    } else {
        float wx = 300.0f - (float)fmod(ts9 * 2.2, 640.0);
        int sx = (((int)(wx - base)) % 640 + 640) % 640 - 20;
        if (sx >= -6 && sx <= WCOLS + 6) {
            int sy = 12;
            int flap = (int)(t / 620000) & 1;
            uint8_t pr = 200, pg = 60, pb = 170;
            mw_px(sx + 1, sy + 1, 30, 8, 44);
            mw_px(sx + 2, sy + 1, 30, 8, 44);
            mw_px(sx + 3, sy + 1, 30, 8, 44);
            mw_px(sx + 4, sy, 40, 12, 56);
            mw_px(sx + 1, sy + 2, pr, pg, pb);
            mw_px(sx + 2, sy + 2, pr, pg, pb);
            if (flap) {
                mw_px(sx + 1, sy - 1, 60, 200, 220);
                mw_px(sx + 3, sy - 1, 60, 200, 220);
            } else {
                mw_px(sx, sy + 1, 60, 200, 220);
            }
        }
    }
}
static void mw_tree_oak(int sx, int gy, float li, float sw,
                        uint32_t h)
{
    uint8_t tr = (uint8_t)(96 * li), tg = (uint8_t)(62 * li),
            tb = (uint8_t)(28 * li);
    uint8_t l0r = (uint8_t)(22 * li), l0g = (uint8_t)(92 * li),
            l0b = (uint8_t)(26 * li);
    uint8_t l1r = (uint8_t)(34 * li), l1g = (uint8_t)(126 * li),
            l1b = (uint8_t)(38 * li);
    uint8_t l2r = (uint8_t)(58 * li), l2g = (uint8_t)(160 * li),
            l2b = (uint8_t)(60 * li);
    if (sw > 0.3f) { l0r = (uint8_t)(14*li); l0g = (uint8_t)(8*li);
        l0b = (uint8_t)(30*li); l1r = (uint8_t)(24*li);
        l1g = (uint8_t)(12*li); l1b = (uint8_t)(44*li);
        l2r = (uint8_t)(40*li); l2g = (uint8_t)(120*li);
        l2b = (uint8_t)(140*li); }
    int th = 10 + (int)(h % 4u);
    for (int y2 = 0; y2 < th; y2++) {
        mw_px(sx, gy - y2, tr, tg, tb);
        mw_px(sx + 1, gy - y2, tr, tg, tb);
        if (y2 < 3) mw_px(sx + 2, gy - y2,
                          (uint8_t)(tr * 0.8f),
                          (uint8_t)(tg * 0.8f),
                          (uint8_t)(tb * 0.8f));
    }
    int cw = 15 + (int)((h >> 4) % 3u);
    int ch = 15 + (int)((h >> 6) % 5u);
    int cy0 = gy - th - ch + 3;
    for (int y2 = 0; y2 < ch; y2++) {
        float yr = (float)y2 / (float)(ch - 1);
        float half = (float)cw * 0.5f *
                     sinf(yr * 3.1416f);
        int hw = (int)(half + 0.5f);
        uint32_t jr = mw_h(h ^ (uint32_t)y2 * 131u);
        hw += (int)(jr % 3u) - 1;
        if (hw < 1) hw = 1;
        for (int dx2 = -hw; dx2 <= hw; dx2++) {
            int py2 = cy0 + y2;
            uint8_t rr = l1r, gg = l1g, bb = l1b;
            if (y2 < ch / 3 && dx2 < 0) { rr = l2r; gg = l2g;
                bb = l2b; }
            else if (y2 > (2 * ch) / 3) { rr = l0r; gg = l0g;
                bb = l0b; }
            if (((jr >> (dx2 & 15)) & 7u) == 0) { rr = l0r;
                gg = l0g; bb = l0b; }
            mw_px(sx + 1 + dx2, py2, rr, gg, bb);
        }
    }
}
static void mw_tree_pine(int sx, int gy, float li, float sw,
                         uint32_t h)
{
    uint8_t tr = (uint8_t)(88 * li), tg = (uint8_t)(58 * li),
            tb = (uint8_t)(26 * li);
    uint8_t d0r = (uint8_t)(12 * li), d0g = (uint8_t)(70 * li),
            d0b = (uint8_t)(34 * li);
    uint8_t d1r = (uint8_t)(20 * li), d1g = (uint8_t)(104 * li),
            d1b = (uint8_t)(48 * li);
    if (sw > 0.3f) { d0r = (uint8_t)(10*li); d0g = (uint8_t)(6*li);
        d0b = (uint8_t)(26*li); d1r = (uint8_t)(20*li);
        d1g = (uint8_t)(10*li); d1b = (uint8_t)(40*li); }
    int tiers = 4 + (int)(h % 2u);
    int tierh = 6 + (int)((h >> 3) % 2u);
    int total = tiers * tierh - (tiers - 1) * 2;
    for (int y2 = 0; y2 < 3; y2++) {
        mw_px(sx, gy - y2, tr, tg, tb);
        mw_px(sx + 1, gy - y2, tr, tg, tb);
    }
    int base = gy - 3;
    for (int ti = 0; ti < tiers; ti++) {
        int ty0 = base - ti * (tierh - 2);
        int maxw = 3 + (tiers - ti) * 2;
        for (int y2 = 0; y2 < tierh; y2++) {
            int hw = maxw - (y2 * maxw) / tierh;
            if (hw < 1) hw = 1;
            for (int dx2 = -hw; dx2 <= hw; dx2++)
                mw_px(sx + dx2, ty0 - y2,
                      (ti + y2) & 1 ? d1r : d0r,
                      (ti + y2) & 1 ? d1g : d0g,
                      (ti + y2) & 1 ? d1b : d0b);
        }
    }
    (void)total;
}
void mw_flora(float cam, float f, int64_t t)
{
    (void)t;
    const float sw = 0.0f;
    float li = 0.35f + 0.65f * f;
    float base = 0.7f * cam;
    int32_t c0 = (int32_t)floorf(base / 64.0f);
    /* NYE plaza veto omitted (not simulated on the monitor) */
    for (int ci = c0 - 1; ci <= c0 + 2; ci++) {
        uint32_t h = mw_h((uint32_t)ci * 2246822519u ^ 0xF10FAu);
        uint32_t roll = h % 100u;
        int sxbase = (int)lroundf((float)(ci * 64) - base);
        int gy = WK_GROUND - 1;
        if (roll < 30u) {
            int wx = 10 + (int)((h >> 8) % 40u);
            int sx = sxbase + wx;
            if (sx > -20 && sx < WCOLS + 20) {
                if ((h >> 16) & 1)
                    mw_tree_oak(sx, gy, li, sw, h);
                else
                    mw_tree_pine(sx, gy, li, sw, h);
            }
        } else if (roll < 55u) {
            int wx = 8 + (int)((h >> 8) % 46u);
            int sx = sxbase + wx;
            if (sx > -14 && sx < WCOLS + 14) {
                uint32_t h2 = mw_h(h ^ 0xBEEFu);
                if ((h >> 16) & 1) {
                    for (int y2 = 0; y2 < 6; y2++)
                        mw_px(sx, gy - y2, (uint8_t)(90 * li),
                              (uint8_t)(60 * li),
                              (uint8_t)(28 * li));
                    for (int y2 = 0; y2 < 10; y2++) {
                        float yr = (float)y2 / 9.0f;
                        int hw = (int)(4.5f * sinf(yr * 3.1416f)
                                       + 0.5f);
                        if (hw < 1) hw = 1;
                        for (int dx2 = -hw; dx2 <= hw; dx2++)
                            mw_px(sx + dx2, gy - 5 - y2,
                                  (uint8_t)((30 + ((h2 >> (dx2 &
                                   7)) & 1) * 26) * li),
                                  (uint8_t)((116 + ((h2 >> (dx2 &
                                   7)) & 1) * 30) * li),
                                  (uint8_t)(36 * li));
                    }
                } else {
                    mw_px(sx, gy, (uint8_t)(88 * li),
                          (uint8_t)(58 * li), (uint8_t)(26 * li));
                    for (int ti = 0; ti < 3; ti++)
                        for (int y2 = 0; y2 < 6; y2++) {
                            int hw = (3 + (3 - ti) * 2) * (6 - y2)
                                     / 6;
                            if (hw < 1) hw = 1;
                            for (int dx2 = -hw; dx2 <= hw; dx2++)
                                mw_px(sx + dx2,
                                      gy - 1 - ti * 4 - y2,
                                      (uint8_t)(16 * li),
                                      (uint8_t)((84 + (ti & 1) *
                                                 24) * li),
                                      (uint8_t)(40 * li));
                        }
                }
            }
        } else if (roll < 78u) {
            int npc = 1 + (int)((h >> 8) & 1u);
            for (int k = 0; k < npc; k++) {
                uint32_t hk = mw_h(h ^ (uint32_t)(k + 1) * 40503u);
                int sx = sxbase + 6 + (int)(hk % 52u);
                if (sx < -8 || sx > WCOLS + 8) continue;
                int kind = (int)((hk >> 16) % 4u);
                float dl = li * 0.55f;
                if (kind == 0) {
                    for (int y2 = 0; y2 < 5; y2++) {
                        int hw = (5 - y2) / 2;
                        for (int dx2 = -hw; dx2 <= hw; dx2++)
                            mw_px(sx + dx2, gy - y2,
                                  (uint8_t)(18 * dl),
                                  (uint8_t)(88 * dl),
                                  (uint8_t)(36 * dl));
                    }
                } else if (kind == 1) {
                    for (int dx2 = -2; dx2 <= 2; dx2++)
                        mw_px(sx + dx2, gy, (uint8_t)(28 * li),
                              (uint8_t)(112 * li),
                              (uint8_t)(34 * li));
                    for (int dx2 = -1; dx2 <= 1; dx2++)
                        mw_px(sx + dx2, gy - 1,
                              (uint8_t)(30 * li),
                              (uint8_t)(130 * li),
                              (uint8_t)(36 * li));
                } else if (kind == 2) {
                    mw_px(sx, gy, (uint8_t)(92 * li),
                          (uint8_t)(92 * li), (uint8_t)(98 * li));
                    mw_px(sx + 1, gy, (uint8_t)(74 * li),
                          (uint8_t)(74 * li), (uint8_t)(80 * li));
                    mw_px(sx, gy - 1, (uint8_t)(110 * li),
                          (uint8_t)(110 * li),
                          (uint8_t)(116 * li));
                } else {
                    for (int fx2 = 0; fx2 < 3; fx2++) {
                        uint32_t fh = mw_h(hk ^ (uint32_t)fx2 *
                                           77u);
                        uint8_t fr = (uint8_t)(180 * li),
                                fg2 = (uint8_t)(60 * li),
                                fb = (uint8_t)(120 * li);
                        if (fh & 1) { fr = (uint8_t)(220 * li);
                            fg2 = (uint8_t)(190 * li);
                            fb = (uint8_t)(40 * li); }
                        mw_px(sx + (int)(fh % 5u) - 2, gy, fr,
                              fg2, fb);
                    }
                }
            }
        }
    }
}
