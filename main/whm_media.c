/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 *
 * whm_media.c - P1: the .whm container player + the LIVE lane.
 *
 * CONTAINER (docs/MEDIA.md): 32B header { "WHMV", ver u8, flags u8,
 * w u16, h u16, fps_q8 u16, nframes u32, loop u32, rsv[14] }, then
 * per frame: { size u32, payload = RGB888 row-RLE }. RLE: each row is
 * runs of [len u8 (1..255), r, g, b] until the row's w pixels are
 * emitted; frames are independent (any frame is a seek point).
 *
 * CLOCK: frame index is pure f(TSF): fi = (tsf-start)*fps/1e6, the
 * same physics that locked the walker and the music. A fleet-wide
 * file (w > 64) is cropped per node at x = strip_idx*64: the movie
 * spans the bezel because the format never knew the bezel existed.
 *
 * LIVE: type-15 fragments reassemble into a 3-slot PSRAM ring keyed
 * by seq; each carries pts_ms. The first frame anchors pts->tsf
 * (due0 = arrival + LATENCY_US), after which every frame schedules
 * at due0 + (pts-pts0)*1000 - identical arithmetic on every node,
 * so live video hits both panels at the same TSF instant. Loss
 * drops a frame and holds the last: live semantics. */
#include "whm_media.h"
#include "display_hal.h"
#include "wifi_tsf.h"
#include "ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static const char *TAG = "whm_media";
#define LATENCY_US 150000LL

typedef struct __attribute__((packed)) {
    char magic[4]; uint8_t ver, flags; uint16_t w, h, fps_q8;
    uint32_t nframes, loop; uint8_t rsv[12];
} whmv_hdr_t;
_Static_assert(sizeof(whmv_hdr_t) == 32, "whmv header");

static FILE *s_f;
static whmv_hdr_t s_h;
static uint32_t *s_ofs;                  /* PSRAM frame offsets */
static int64_t s_start;
static int32_t s_last_fi = -1;
static bool s_loop;
static uint8_t *s_buf;                   /* PSRAM frame payload */
static uint32_t s_buf_cap;

/* ---- LIVE ring ---- */
#define LV_SLOTS 3
#define LV_CAP   16384
static struct {
    uint16_t seq, w, h;
    uint32_t pts, len, got;
    uint8_t frag_n, mask;
    int64_t due;
    uint8_t *buf;
} s_lv[LV_SLOTS];
static bool s_live;
static int64_t s_lv_due0; static uint32_t s_lv_pts0; static bool s_lv_anch;
static uint16_t s_lv_shown_seq = 0xffff;

static void media_close(void)
{
    if (s_f) { fclose(s_f); s_f = NULL; }
    if (s_ofs) { free(s_ofs); s_ofs = NULL; }
    s_last_fi = -1;
}

static void rle_blit(const uint8_t *p, uint32_t len, uint16_t w,
                     uint16_t h)
{
    int xoff = (int)whm_ui_strip_idx() * 64;
    uint32_t i = 0;
    for (int y = 0; y < h && y < 64; y++) {
        int x = 0;
        while (x < w) {
            if (i + 4 > len) return;
            uint8_t run = p[i], r = p[i+1], g = p[i+2], b = p[i+3];
            i += 4;
            if (!run) return;
            for (int k = 0; k < run && x < w; k++, x++) {
                int sx = x - xoff;
                if (sx >= 0 && sx < 64)
                    whm_display_set_pixel(sx, y, r, g, b);
            }
        }
    }
}

esp_err_t whm_media_play(const char *path, int64_t start_tsf,
                         int loop_ovr)
{
    media_close(); s_live = false;
    char full[96];
    snprintf(full, sizeof(full), "/sdcard/media/%s", path);
    s_f = fopen(full, "rb");
    if (!s_f) { printf("media: no %s\n", full); return ESP_FAIL; }
    if (fread(&s_h, 1, 32, s_f) != 32 ||
        memcmp(s_h.magic, "WHMV", 4)) {
        printf("media: bad header\n"); media_close();
        return ESP_FAIL;
    }
    s_ofs = heap_caps_malloc(s_h.nframes * 4, MALLOC_CAP_SPIRAM);
    if (!s_ofs) { media_close(); return ESP_ERR_NO_MEM; }
    uint32_t off = 32;
    for (uint32_t i = 0; i < s_h.nframes; i++) {
        uint32_t sz;
        fseek(s_f, off, SEEK_SET);
        if (fread(&sz, 1, 4, s_f) != 4) { s_h.nframes = i; break; }
        s_ofs[i] = off + 4;
        off += 4 + sz;
        if (sz > s_buf_cap) s_buf_cap = sz;
    }
    if (!s_buf || 1) {
        free(s_buf);
        s_buf = heap_caps_malloc(s_buf_cap + 16, MALLOC_CAP_SPIRAM);
    }
    s_loop = loop_ovr >= 0 ? loop_ovr : (s_h.loop != 0);
    s_start = start_tsf;
    printf("media: '%s' %ux%u @%.1ffps, %lu frames%s, downbeat in "
           "%lldms\n", path, s_h.w, s_h.h, s_h.fps_q8 / 256.0,
           (unsigned long)s_h.nframes, s_loop ? " loop" : "",
           (long long)((start_tsf - whm_wifi_tsf_now()) / 1000));
    return ESP_OK;
}

