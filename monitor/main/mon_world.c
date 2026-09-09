#include <string.h>
#include "mon_world.h"
static wchunk_t s_wch[6];
uint32_t mw_h(uint32_t v)
{
    v ^= v >> 16; v *= 0x7feb352du;
    v ^= v >> 15; v *= 0x846ca68bu;
    v ^= v >> 16;
    return v ? v : 1;
}
const wchunk_t *mw_chunk(int32_t id)
{
    int slot = (int)(((uint32_t)id) % 6u);
    wchunk_t *c = &s_wch[slot];
    if (c->id == id && c->np) return c;
    uint32_t r = mw_h((uint32_t)id * 2654435761u + 0xB16B00B5u);
    c->id = id;
    int32_t base = id * 64;
    c->np = (uint8_t)(3 + (r % 3));
    for (int k = 0; k < c->np; k++) {
        r = mw_h(r);
        c->p[k].w = (uint8_t)(14 + (r % 20));
        c->p[k].x = base + (int32_t)(mw_h(r + 7) %
                                     (uint32_t)(64 - c->p[k].w));
        c->p[k].y = (uint8_t)(12 + (mw_h(r + 13) % 34));
    }
    uint32_t gr = mw_h((uint32_t)id * 977u + 5u);
    if ((gr & 3) == 0) {
        c->gap_w = (uint8_t)(10 + (gr >> 4) % 5);
        c->gap_x = base + 8 + (int32_t)((gr >> 8) %
                                        (uint32_t)(48 - c->gap_w));
        c->bridged = ((gr >> 2) % 5) < 3;
    } else {
        c->gap_w = 0;
        c->bridged = false;
    }
    uint32_t hr = mw_h((uint32_t)id * 31337u);
    c->house_x = ((hr & 3) == 1)
        ? base + 4 + (int32_t)((hr >> 4) % 44) : -1;
    if (c->house_x >= 0 && c->gap_w &&
        c->house_x + 14 > c->gap_x &&
        c->house_x < c->gap_x + c->gap_w + 2) {
        c->house_x = -1;
    }
    c->nl = 0;
    c->ns = 0;
    for (int k = 0; k < c->np; k++) {
        uint8_t below = WK_GROUND;
        int m_best = -1;
        for (int m = 0; m < c->np; m++) {
            if (m == k) continue;
            if (c->p[m].y > c->p[k].y &&
                c->p[m].x < c->p[k].x + c->p[k].w &&
                c->p[m].x + c->p[m].w > c->p[k].x &&
                c->p[m].y < below) {
                below = c->p[m].y;
                m_best = m;
            }
        }
        int gap = (int)below - (int)c->p[k].y;
        if (gap > WK_REACH && c->nl < 3) {
            wlad_t *L = &c->l[c->nl];
            int32_t lo = c->p[k].x + 2, hi = c->p[k].x + c->p[k].w - 2;
            if (m_best >= 0) {
                int32_t lo2 = c->p[m_best].x + 1;
                int32_t hi2 = c->p[m_best].x + c->p[m_best].w - 1;
                if (lo2 > lo) lo = lo2;
                if (hi2 < hi) hi = hi2;
            }
            if (hi - lo < 3 && m_best >= 0) {
                below = WK_GROUND;
                lo = c->p[k].x + 2;
                hi = c->p[k].x + c->p[k].w - 2;
            }
            if (hi > lo) {
                L->x = lo + (int32_t)(mw_h((uint32_t)(id + k)) %
                                      (uint32_t)(hi - lo));
                L->ytop = c->p[k].y;
                L->ybot = below;
                c->nl++;
            }
        } else if (gap > 12 && c->ns < 2 &&
                   (mw_h((uint32_t)(id * 5 + k)) & 3) == 0) {
            wsli_t *S = &c->s[c->ns++];
            S->dir = (mw_h((uint32_t)(id + 99 + k)) & 1) ? 1 : -1;
            S->x = S->dir > 0 ? c->p[k].x + c->p[k].w - 1
                              : c->p[k].x;
            S->ytop = c->p[k].y;
            S->ybot = below;
        }
    }
    return c;
}
void mw_hsv(uint16_t h, uint8_t s, uint8_t v,
            uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint32_t c = (uint32_t)v * s / 255;
    uint32_t x = c * (60 - ((h % 120 >= 60) ? (h % 60) : (60 - h % 60)));
    x = c - x / 60;                        /* c*(1-|h/60 mod 2 -1|) */
    x = c - (c * ((h % 120 < 60) ? (60 - h % 60) : (h % 60))) / 60;
    uint32_t m = v - c;
    uint32_t rr = 0, gg = 0, bb2 = 0;
    if (h < 60)       { rr = c; gg = x; }
    else if (h < 120) { rr = x; gg = c; }
    else if (h < 180) { gg = c; bb2 = x; }
    else if (h < 240) { gg = x; bb2 = c; }
    else if (h < 300) { rr = x; bb2 = c; }
    else              { rr = c; bb2 = x; }
    *r = (uint8_t)(rr + m); *g = (uint8_t)(gg + m);
    *b = (uint8_t)(bb2 + m);
}
