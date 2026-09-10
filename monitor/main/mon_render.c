/* The x3 world twin + telemetry column. Terrain paint and sprite
 * reproduced VERBATIM from main/ui.c (wk_px -> mw_px, s_wk -> the
 * wire pose); hues on the microsecond clock like the panels. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_timer.h"
#include "mon_pins.h"
#include "mon_parse.h"
#include "mon_world.h"
#include "mon_draw.h"
#include "mon_sky.h"
#include "gfx5x7.h"
#include "display_hal.h"
#define WCOLS 128
/* viewport-parameterized plotter: every layer - sky, flora,
   birds, terrain, sprite - draws through whichever transform
   the page sets. FULL and WORLD share one compose pipeline. */
static int mw_x0 = 0, mw_y0 = 24, mw_sc = 3,
           mw_rowcrop = 0, mw_rows = 64;
void mw_viewport(int x0, int y0, int sc, int rowcrop, int rows)
{
    mw_x0 = x0; mw_y0 = y0; mw_sc = sc;
    mw_rowcrop = rowcrop; mw_rows = rows;
}
void mw_fill(uint8_t r, uint8_t g, uint8_t b)
{
    whm_display_fill_rect(mw_x0, mw_y0, WCOLS * mw_sc,
                          mw_rows * mw_sc, r, g, b);
}
void mw_px(int sx, int sy, uint8_t r, uint8_t g, uint8_t b)
{
    int y2 = sy - mw_rowcrop;
    if (sx < 0 || sx >= WCOLS || y2 < 0 || y2 >= mw_rows) return;
    whm_display_fill_rect(mw_x0 + sx * mw_sc, mw_y0 + y2 * mw_sc,
                          mw_sc, mw_sc, r, g, b);
}
static void sprite(int lx, int y, int64_t t)
{
    bool a = (g_mon.phase >> 2) & 1;
    uint8_t br = 213, bg = 194, bb = 167;
    uint8_t sr, sg, sb;
    mw_hsv((uint16_t)((t / 90000) % 360), 230, 255, &sr, &sg, &sb);
    int st = g_mon.st, dir = g_mon.dir, sdir = g_mon.sdir;
    int timer = g_mon.timer, phase = g_mon.phase;
    int squat = (st == WK_CROUCH || st == WK_LAND) ? 2
              : (st == WK_SIT || st == WK_SETUP ||
                 st == WK_PACK) ? 3 : 0;
    for (int dy2 = -11 + squat; dy2 <= -9 + squat; dy2++)
        for (int dx2 = -1; dx2 <= 1; dx2++)
            mw_px(lx + dx2, y + dy2, br, bg, bb);
    int scarf = (st == WK_JUMP) ? 2 : 0;
    mw_px(lx, y - 8 + squat, sr, sg, sb);
    mw_px(lx - dir, y - 8 + squat, sr, sg, sb);
    mw_px(lx - (2 + scarf) * dir, y - 8 + squat + (a ? 1 : 0),
          sr, sg, sb);
    if (scarf) mw_px(lx - 4 * dir, y - 7, sr, sg, sb);
    for (int dy2 = -7 + squat; dy2 <= -3; dy2++)
        mw_px(lx, y + dy2, br, bg, bb);
    if (st == WK_CROUCH) {
        mw_px(lx - 1, y - 2, br, bg, bb);
        mw_px(lx + 1, y - 2, br, bg, bb);
        mw_px(lx - 1, y - 1, br, bg, bb);
        mw_px(lx + 1, y - 1, br, bg, bb);
        int ph = phase & 7;
        uint8_t pr, pg, pb;
        mw_hsv((uint16_t)((t / 20000) % 360), 255, 255,
               &pr, &pg, &pb);
        mw_px(lx, y - 12 - (ph & 3), pr, pg, pb);
        mw_px(lx + ((ph & 1) ? 1 : -1), y - 13 - (ph >> 2),
              pr, pg, pb);
    } else if (st == WK_JUMP) {
        mw_px(lx - dir, y - 2, br, bg, bb);
        mw_px(lx, y - 1, br, bg, bb);
        mw_px(lx - 1, y - 9, br, bg, bb);
        mw_px(lx + 1, y - 9, br, bg, bb);
    } else if (st == WK_LAND) {
        mw_px(lx - 2, y - 1, br, bg, bb);
        mw_px(lx + 2, y - 1, br, bg, bb);
        mw_px(lx - 1, y - 1, br, bg, bb);
        mw_px(lx + 1, y - 1, br, bg, bb);
    } else if (st == WK_SIT || st == WK_SETUP || st == WK_PACK) {
        mw_px(lx - 1, y - 4, br, bg, bb);
        mw_px(lx + dir, y - 2, br, bg, bb);
        mw_px(lx + 2 * dir, y - 1, br, bg, bb);
        if (phase) {
            int ux = lx - 4 * dir;
            int hh = 10;
            if (st == WK_SETUP) hh = 10 - timer;
            if (st == WK_PACK)  hh = timer + 2;
            if (hh < 0) hh = 0;
            for (int dy3 = 1; dy3 <= hh; dy3++)
                mw_px(ux, y - dy3, 240, 240, 240);
            if (hh >= 8) {
                int spn = hh >= 10 ? 3 : hh - 7;
                for (int i = -spn; i <= spn; i++) {
                    uint8_t cr9, cg9, cb9;
                    mw_hsv((uint16_t)(((t / 90000) +
                            (i + 3) * 51) % 360), 235, 255,
                            &cr9, &cg9, &cb9);
                    mw_px(ux + i, y - hh - 1, cr9, cg9, cb9);
                    if (i > -spn && i < spn)
                        mw_px(ux + i, y - hh - 2, cr9, cg9, cb9);
                }
            }
        }
    } else if (st == WK_BASE) {
        int ph = phase;
        if (ph <= 1) {
            mw_px(lx - dir, y - 2, br, bg, bb);
            mw_px(lx, y - 1, br, bg, bb);
        } else if (ph <= 3) {
            mw_px(lx, y - 2, br, bg, bb);
            mw_px(lx, y - 1, br, bg, bb);
            mw_px(lx - 1, y - 10, br, bg, bb);
            mw_px(lx + 1, y - 10, br, bg, bb);
        }
        if (ph >= 1 && ph <= 3) {
            int wspan = ph == 1 ? 3 + (6 - timer) : 9;
            if (wspan > 9) wspan = 9;
            int half = wspan / 2;
            for (int i = -half; i <= half; i++) {
                uint8_t cr9, cg9, cb9;
                mw_hsv((uint16_t)(((t / 90000) +
                        (i + half) * 36) % 360), 235, 255,
                        &cr9, &cg9, &cb9);
                mw_px(lx + i, y - 16, cr9, cg9, cb9);
                if (i > -half && i < half)
                    mw_px(lx + i, y - 15, cr9, cg9, cb9);
            }
            mw_px(lx - half, y - 13, 255, 255, 255);
            mw_px(lx + half, y - 13, 255, 255, 255);
            mw_px(lx - 1, y - 12, 255, 255, 255);
            mw_px(lx + 1, y - 12, 255, 255, 255);
        }
        if (ph >= 4) {
            int gx = lx - dir * 3;
            int wob = (ph == 5) ? ((timer & 3) < 2 ? 1 : -1) : 0;
            int span = (ph == 6) ? 1 + timer / 4 : 4;
            for (int i = -span; i <= span; i++) {
                uint8_t cr9, cg9, cb9;
                mw_hsv((uint16_t)(((t / 90000) +
                        (i + span) * 36) % 360), 235, 255,
                        &cr9, &cg9, &cb9);
                mw_px(gx + i + wob, y - 1 - ((i & 1) ? 1 : 0),
                      cr9, cg9, cb9);
            }
        }
    } else if (st == WK_POP) {
        uint8_t pr, pg, pb;
        mw_hsv((uint16_t)((t / 15000) % 360), 255, 255,
               &pr, &pg, &pb);
        mw_px(lx - 3, y - 6, pr, pg, pb);
        mw_px(lx + 3, y - 8, pr, pg, pb);
        mw_px(lx - 2, y - 11, pr, pg, pb);
        mw_px(lx + 2, y - 3, pr, pg, pb);
        mw_px(lx - 1, y - 1, br, bg, bb);
        mw_px(lx + 1, y - 1, br, bg, bb);
    } else if (st == WK_LADDER || st == WK_SHIMMY) {
        mw_px(lx - 1, y + (a ? -8 : -6), br, bg, bb);
        mw_px(lx + 1, y + (a ? -6 : -8), br, bg, bb);
        mw_px(lx - 1, y - (a ? 1 : 2), br, bg, bb);
        mw_px(lx + 1, y - (a ? 2 : 1), br, bg, bb);
    } else if (st == WK_SLIDE) {
        mw_px(lx - 1, y - 9, br, bg, bb);
        mw_px(lx + 1, y - 9, br, bg, bb);
        mw_px(lx - sdir * 3, y - 5, 48, 66, 89);
        mw_px(lx - sdir * 5, y - 4, 26, 36, 58);
        mw_px(lx - 1, y - 1, br, bg, bb);
        mw_px(lx + 1, y - 1, br, bg, bb);
    } else if (st == WK_FALL) {
        mw_px(lx - 1, y - 8, br, bg, bb);
        mw_px(lx + 1, y - 8, br, bg, bb);
        mw_px(lx - 1, y - 1, br, bg, bb);
        mw_px(lx + 1, y - 1, br, bg, bb);
    } else if (st == WK_IDLE) {
        mw_px(lx - 1, y - 5, br, bg, bb);
        mw_px(lx + 1, y - 5, br, bg, bb);
        mw_px(lx - 1, y - 1, br, bg, bb);
        mw_px(lx + 1, y - 1, br, bg, bb);
    } else {
        int8_t d = (int8_t)dir;
        if (a) {
            mw_px(lx + d, y - 2, br, bg, bb);
            mw_px(lx + 2 * d, y - 1, br, bg, bb);
            mw_px(lx - d, y - 2, br, bg, bb);
            mw_px(lx - 2 * d, y - 1, br, bg, bb);
            mw_px(lx - d, y - 6, br, bg, bb);
            mw_px(lx + d, y - 5, br, bg, bb);
        } else {
            mw_px(lx, y - 2, br, bg, bb);
            mw_px(lx, y - 1, br, bg, bb);
            mw_px(lx + d, y - 2, br, bg, bb);
            mw_px(lx + d, y - 6, br, bg, bb);
            mw_px(lx - d, y - 5, br, bg, bb);
        }
    }
}
static float mon_cam_est(int64_t now)
{
    /* cam: type-10 snaps + slope EMA between them - ONE
       estimator now serves both pages (the FULL duplicate
       kept its own statics and could drift). */
    static float cam = 0, camv = 30.0f, lastc = -1;
    static int64_t camt = 0; static uint32_t lastn = 0;
    if (g_mon.n_wkp != lastn) {
        if (camt) {
            float v = (g_mon.cam - lastc) /
                      ((float)(now - camt) / 1e6f);
            if (v > 0 && v < 200) camv = camv * 0.5f + v * 0.5f;
        }
        lastc = g_mon.cam; camt = now; lastn = g_mon.n_wkp;
    }
    if (camt) return lastc + camv * (float)(now - camt) / 1e6f;
    return g_mon.x - 64.0f;
}
static void world_compose(int64_t t, float cam, int32_t ox)
{
    mon_state_t M;
    memcpy(&M, (const void *)&g_mon, sizeof(M));
#define g_mon M
    float f = mw_daylight();
    mw_sky(t, cam, f);                 /* the true sky */
    mw_flora(cam, f, t);               /* behind platforms */
    mw_birds(t, cam, f);
    /* ground - carved by gaps, planked by bridges (verbatim) */
    int32_t id0 = (int32_t)floorf((float)ox / 64.0f);
    for (int x = 0; x < WCOLS; x++) {
        int32_t wx = ox + x;
        bool gap = false, bridge = false;
        for (int d = 0; d <= 2; d++) {
            const wchunk_t *c = mw_chunk(id0 + d);
            if (c->gap_w && wx >= c->gap_x &&
                wx < c->gap_x + c->gap_w) {
                gap = true;
                bridge = c->bridged;
            }
        }
        if (!gap) {
            mw_px(x, 58, 11, 16, 11);
            mw_px(x, 59, 5, 8, 5);
            mw_px(x, 60, 2, 3, 3);
        } else if (bridge) {
            mw_px(x, 58, 79, 42, 11);
            mw_px(x, 59, 40, 22, 6);
            if ((wx & 3) == 0) mw_px(x, 57, 96, 52, 14);
        } else {
            mw_px(x, 60, 1, 1, 2);
            mw_px(x, 61, 1, 1, 2);
        }
    }
    for (int d = 0; d <= 2; d++) {
        const wchunk_t *c = mw_chunk(id0 + d);
        if (c->house_x >= 0) {
            int hx = (int)(c->house_x - ox);
            for (int yy = 46; yy <= 57; yy++)
                for (int xx = 0; xx < 14; xx++)
                    mw_px(hx + xx, yy, 34, 22, 12);
            for (int rr = 0; rr < 5; rr++)
                for (int xx = rr; xx < 14 - rr; xx++)
                    mw_px(hx + xx, 45 - rr + 4, 70, 16, 8);
            for (int yy = 52; yy <= 57; yy++) {
                mw_px(hx + 10, yy, 8, 5, 3);
                mw_px(hx + 11, yy, 8, 5, 3);
            }
            float wl = 1.0f - f;
            uint8_t wr = (uint8_t)(6 + wl * 150.0f);
            uint8_t wg = (uint8_t)(6 + wl * 95.0f);
            for (int yy = 49; yy <= 51; yy++)
                for (int xx = 3; xx <= 5; xx++)
                    mw_px(hx + xx, yy, wr, wg, 8);
        }
        for (int k = 0; k < c->np; k++) {
            uint8_t py = c->p[k].y;
            for (int32_t px = c->p[k].x;
                 px < c->p[k].x + c->p[k].w; px++) {
                int lx = (int)(px - ox);
                int32_t rel = px - c->p[k].x;
                bool rim = rel == 0 || rel == c->p[k].w - 1;
                if (py <= 14) {
                    mw_px(lx, py, rim ? 120 : 185, rim ? 122 : 187,
                          rim ? 132 : 198);
                    mw_px(lx, py + 1, 95, 98, 112);
                    if (!rim && (px & 3) == 1)
                        mw_px(lx, py - 1, 150, 152, 165);
                } else if (py <= 19) {
                    mw_px(lx, py, 34, 150, 44);
                    mw_px(lx, py + 1, 24, 92, 34);
                    mw_px(lx, py + 2, 8, 44, 14);
                    if (rel == c->p[k].w / 2) {
                        mw_px(lx, py + 3, 52, 28, 8);
                        mw_px(lx, py + 4, 52, 28, 8);
                    }
                } else {
                    mw_px(lx, py, 29, 133, 36);
                    mw_px(lx, py + 1, 26, 30, 48);
                    mw_px(lx, py + 2, 10, 12, 22);
                    mw_px(lx, py + 3, 5, 6, 12);
                }
            }
        }
        for (int k = 0; k < c->nl; k++) {
            int lx = (int)(c->l[k].x - ox);
            for (int y = c->l[k].ytop; y <= c->l[k].ybot; y++) {
                mw_px(lx - 1, y, 79, 42, 11);
                mw_px(lx + 1, y, 79, 42, 11);
                if (((y - c->l[k].ytop) & 3) == 1)
                    mw_px(lx, y, 143, 89, 30);
            }
        }
        for (int k = 0; k < c->ns; k++) {
            int steps = c->s[k].ybot - c->s[k].ytop;
            for (int q = 0; q <= steps; q++) {
                int lx = (int)(c->s[k].x + c->s[k].dir * q - ox);
                mw_px(lx, c->s[k].ytop + q, 98, 126, 158);
                mw_px(lx + c->s[k].dir, c->s[k].ytop + q,
                      48, 66, 89);
            }
        }
    }
    /* bezel seams between fleet strips */
    int strips = g_mon.strips ? g_mon.strips : 2;
    for (int s2 = 1; s2 < strips && s2 < 2; s2++)
        for (int sy = 0; sy < 64; sy++)
            mw_px(s2 * 64, sy, 20, 26, 36);
    /* the little red chair + the sprite (verbatim) */
    int wlx = (int)lroundf(g_mon.x) - ox;
    int wy = (int)lroundf(g_mon.y);
    if (g_mon.st == WK_SETUP || g_mon.st == WK_SIT ||
        g_mon.st == WK_PACK) {
        int chx = wlx - g_mon.dir;
        mw_px(chx - 1, wy - 3, 90, 20, 12);
        mw_px(chx, wy - 3, 90, 20, 12);
        mw_px(chx + 1, wy - 3, 90, 20, 12);
        mw_px(chx - 1, wy - 5, 90, 20, 12);
        mw_px(chx - 1, wy - 4, 90, 20, 12);
        mw_px(chx - 1, wy - 1, 60, 13, 8);
        mw_px(chx + 1, wy - 1, 60, 13, 8);
    }
    if (wlx >= -6 && wlx <= WCOLS + 6) sprite(wlx, wy, t);
#undef g_mon
}
void mon_render(uint32_t kfs)
{
    mw_viewport(0, 24, 3, 0, 64);
    int64_t now = esp_timer_get_time();  /* LOCAL: cam slope dt */
    int64_t t = mon_now();               /* SHARED: all cosmetics */
    mon_state_t M;                 /* atomic-enough pose snapshot:
        one copy per frame, so the rx task can't tear fields
        mid-compose across cores */
    memcpy(&M, (const void *)&g_mon, sizeof(M));
#define g_mon M
    float cam = mon_cam_est(now);
    int32_t ox = (int32_t)lroundf(cam);
    /* top + bottom bands */
    char ln[56];
    snprintf(ln, sizeof ln, "step %lu  %s  kf/s %lu",
             (unsigned long)g_mon.step, mon_st_name(g_mon.st),
             (unsigned long)kfs);
    gfx_text(4, 4, ln, 2, 124, 196, 255);
    {   /* AGE: ms since the last ACCEPTED keyframe - the
           tree-splitter. Sawtooth to 300-800 ms = acceptance
           gaps (raw8 shows why); steady ~33 ms with a frozen
           walker = the freeze lives downstream of g_mon. */
        int64_t age9 = g_mon.last_kf_us
            ? (esp_timer_get_time() - g_mon.last_kf_us) / 1000
            : -1;
        snprintf(ln, sizeof ln, "x %.0f  cam %.0f  age %lld  %s",
                 g_mon.x, g_mon.cam, (long long)age9, g_mon.from);
    }
    gfx_text(4, LCD_H - 20, ln, 2, 140, 150, 168);
    world_compose(t, cam, ox);
    /* telemetry column */
    int cx0 = mw_x0 + WCOLS * mw_sc + 6;
    uint8_t oc_r = g_mon.owner ? 255 : 65,
            oc_g = g_mon.owner ? 92 : 208,
            oc_b = g_mon.owner ? 200 : 255;
    whm_display_fill_rect(cx0 - 4, 0, 2, LCD_H, oc_r, oc_g, oc_b);
    gfx_text(cx0, 8, (char *)mon_st_name(g_mon.st), 3,
             oc_r, oc_g, oc_b);
    snprintf(ln, sizeof ln, "strip %u", g_mon.owner);
    gfx_text(cx0, 36, ln, 2, 200, 208, 220);
    snprintf(ln, sizeof ln, "y %.1f", g_mon.y);
    gfx_text(cx0, 58, ln, 1, 160, 170, 185);
#if MON_COL_COMPACT
    snprintf(ln, sizeof ln, "vx %+.2f", g_mon.vx);
    gfx_text(cx0, 70, ln, 1, 160, 170, 185);
    snprintf(ln, sizeof ln, "vy %+.2f", g_mon.vy);
    gfx_text(cx0, 80, ln, 1, 160, 170, 185);
#else
    snprintf(ln, sizeof ln, "vx %+.2f vy %+.2f",
             g_mon.vx, g_mon.vy);
    gfx_text(cx0, 70, ln, 1, 160, 170, 185);
#endif
    snprintf(ln, sizeof ln, "rung @%lu",
             (unsigned long)g_mon.rung_step);
    gfx_text(cx0, 84, ln, 1, 200, 180, 69);
    gfx_text(cx0, 102, "recent", 1, 110, 120, 138);
    for (int i = 0; i < 5; i++) {
        int idx = (g_mon.tr_n + 4 - i) % 5;
        if (!g_mon.tr[idx].step) continue;
        snprintf(ln, sizeof ln, "@%lu %s",
                 (unsigned long)g_mon.tr[idx].step,
                 mon_st_name(g_mon.tr[idx].st));
        uint8_t r2 = g_mon.tr[idx].owner ? 255 : 65;
        uint8_t g2 = g_mon.tr[idx].owner ? 92 : 208;
        uint8_t b2 = g_mon.tr[idx].owner ? 200 : 255;
        gfx_text(cx0, 114 + i * 12, ln, 1, r2, g2, b2);
    }
    snprintf(ln, sizeof ln, "drop %lu",
             (unsigned long)g_mon.n_drop);
    gfx_text(cx0, 182, ln, 1, 110, 120, 138);
#undef g_mon
}

void mon_render_full(void)
{
    /* FULLSCREEN world = the SAME compose pipeline as WORLD -
       sky, flora, birds, houses, chair, the real sprite -
       through a bigger viewport. The duplicated terrain and
       the minimal stick-sprite are gone. */
    int64_t now = esp_timer_get_time();
    int64_t t = mon_now();
    float cam = mon_cam_est(now);
    int32_t ox = (int32_t)lroundf(cam);
#if LCD_W >= 536
    mw_viewport(12, 0, 4, 2, 60);      /* x4, rows 2..61 */
#else
    mw_viewport((LCD_W - 128 * 3) / 2,
                (LCD_H - 64 * 3) / 2, 3, 0, 64);
#endif
    world_compose(t, cam, ox);
    mw_viewport(0, 24, 3, 0, 64);      /* restore hybrid */
}