void whm_media_stop(void) { media_close(); s_live = false; }
bool whm_media_active(void) { return s_f != NULL || s_live; }

void whm_media_live_begin(void)
{
    media_close();
    for (int i = 0; i < LV_SLOTS; i++) {
        if (!s_lv[i].buf)
            s_lv[i].buf = heap_caps_malloc(LV_CAP, MALLOC_CAP_SPIRAM);
        s_lv[i].seq = 0xffff;
    }
    s_lv_anch = false; s_lv_shown_seq = 0xffff;
    s_live = true;
    ESP_LOGI(TAG, "LIVE lane open (pts->tsf anchors on first frame)");
}

static void lv_map(uint32_t pts, int64_t rx_tsf)
{
    if (!s_lv_anch) {
        s_lv_anch = true; s_lv_pts0 = pts;
        s_lv_due0 = rx_tsf + LATENCY_US;
    }
}

void whm_media_live_rx(uint16_t seq, uint8_t fi, uint8_t fn,
                       uint16_t w, uint16_t h, uint32_t pts,
                       const uint8_t *pl, uint16_t plen,
                       int64_t rx_tsf)
{
    if (!s_live) whm_media_live_begin();
    lv_map(pts, rx_tsf);
    int sl = seq % LV_SLOTS;
    if (s_lv[sl].seq != seq) {
        s_lv[sl].seq = seq; s_lv[sl].w = w; s_lv[sl].h = h;
        s_lv[sl].pts = pts; s_lv[sl].len = 0; s_lv[sl].got = 0;
        s_lv[sl].frag_n = fn; s_lv[sl].mask = 0;
        s_lv[sl].due = s_lv_due0 +
            (int64_t)(pts - s_lv_pts0) * 1000LL;
    }
    uint32_t at = (uint32_t)fi * 1100u;
    if (at + plen <= LV_CAP && !(s_lv[sl].mask & (1u << fi))) {
        memcpy(s_lv[sl].buf + at, pl, plen);
        s_lv[sl].mask |= 1u << fi;
        s_lv[sl].got++;
        if (at + plen > s_lv[sl].len) s_lv[sl].len = at + plen;
    }
}

void whm_media_live_local(uint16_t w, uint16_t h, uint32_t pts,
                          const uint8_t *rle, uint32_t len)
{
    if (!s_live) whm_media_live_begin();
    int64_t now = whm_wifi_tsf_now();
    lv_map(pts, now);
    int sl = 0; static uint16_t lseq = 0x8000;
    sl = (++lseq) % LV_SLOTS;
    s_lv[sl].seq = lseq; s_lv[sl].w = w; s_lv[sl].h = h;
    s_lv[sl].pts = pts;
    s_lv[sl].len = len > LV_CAP ? LV_CAP : len;
    memcpy(s_lv[sl].buf, rle, s_lv[sl].len);
    s_lv[sl].frag_n = 1; s_lv[sl].mask = 1; s_lv[sl].got = 1;
    s_lv[sl].due = s_lv_due0 + (int64_t)(pts - s_lv_pts0) * 1000LL;
}

void whm_media_tick(int64_t tf)
{
    int64_t tsf = whm_wifi_tsf_now();
    if (s_live) {
        int best = -1; int64_t bdue = 0;
        for (int i = 0; i < LV_SLOTS; i++) {
            if (s_lv[i].seq == 0xffff) continue;
            if (s_lv[i].got < s_lv[i].frag_n) continue;
            if (s_lv[i].due > tsf) continue;
            if (s_lv[i].seq == s_lv_shown_seq) continue;
            if (best < 0 || s_lv[i].due > bdue) {
                best = i; bdue = s_lv[i].due;
            }
        }
        if (best >= 0) {
            whm_display_clear();
            rle_blit(s_lv[best].buf, s_lv[best].len,
                     s_lv[best].w, s_lv[best].h);
            s_lv_shown_seq = s_lv[best].seq;
        }
        return;
    }
    if (!s_f || tsf < s_start) return;
    int64_t fi64 = (tsf - s_start) * (int64_t)s_h.fps_q8 /
                   (256LL * 1000000LL);
    if (s_h.nframes == 0) return;
    if (fi64 >= (int64_t)s_h.nframes) {
        if (!s_loop) { media_close(); return; }
        fi64 %= s_h.nframes;
    }
    int32_t fi = (int32_t)fi64;
    if (fi == s_last_fi) return;
    s_last_fi = fi;
    fseek(s_f, s_ofs[fi] - 4, SEEK_SET);
    uint32_t sz;
    if (fread(&sz, 1, 4, s_f) != 4 || sz > s_buf_cap) return;
    if (fread(s_buf, 1, sz, s_f) != sz) return;
    whm_display_clear();
    rle_blit(s_buf, sz, s_h.w, s_h.h);
}

int whm_media_list(void)
{
    DIR *d = opendir("/sdcard/media");
    if (!d) return 0;
    struct dirent *e; int n = 0;
    while ((e = readdir(d))) {
        const char *x = strrchr(e->d_name, '.');
        if (x && strcasecmp(x, ".whm") == 0) {
            printf("  [%d] %s\n", n++, e->d_name);
        }
    }
    closedir(d);
    if (!n) printf("  (no .whm files - see tools/whmconv.py)\n");
    return n;
}
