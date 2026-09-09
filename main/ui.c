/*
 * ui.c - the display owner: info screens + test patterns + BOOT button.
 *
 * SCREENS (auto-rotating by default; `screen <name>` holds one, `screen auto`
 * resumes; BOOT button short-press = next screen and hold, long-press = auto):
 *   clock   RTC time large, date, uptime
 *   level   bubble level driven live by the IMU (crosshair + rings) -
 *           the seed of the gravity demos on the roadmap
 *   env     SHTC3 temperature large + humidity bar
 *   net     mode/ssid, IP, RSSI bars, TSF drift ppm
 *   sys     version, heaps, uptime
 *   sd      card presence, size, free space
 *   vu      live dual-mic level meters with peak hold (ES7210)
 *   life    Conway's Game of Life with age coloring (the screen Waveshare
 *           never designed but should have)
 *
 * Vendor parity note: Waveshare's shipped Test firmware cycles six info
 * pages (QMI, RTC, SD, SHTC3, WiFi, Audio). All six exist here in improved
 * form; sys and life are new.
 *
 * PATTERNS (bench validation, unchanged semantics from v0.2.0):
 *   corners red green blue white gradient gray checker sweep cycle
 */
#include "ui.h"
#include "esp_console.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#ifndef WHM_VERSION_STR
#define WHM_VERSION_STR "dev"
#endif
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"

#include "whm_board.h"
#include "display_hal.h"
#include "gfx5x7.h"
#include "sensors_test.h"
#include "wifi_tsf.h"
#include "storage_test.h"
#include "audio_test.h"
#include "mp3_player.h"
#include "sync.h"
#include "whlink.h"
#include "esp_rom_sys.h"
#include "settings.h"
#include "timesync.h"

static const char *TAG = "whm_ui";

static uint16_t W, H;

/* ---- fleet/system status: silent when healthy, a 2px top bar when not.
 * yellow = working on it, red = broken, green = brief success pulse. ---- */
typedef enum { UST_OK = 0, UST_YELLOW, UST_RED } ui_status_t;
static int64_t s_ok_pulse_until = 0;

void whm_ui_flash_ok(void) { s_ok_pulse_until = esp_timer_get_time() + 1300000; }

/* time_trusted: some discipline source has set the wall clock */
static ui_status_t status_level(bool *time_trusted)
{
    bool sta = whm_wifi_is_sta();
    bool con = whm_wifi_connected();
    whm_timesync_state_t ts = whm_timesync_state();
    bool flockwall = whm_sync_wall_from_flock();
    if (time_trusted) {
        *time_trusted = (ts == WHM_TS_SYNCED) || flockwall;
    }
    static int64_t s_discon_since = 0;
    int64_t now = esp_timer_get_time();
    if (sta && !con) {
        if (!s_discon_since) s_discon_since = now;
    } else {
        s_discon_since = 0;
    }
    if (sta && !con && now - s_discon_since > 20000000LL) return UST_RED;
    if (whm_sync_role() == WHM_SYNC_MEMBER) {
        int64_t age = whm_sync_conductor_age_us();
        if (age > 0 && age > 30000000LL) return UST_RED;
    }
    if (whm_storage_maintenance()) return UST_YELLOW;
    if (sta && !con) return UST_YELLOW;
    if (sta && con && ts == WHM_TS_WAITING) return UST_YELLOW;
    return UST_OK;
}

static void status_edges_and_bar(void)
{
    /* success pulses on rising edges: STA got IP; SNTP sync landed */
    static bool prev_con = false;
    static whm_timesync_state_t prev_ts = WHM_TS_DISABLED;
    bool con = whm_wifi_connected();
    whm_timesync_state_t ts = whm_timesync_state();
    if (con && !prev_con) whm_ui_flash_ok();
    if (ts == WHM_TS_SYNCED && prev_ts != WHM_TS_SYNCED) whm_ui_flash_ok();
    prev_con = con;
    prev_ts = ts;
    /* flock link events pulse too: member hears its conductor / takes the
       wall; conductor gains its first station */
    static bool prev_heard = false, prev_fw = false;
    static int prev_stn = 0;
    bool fw = whm_sync_wall_from_flock();
    int64_t cage = whm_sync_conductor_age_us();
    bool heard = (cage >= 0 && cage < 10000000LL);
    if (heard && !prev_heard) whm_ui_flash_ok();
    if (fw && !prev_fw) whm_ui_flash_ok();
    int stn = whm_wifi_ap_stations();
    if (whm_sync_role() == WHM_SYNC_CONDUCTOR && stn > 0 && prev_stn <= 0) {
        whm_ui_flash_ok();
    }
    prev_heard = heard;
    prev_fw = fw;
    prev_stn = stn;

    int64_t now = esp_timer_get_time();
    if (now < s_ok_pulse_until) {
        whm_display_fill_rect(0, 0, W, 2, 60, 220, 90);
        return;
    }
    ui_status_t st = status_level(NULL);
    if (st == UST_RED) whm_display_fill_rect(0, 0, W, 2, 235, 50, 40);
    else if (st == UST_YELLOW) whm_display_fill_rect(0, 0, W, 2, 235, 190, 40);
}

/* semantic text palette - one voice per role across every screen */
#define C_HINT 110, 110, 130
#define C_DIM  120, 120, 140
#define C_GOOD  90, 220,  90
#define C_BAD  255,  80,  80

/* -------- wanderer (P2b seam test) --------
 * Position is a PURE FUNCTION of the show clock (TSF when linked,
 * esp_timer solo) - no state, tail included - so panels rendering
 * adjacent windows of the virtual strip can only disagree by flip
 * quantization. That residue is exactly what the bench eyeballs. */
static void hsv_rgb(uint16_t h, uint8_t s, uint8_t v,
                    uint8_t *r, uint8_t *g, uint8_t *b);

static uint8_t s_w_idx = 0, s_w_n = 1;
static uint16_t s_w_speed = 40;
static bool s_w_loaded = false;

static void wander_load(void)
{
    if (s_w_loaded) return;
    uint8_t v8 = 0;
    uint32_t v32 = 0;
    if (whm_settings_get_u8("wand_idx", &v8) == ESP_OK) s_w_idx = v8;
    if (whm_settings_get_u8("wand_n", &v8) == ESP_OK && v8 >= 1 && v8 <= 8) {
        s_w_n = v8;
    }
    if (whm_settings_get_u32("wand_spd", &v32) == ESP_OK &&
        v32 >= 5 && v32 <= 240) {
        s_w_speed = (uint16_t)v32;
    }
    if (s_w_idx >= s_w_n) s_w_idx = 0;
    s_w_loaded = true;
}

void whm_ui_wander_get(uint8_t *idx, uint8_t *n, uint16_t *spd)
{
    wander_load();
    if (idx) *idx = s_w_idx;
    if (n) *n = s_w_n;
    if (spd) *spd = s_w_speed;
}

esp_err_t whm_ui_wander_set(uint8_t idx, uint8_t n, uint16_t spd)
{
    if (n < 1 || n > 8 || idx >= n || spd < 5 || spd > 240) {
        return ESP_ERR_INVALID_ARG;
    }
    s_w_idx = idx; s_w_n = n; s_w_speed = spd; s_w_loaded = true;
    whm_settings_set_u8("wand_idx", idx);
    whm_settings_set_u8("wand_n", n);
    whm_settings_set_u32("wand_spd", spd);
    return ESP_OK;
}

static void wander_pos(int64_t t_us, float *ox, float *oy)
{
    float vw = (float)s_w_n * 64.0f;
    double span = 2.0 * (double)(vw - 6.0f);
    double p = fmod(((double)t_us / 1e6) * (double)s_w_speed, span);
    if (p < 0) p += span;
    *ox = (p < (double)(vw - 6.0f)) ? (float)p + 3.0f
                                    : (float)(span - p) + 3.0f;
    float ts = (float)((double)t_us / 1e6);
    float yy = 32.0f + 22.0f * sinf(ts * 0.9f) + 6.0f * sinf(ts * 2.17f);
    if (yy < 4.0f) yy = 4.0f;
    if (yy > 59.0f) yy = 59.0f;
    *oy = yy;
}

static void pat_wander(int64_t t)
{
    wander_load();   /* solo fallback */
    whm_display_fill(2, 2, 6);
    for (int k = 8; k >= 0; k--) {          /* analytic tail, old->new */
        float fx, fy;
        wander_pos(t - (int64_t)k * 40000, &fx, &fy);
        float lx = fx - (float)s_w_idx * 64.0f;
        if (lx < -4.0f || lx > 68.0f) continue;
        uint16_t hue = (uint16_t)(((t / 80000) + (int64_t)k * 7) % 360);
        uint8_t r, g, b;
        hsv_rgb(hue, 255, k ? (uint8_t)(210 - k * 22) : 255, &r, &g, &b);
        int rad = k ? 1 : 2;
        int cx = (int)lroundf(lx), cy = (int)lroundf(fy);
        for (int dy = -rad; dy <= rad; dy++) {
            for (int dx = -rad; dx <= rad; dx++) {
                if (dx * dx + dy * dy > rad * rad + (k ? 0 : 1)) continue;
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < 64 && py >= 0 && py < 64) {
                    whm_display_set_pixel((uint16_t)px, (uint16_t)py,
                                          r, g, b);
                }
            }
        }
    }
    char tag[8];
    snprintf(tag, sizeof(tag), "%u/%u", (unsigned)(s_w_idx + 1),
             (unsigned)s_w_n);
    gfx_text(2, 57, tag, 1, 55, 55, 80);
}

static bool wall_now(struct tm *lt, suseconds_t *usec);
static bool s_fw_hold;                    /* fireworks: chair hold */
static int64_t s_fw_sparkle_until;        /* 'fw sparkle': bg-only test */
static int64_t s_fw_sparkle_last;
/* WALKER OWNERSHIP: exactly one unit simulates; everyone renders the
 * beacon pose. Transfer is edge-triggered by containment in the
 * MOVING strip windows (scroll itself hands him off); a 1.2s
 * containment-seizure covers a dead owner. Type-9, 10Hz. */
static struct {
    uint8_t owner;
    int64_t rx_us, last_tx, grace_until;
    uint8_t burst, resync;   /* resync: skip step-debt on gain */
    int64_t own_tsf;         /* claim rank: beacon TSF (doctrine 15
                                made every step "future" - step-rank
                                inverted into a flip-flop amplifier) */
    uint32_t ev_step;        /* evidence watermark (dedupe/order) */
    uint8_t storms;          /* corrections in window */
    int64_t storm_t0, mute_until;
} s_wko;
/* EQUAL-STEP REFEREE: compare poses at the SAME step number, not the
 * same wall moment - a beacon is 10-300ms old, and judging a live
 * replica against stale evidence poisoned it at every state change
 * (adopting the past = manufacturing the very lag we hunted). Ring
 * of recent own poses keyed by step; corrections apply as DELTAS. */
/* DOCTRINE 15: tell the future before it happens. The owner's shadow
 * sim emits KEYFRAMES ~2x sync-lead ahead; replicas verify AT-STEP
 * (zero evidence age) and on mismatch SNAP to the complete promised
 * state - initial-condition setting, never nudging. Trailing
 * telemetry and its 660ms judgment ring are retired. */
#define WKF_N 8
static struct { uint32_t step; float x; int8_t yq1;
                uint8_t st; int8_t dir; uint16_t timer;
                float tgt, vx;
                uint8_t valid; } s_wkf[WKF_N];
static uint32_t s_wkf_ok, s_wkf_snap, s_wkf_stale;
static struct { uint32_t step; uint8_t from, to; } s_wkh[8];
static uint8_t s_wkh_w;
static bool s_wk_shadowing;      /* shadow steps: no trace writes */
static bool s_wk_pure;               /* bisection: strip influences */
static bool wk_i_own(void)
{
    return s_w_n <= 1 || s_wko.owner == s_w_idx;
}
static bool wk_nye_window(int32_t chunk);
static float s_wk_scroll = 1.0f;          /* current (slewed) */
static float s_wk_tgt_scroll = 1.0f;      /* target: console + NYE */
static bool s_wk_alt_scarf;               /* friend wears teal */
static void wk_sprite(int lx, int y, int64_t t);

/* -------- the walker v3: a world with weather, ways, and whimsy -------
 * Everything from v2 stands (chunks = pure f(x), fleet-deterministic
 * stepping) plus: 1px/s camera, taller/denser terrain, ground GAPS
 * (bridged, or long-jumped - always traversable), houses whose windows
 * glow at night, a sky that mirrors the REAL wall clock (stars, a
 * crescent moon, drifting clouds, dawn/dusk glow), ladders whose feet
 * always rest on something, and a jump vocabulary: charged long-jump
 * with power sparkles, quick hop for vertical reach, squash landing. */
#define WK_TICK_US   33000LL
#define WK_ANCHOR_US 600000000LL
#define WK_CAM_SPD   1.0f
#define WK_GROUND    57
#define WK_REACH     21

enum { WK_WALK, WK_CLIMB, WK_LADDER, WK_SLIDE, WK_FALL, WK_IDLE,
       WK_CROUCH, WK_JUMP, WK_LAND,
       WK_GOCHAIR, WK_SETUP, WK_SIT, WK_PACK,   /* the camping ritual */
       WK_SHIMMY, WK_POP };                      /* bezel-wrap ritual */

typedef struct { int32_t x; uint8_t w, y; } wplat_t;
typedef struct { int32_t x; uint8_t ytop, ybot; } wlad_t;
typedef struct { int32_t x; uint8_t ytop, ybot; int8_t dir; } wsli_t;
typedef struct {
    int32_t id;
    wplat_t p[5]; uint8_t np;
    wlad_t  l[3]; uint8_t nl;
    wsli_t  s[2]; uint8_t ns;
    int32_t gap_x; uint8_t gap_w;        /* 0 = no gap */
    bool bridged;
    int32_t house_x;                     /* <0 = none */
} wchunk_t;
static wchunk_t s_wch[6];

static struct {
    float x, y, vy, vx;
    int8_t dir;
    uint8_t st, phase;
    uint16_t timer;
    float tgt;
    int8_t sdir;
    uint8_t turn_cd;         /* deliberate turns: no chattering */
    float spd;               /* strolling pace, chosen at turns */
    uint8_t fresh;           /* just claimed somewhere new */
} s_wk;
static uint32_t wk_h(uint32_t s);          /* defined just below */
static uint32_t s_wk_steps;
/* P1 input spine: control block + step-stamped event log. The log
 * is consumed AT STEP HEAD by wk_step itself, so live stepping and
 * replay are the same consumer - one mechanism (doctrine 15 made
 * flesh). USER mode expires after 300 steps (10 s) of silence and
 * the walker shakes off and wanders. */
static struct { uint8_t mode; uint32_t until; int8_t vx;
                uint8_t jump; uint8_t seek; float seekx; } s_wkc;
static struct { uint32_t step; uint8_t act; float arg;
                uint8_t valid; } s_wki[32];
static uint8_t s_wki_w;
static volatile bool s_wkp_poke;   /* new subscriber wants params */
static uint8_t s_wk_draws;   /* per-step draw counter (pure RNG) */
static int64_t s_wk_anchor = -1;

/* -------- the reward table: purposeful exploration ------------------
 * Platforms start at 1.0 and decay hard per visit (0.15, 0.05, 0.02 -
 * rapidly approaching zero); ladders and slides decay gently to a 0.5
 * floor (hey, they are still fun). Loafing is EXEMPT by design:
 * watching the world go by always pays best. Fleet-deterministic -
 * built purely from the shared step stream, reset at each anchor. */
#define WK_SEEN_N 48
static struct { int32_t key; uint8_t visits; } s_wk_seen[WK_SEEN_N];
static uint8_t s_wk_seen_wr;

static uint8_t wk_visits(int32_t key)
{
    for (int i = 0; i < WK_SEEN_N; i++) {
        if (s_wk_seen[i].key == key) return s_wk_seen[i].visits;
    }
    return 0;
}

static void wk_mark(int32_t key)
{
    for (int i = 0; i < WK_SEEN_N; i++) {
        if (s_wk_seen[i].key == key) {
            if (s_wk_seen[i].visits < 250) s_wk_seen[i].visits++;
            return;
        }
    }
    s_wk_seen[s_wk_seen_wr].key = key;
    s_wk_seen[s_wk_seen_wr].visits = 1;
    s_wk_seen_wr = (uint8_t)((s_wk_seen_wr + 1) % WK_SEEN_N);
}

static float wk_reward_plat(int32_t chunk, int k)
{
    static const float dec[4] = { 1.0f, 0.15f, 0.05f, 0.02f };
    uint8_t v = wk_visits(chunk * 8 + k);
    return dec[v > 3 ? 3 : v];
}

static float wk_reward_fun(int32_t key)   /* ladders + slides */
{
    static const float dec[3] = { 1.0f, 0.7f, 0.5f };
    uint8_t v = wk_visits(key);
    return dec[v > 2 ? 2 : v];
}

static const wchunk_t *wk_chunk(int32_t id);   /* defined below */

/* distance-weighted promise of fresh territory in one direction */
static float wk_scout(float x, int8_t dir)
{
    if (s_wk_pure) return 0.0f;      /* bisection: no reward pull */
    float score = 0.0f;
    int32_t id = (int32_t)floorf(x / 64.0f);
    for (int d = -2; d <= 2; d++) {
        const wchunk_t *c = wk_chunk(id + d);
        for (int k = 0; k < c->np; k++) {
            float px = (float)c->p[k].x + (float)c->p[k].w * 0.5f;
            float dx = px - x;
            if ((dir > 0) != (dx > 0)) continue;
            float ad = fabsf(dx);
            if (ad > 90.0f || ad < 3.0f) continue;
            score += wk_reward_plat(id + d, k) / (1.0f + ad / 24.0f);
        }
    }
    return score;
}

static uint32_t wk_rnd(void)
{
    /* PURE f(anchor, step, draw#): stateful xorshift made replicas
       depend on draw HISTORY - one skipped frame or corrector snap
       desynced the cursor until the next anchor. This form is
       identical on every unit by construction, forever. */
    return wk_h((uint32_t)(s_wk_anchor & 0xffffffff)
                ^ s_wk_steps * 2654435761u
                ^ (uint32_t)(++s_wk_draws) * 0x9E3779B9u);
}

static uint32_t wk_h(uint32_t v)
{
    v ^= v >> 16; v *= 0x7feb352du;
    v ^= v >> 15; v *= 0x846ca68bu;
    v ^= v >> 16;
    return v ? v : 1;
}

static const wchunk_t *wk_chunk(int32_t id)
{
    /* NYE plaza: this window is open ground - no platforms, no
       houses; poles and lights are drawn by wk_nye_scene */
    int slot = (int)(((uint32_t)id) % 6u);
    wchunk_t *c = &s_wch[slot];
    if (wk_nye_window(id)) {
        memset(c, 0, sizeof(*c));
        c->id = id;
        return c;                 /* open ground under the party */
    }
    if (c->id == id && c->np) return c;
    uint32_t r = wk_h((uint32_t)id * 2654435761u + 0xB16B00B5u);
    c->id = id;
    int32_t base = id * 64;
    c->np = (uint8_t)(3 + (r % 3));       /* 3-5: denser, taller */
    for (int k = 0; k < c->np; k++) {
        r = wk_h(r);
        c->p[k].w = (uint8_t)(14 + (r % 20));
        c->p[k].x = base + (int32_t)(wk_h(r + 7) %
                                     (uint32_t)(64 - c->p[k].w));
        c->p[k].y = (uint8_t)(12 + (wk_h(r + 13) % 34));
    }
    /* ground gap (jump's raison d'etre) - bridged 60% of the time */
    uint32_t gr = wk_h((uint32_t)id * 977u + 5u);
    if ((gr & 3) == 0) {
        c->gap_w = (uint8_t)(10 + (gr >> 4) % 5);
        c->gap_x = base + 8 + (int32_t)((gr >> 8) %
                                        (uint32_t)(48 - c->gap_w));
        c->bridged = ((gr >> 2) % 5) < 3;
    } else {
        c->gap_w = 0;
        c->bridged = false;
    }
    /* a house, sometimes (scenery; he strolls in front) */
    uint32_t hr = wk_h((uint32_t)id * 31337u);
    c->house_x = ((hr & 3) == 1)
        ? base + 4 + (int32_t)((hr >> 4) % 44) : -1;
    if (c->house_x >= 0 && c->gap_w &&
        c->house_x + 14 > c->gap_x &&
        c->house_x < c->gap_x + c->gap_w + 2) {
        c->house_x = -1;                  /* no houses over pits */
    }
    /* ladders + slides; ladder FEET must rest on something */
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
            if (m_best >= 0) {            /* intersect both spans */
                int32_t lo2 = c->p[m_best].x + 1;
                int32_t hi2 = c->p[m_best].x + c->p[m_best].w - 1;
                if (lo2 > lo) lo = lo2;
                if (hi2 < hi) hi = hi2;
            }
            if (hi - lo < 3 && m_best >= 0) {
                below = WK_GROUND;        /* fall back: rest on ground */
                lo = c->p[k].x + 2;
                hi = c->p[k].x + c->p[k].w - 2;
            }
            if (hi > lo) {
                L->x = lo + (int32_t)(wk_h((uint32_t)(id + k)) %
                                      (uint32_t)(hi - lo));
                L->ytop = c->p[k].y;
                L->ybot = below;
                c->nl++;
            }
        } else if (gap > 12 && c->ns < 2 &&
                   (wk_h((uint32_t)(id * 5 + k)) & 3) == 0) {
            wsli_t *S = &c->s[c->ns++]; /* wheee */
            S->dir = (wk_h((uint32_t)(id + 99 + k)) & 1) ? 1 : -1;
            S->x = S->dir > 0 ? c->p[k].x + c->p[k].w - 1 : c->p[k].x;
            S->ytop = c->p[k].y;
            S->ybot = below;
        }
    }
    return c;
}

static bool wk_in_open_gap(float x)
{
    int32_t id = (int32_t)floorf(x / 64.0f);
    for (int d = -1; d <= 1; d++) {
        const wchunk_t *c = wk_chunk(id + d);
        if (c->gap_w && !c->bridged &&
            x >= (float)c->gap_x && x < (float)(c->gap_x + c->gap_w)) {
            return true;
        }
    }
    return false;
}

static bool wk_support(float x, int footy)
{
    if (footy >= WK_GROUND) return !wk_in_open_gap(x);
    int32_t id = (int32_t)floorf(x / 64.0f);
    for (int d = -1; d <= 1; d++) {
        const wchunk_t *c = wk_chunk(id + d);
        for (int k = 0; k < c->np; k++) {
            if (footy == c->p[k].y && x >= (float)c->p[k].x - 1.0f &&
                x <= (float)(c->p[k].x + c->p[k].w)) {
                return true;
            }
        }
    }
    return false;
}

static const wlad_t *wk_ladder_at(float x, int footy, bool from_top)
{
    int32_t id = (int32_t)floorf(x / 64.0f);
    for (int d = -1; d <= 1; d++) {
        const wchunk_t *c = wk_chunk(id + d);
        for (int k = 0; k < c->nl; k++) {
            if (fabsf(x - (float)c->l[k].x) > 1.2f) continue;
            if (!from_top && footy == c->l[k].ybot) return &c->l[k];
            if (from_top && footy == c->l[k].ytop) return &c->l[k];
        }
    }
    return NULL;
}

static const wsli_t *wk_slide_at(float x, int footy)
{
    int32_t id = (int32_t)floorf(x / 64.0f);
    for (int d = -1; d <= 1; d++) {
        const wchunk_t *c = wk_chunk(id + d);
        for (int k = 0; k < c->ns; k++) {
            if (footy == c->s[k].ytop &&
                fabsf(x - (float)c->s[k].x) < 1.2f) {
                return &c->s[k];
            }
        }
    }
    return NULL;
}

/* SPLIT-CLOCK CAMERA: the naive integrator broke on mixed time
 * bases - wrap logic calls wk_cam(ts) with a SAVED stamp while the
 * render uses frame-grid t, and a stateful cam walked BACKWARD on
 * every negative delta (net scroll ~ zero). Fix: ONE blessed
 * advancer, ticked once per frame; wk_cam() is a pure reader that
 * ignores its argument, so no caller can corrupt state. */
static double s_cam_acc = -1.0;
static double s_cam_base;
static uint32_t s_cam_base_step;
static float s_scr_from = 1.0f, s_scr_to = 1.0f;
static uint32_t s_scr_step;
static float wk_scroll_at(uint32_t st);
static void wk_cam_tick(int64_t t)
{
    static int64_t lt;
    if (s_cam_acc < 0.0) {
        /* SHARED ORIGIN, DOUBLE PRECISION. The 0.29.3 per-unit
           zero-rebase fixed float rounding but silently destroyed
           the fleet invariant that made seams work: cam was pure
           f(TSF), IDENTICAL on every unit each frame - chunks
           matched at the bezel and the deterministic walker sim
           agreed on which panel he stood on. Double gives quantum
           ~1e-10 at TSF magnitude: precision AND shared origin.
           cam(t) = t*SPD for constant speed, regardless of when a
           unit booted; 'fleet walk speed' keeps changes lock-step. */
        s_cam_acc = (double)t / 1e6 * (double)WK_CAM_SPD;
        s_cam_base = s_cam_acc;
        s_cam_base_step = s_wk_steps;
        lt = t;
    }
    (void)lt;
    /* FOURTH CONVICTION (owner: "pixel-perfect seam crossings get
       rarer with uptime"): this function's own comment preaches
       cam = pure f(TSF), identical everywhere - and the show-era
       scroll multiplier quietly turned it into a LOCAL integral
       fed by LOCAL frame dt. Two integrators, two jitters,
       divergence forever. Slew + integration now live in the STEP
       DOMAIN (wk_cam_step, fixed 33ms quanta on the shared step
       count), and the owner's type-10 cam field carries POSITION
       as a 5 s authoritative snap. The comment tells the truth
       again. */
}
static float wk_cam(int64_t t)
{
    (void)t;
    return (float)s_cam_acc;
}

void whm_ui_walk_status(uint32_t *step, int *own, float *cam2,
                        float *x2)
{
    *step = s_wk_steps;
    *own = wk_i_own() ? 1 : 0;
    *cam2 = (float)s_cam_acc;
    *x2 = s_wk.x;
}

void whm_ui_scroll_q8(uint8_t *now8, uint8_t *tgt8)
{
    float nw = wk_scroll_at(s_wk_steps);
    *now8 = (uint8_t)(nw * 256.0f > 255.0f ? 255 : nw * 256.0f);
    *tgt8 = (uint8_t)(s_scr_to * 256.0f > 255.0f ? 255
                                                 : s_scr_to * 256.0f);
}

void whm_ui_cam_set(float c)
{
    if (wk_i_own()) return;                  /* the owner's cam IS
                                              the truth; never let
                                              an ownership flap
                                              cross-clobber it */
    double dx2 = (double)c - s_cam_acc;
    if (dx2 > 2.0 || dx2 < -2.0)
        printf("walker: CAM SNAP dx=%.2f (divergence alarm - "
               "should be ~0 forever)\n", dx2);
    s_cam_acc = (double)c;                 /* type-10 snap */
    s_cam_base = s_cam_acc;
    s_cam_base_step = s_wk_steps;
}

static void wk_respawn(int64_t t, uint8_t n)
{
    s_wk.x = wk_cam(t) + (float)n * 32.0f;
    s_wk.y = WK_GROUND;
    s_wk.vy = 0;
    s_wk.vx = 0;
    s_wk.dir = -1;
    s_wk.st = WK_WALK;
    s_wk.phase = 0;
    s_wk.timer = 0;
    s_wk.turn_cd = 0;
    s_wk.spd = 0.75f;
    s_wk.fresh = 0;
    s_wk_steps = 0;
    memset(s_wk_seen, 0, sizeof(s_wk_seen));   /* fresh outlook */
    s_wk_seen_wr = 0;
}

static int32_t s_wk_last_chunk = INT32_MIN;

/* SEVENTH - AND ARCHITECTURAL: three integrator repairs, three
   new leaks (starve on replay, starve on fast-forward, freeze in
   storms - the owner's cam= columns convicted each). Integration
   with guards is the wrong SHAPE: every execution path is another
   chance to skip or double-feed. The camera is now CLOSED-FORM:
   cam(step) = epoch_base + exact ramp-aware sum to step, rebased
   once per frame and on shared events (scroll targets, type-10
   snaps). It depends on WHICH STEP IT IS and on nothing else -
   no path can starve what no path feeds. */
static float wk_scroll_at(uint32_t st)
{
    if (s_scr_to == s_scr_from || st <= s_scr_step)
        return st <= s_scr_step ? s_scr_from : s_scr_to;
    float span = fabsf(s_scr_to - s_scr_from) * 30.0f; /* 1.0x/s */
    float f = (float)(st - s_scr_step) / span;
    if (f >= 1.0f) return s_scr_to;
    return s_scr_from + (s_scr_to - s_scr_from) * f;
}

static void wk_scroll_target(float v)
{
    s_scr_from = wk_scroll_at(s_wk_steps);
    s_scr_to = v;
    s_scr_step = s_wk_steps;
    s_wk_tgt_scroll = v;               /* legacy readers */
}

static void wk_cam_sync(void)
{
    /* BIDIRECTIONAL-EXACT (the +223 bomb, owner's capture,
       convicted by arithmetic: 6697 steps x 0.0333 = 223.01.
       Storm -> respawn zeroed s_wk_steps; the old one-way rebase
       slid base_step backward with NO sum - arming the bomb -
       then fast-forward re-integrated the ENTIRE history onto
       the old base. Rewinds now SUBTRACT the same closed sum
       they would add: any rewind + replay + fast-forward nets
       zero by construction, whatever path caused it. */
    if (s_wk_steps >= s_cam_base_step) {
        for (uint32_t s = s_cam_base_step; s < s_wk_steps; s++)
            s_cam_base += 0.0333 * (double)WK_CAM_SPD *
                          (double)wk_scroll_at(s + 1);
    } else {
        double undo = 0.0;
        for (uint32_t s = s_wk_steps; s < s_cam_base_step; s++)
            undo += 0.0333 * (double)WK_CAM_SPD *
                    (double)wk_scroll_at(s + 1);
        s_cam_base -= undo;
        printf("walker: cam rewind %lu->%lu (undo %.2f px)\n",
               (unsigned long)s_cam_base_step,
               (unsigned long)s_wk_steps, undo);
    }
    s_cam_base_step = s_wk_steps;
    s_cam_acc = s_cam_base;
    s_wk_scroll = wk_scroll_at(s_wk_steps);
}

static void wk_step(int64_t t, uint8_t n)
{
    uint8_t st_in = (uint8_t)s_wk.st;
    s_wk_draws = 0;              /* pure RNG: draw# restarts per step */
    for (int ii = 0; ii < 32; ii++) {        /* input log: apply at
                                                the stamped step */
        if (!s_wki[ii].valid || s_wki[ii].step != s_wk_steps)
            continue;
        uint8_t a = s_wki[ii].act;
        if (a == 0) { s_wkc.mode = 0; }
        else {
            s_wkc.mode = 1;
            s_wkc.until = s_wk_steps + 300;
            if (a == 1) s_wkc.vx = -1;
            else if (a == 2) s_wkc.vx = 1;
            else if (a == 3) s_wkc.vx = 0;
            else if (a == 4) s_wkc.jump = 1;
            else if (a == 5) {           /* SEEK world-x: the visit
                                            verb - steer-to-edge is
                                            just an input */
                s_wkc.seek = 1;
                s_wkc.seekx = s_wki[ii].arg;
            }
            if (a != 5 && a != 4) s_wkc.seek = 0;
        }
    }
    if (s_wkc.mode == 1 && s_wk_steps >= s_wkc.until) {
        s_wkc.mode = 0;          /* shakes off, wanders again */
        s_wkc.jump = 0;
    }
    /* STEP-TIME GENERATOR (doctrine 13): reward marks are a pure
       function of the trajectory - one mark per chunk ENTRY, inside
       the step, so a late entrant's replay produces the identical
       seen[] a continuous runner built. The 0.32.0 wall-clock beacon
       marks made replay != continuous: the -21px entry drift. */
    {
        int32_t ch = (int32_t)floorf(s_wk.x / 64.0f);
        if (ch != s_wk_last_chunk) {
            s_wk_last_chunk = ch;
            wk_mark(ch);
        }
    }
    #define WK_ST_TRACE()                                            \
        do { if (!s_wk_shadowing && (uint8_t)s_wk.st != st_in) {                        \
            s_wkh[s_wkh_w].step = s_wk_steps;                        \
            s_wkh[s_wkh_w].from = st_in;                             \
            s_wkh[s_wkh_w].to = (uint8_t)s_wk.st;                    \
            s_wkh_w = (uint8_t)((s_wkh_w + 1) % 8); } } while (0)
    float center = wk_cam(t) + (float)n * 32.0f;
    switch (s_wk.st) {
    case WK_WALK: {
        if (s_wkc.mode == 1) {           /* USER: instant heel-turn,
                                            autonomy suspended */
            if (s_wkc.seek) {
                float dx2 = s_wkc.seekx - s_wk.x;
                if (fabsf(dx2) < 1.0f) {
                    s_wkc.seek = 0;      /* arrived: stand */
                    s_wkc.vx = 0;
                } else {
                    s_wkc.vx = dx2 > 0 ? 1 : -1;
                }
            }
            if (s_wkc.vx) s_wk.dir = s_wkc.vx;
            if (s_wkc.vx) s_wk.x += (float)s_wk.dir * s_wk.spd;
            s_wk.phase++;
            int ufy = (int)lroundf(s_wk.y);
            if (s_wkc.jump && ufy >= WK_GROUND - 0) {
                s_wkc.jump = 0;
                s_wk.st = WK_CROUCH;     /* the CHARGED long jump -
                                            user gets the good one */
                s_wk.timer = 8;
            } else {
                s_wkc.jump = 0;          /* airborne press: consumed */
            }
            break;
        }
        s_wk.x += (float)s_wk.dir * s_wk.spd;
        s_wk.phase++;
        if (s_wk.turn_cd) s_wk.turn_cd--;
        int fy = (int)lroundf(s_wk.y);
        /* an open gap lip DEMANDS the long jump (traversal guaranteed) */
        if (fy >= WK_GROUND &&
            wk_in_open_gap(s_wk.x + (float)s_wk.dir * 2.0f)) {
            s_wk.st = WK_CROUCH;
            s_wk.timer = 8;               /* wind-up */
            break;
        }
        const wlad_t *lu = wk_ladder_at(s_wk.x, fy, false);
        const wlad_t *ld = wk_ladder_at(s_wk.x, fy, true);
        const wsli_t *sl = wk_slide_at(s_wk.x, fy);
        uint32_t r = wk_rnd();
        if (sl && (r % 100) <
            (uint32_t)(wk_reward_fun(0x20000000 ^ sl->x) * 45.0f)) {
            wk_mark(0x20000000 ^ sl->x);
            s_wk.st = WK_SLIDE;
            s_wk.sdir = sl->dir;
            s_wk.tgt = (float)sl->ybot;
            break;
        }
        if (lu && (r % 100) <
            (uint32_t)(wk_reward_fun(0x40000000 ^ lu->x) * 60.0f)) {
            wk_mark(0x40000000 ^ lu->x);
            s_wk.st = WK_LADDER;
            s_wk.tgt = (float)lu->ytop;
            s_wk.fresh = 1;
            break;
        }
        if (ld && (r % 100) <
            (uint32_t)(wk_reward_fun(0x40000000 ^ ld->x) * 30.0f)) {
            wk_mark(0x40000000 ^ ld->x);
            s_wk.st = WK_LADDER;
            s_wk.tgt = (float)ld->ybot;
            break;
        }
        int32_t id = (int32_t)floorf(s_wk.x / 64.0f);
        for (int d = -1; d <= 1 && s_wk.st == WK_WALK; d++) {
            const wchunk_t *c = wk_chunk(id + d);
            for (int k = 0; k < c->np; k++) {
                float edge = s_wk.dir > 0
                    ? (float)c->p[k].x
                    : (float)(c->p[k].x + c->p[k].w);
                if (fabsf(s_wk.x - edge) < 1.0f &&
                    c->p[k].y < fy) {
                    int dh = fy - c->p[k].y;
                    float rw = wk_reward_plat(id + d, k);
                    uint32_t gate = (uint32_t)(rw * 90.0f) + 8;
                    if (dh <= 9 && (wk_rnd() % 100) < gate) {
                        wk_mark((id + d) * 8 + k);
                        s_wk.fresh = rw > 0.8f;
                        s_wk.st = WK_JUMP;      /* quick HOP up */
                        s_wk.vy = -1.6f;
                        s_wk.vx = (float)s_wk.dir * 0.45f;
                    } else if (dh <= WK_REACH &&
                               (wk_rnd() % 100) < gate) {
                        wk_mark((id + d) * 8 + k);
                        s_wk.fresh = rw > 0.8f;
                        s_wk.st = WK_CLIMB;
                        s_wk.tgt = (float)c->p[k].y;
                    } else if (!s_wk.turn_cd) {
                        s_wk.st = WK_IDLE;      /* consider... */
                        s_wk.timer = 14;
                        s_wk.dir = -s_wk.dir;   /* ...then one turn */
                        s_wk.turn_cd = 60;
                    }
                    break;
                }
            }
        }
        if (s_wk.st != WK_WALK) break;
        if (!wk_support(s_wk.x, fy)) {
            s_wk.st = WK_FALL;
            s_wk.vy = 0;
            break;
        }
        float off = s_wk.x - center;
        bool away = (off > 0 && s_wk.dir > 0) ||
                    (off < 0 && s_wk.dir < 0);
        if (fabsf(off) > 60.0f && away && !s_wk.turn_cd) {
            s_wk.dir = off > 0 ? -1 : 1;   /* leash, once, latched */
            s_wk.turn_cd = 50;
        } else if (fy >= WK_GROUND && !s_wk.turn_cd &&
                   r % 2600 == 3) {        /* the camping ritual */
            s_wk.tgt = center + 26.0f;
            s_wk.dir = s_wk.x < s_wk.tgt ? 1 : -1;
            if (!s_wk_pure)
                s_wk.st = WK_GOCHAIR;
            s_wk.turn_cd = 90;
        } else if (fy >= WK_GROUND && !s_wk.turn_cd &&
                   off < -8.0f && r % 3800 == 5) {
            s_wk.st = WK_SHIMMY;           /* bezel-wrap ritual */
            s_wk.dir = -1;
        } else if (r % 700 == 0) {
            s_wk.st = WK_IDLE;
            s_wk.timer = (uint16_t)(25 + wk_rnd() % 70);
        } else if (!s_wk.turn_cd && r % 1600 == 2) {
            s_wk.st = WK_JUMP;             /* exuberant hop */
            s_wk.vy = -1.4f;
            s_wk.vx = (float)s_wk.dir * 0.3f;
        } else if (!s_wk.turn_cd && r % 140 == 1) {
            float sl2 = wk_scout(s_wk.x, -1);
            float sr2 = wk_scout(s_wk.x, 1);
            int8_t toward = off > 0 ? -1 : 1;
            if (fabsf(sl2 - sr2) > 0.15f && (wk_rnd() % 5)) {
                s_wk.dir = sl2 > sr2 ? -1 : 1;   /* toward the fresh */
            } else {
                s_wk.dir = ((wk_rnd() % 4) == 0) ? (int8_t)-toward
                                                 : toward;
            }
            s_wk.turn_cd = 45;
            s_wk.spd = (wk_rnd() & 1) ? 0.75f : 0.55f;  /* stroll */
        }
        break;
    }
    case WK_CROUCH:
        s_wk.phase++;
        if (s_wk.timer) { s_wk.timer--; break; }
        s_wk.st = WK_JUMP;                /* CHARGED long-jump */
        s_wk.vy = -1.9f;
        s_wk.vx = (float)s_wk.dir * 1.35f;
        break;
    case WK_JUMP:
        s_wk.x += s_wk.vx;
        s_wk.vy += 0.2f;
        s_wk.y += s_wk.vy;
        s_wk.phase++;
        if (s_wk.vy > 0.0f) {
            int fy = (int)lroundf(s_wk.y);
            if (s_wk.y >= (float)WK_GROUND) {
                s_wk.y = WK_GROUND;
                if (wk_in_open_gap(s_wk.x)) {   /* shouldn't happen */
                    s_wk.x += (float)s_wk.dir * 4.0f;
                }
                s_wk.st = WK_LAND;
                s_wk.timer = 3;
            } else if (wk_support(s_wk.x, fy)) {
                s_wk.y = (float)fy;
                s_wk.st = WK_LAND;
                s_wk.timer = 3;
            }
        }
        break;
    case WK_LAND:
        if (s_wk.timer) s_wk.timer--;
        else { s_wk.st = WK_WALK; s_wk.vx = 0; s_wk.vy = 0; }
        break;
    case WK_GOCHAIR:
        s_wk.x += (float)s_wk.dir * 0.65f;
        s_wk.phase++;
        if (!wk_support(s_wk.x, (int)lroundf(s_wk.y))) {
            s_wk.st = WK_WALK;             /* pit en route: abort */
            s_wk.dir = -s_wk.dir;
            break;
        }
        if (fabsf(s_wk.x - s_wk.tgt) < 1.0f) {
            s_wk.st = WK_SETUP;
            s_wk.timer = 10;
        }
        break;
    case WK_SETUP:
        if (s_wk.timer) s_wk.timer--;
        else {
            s_wk.st = WK_SIT;
            /* SIT LONGER (owner): real contemplation, ~23-40 s. */
            s_wk.timer = (uint16_t)(700 + wk_rnd() % 500);
            s_wk.dir = -1;                 /* face the oncoming world */
        }
        break;
    case WK_SIT:
        if (s_fw_hold) break;                 /* the show must go on */
        {   /* PACK UP BEFORE THE VIEW LEAVES HIM (owner): as the
               world scrolls he drifts toward the trailing edge -
               stand and pack while still on screen. Pure
               f(x, cam): shared, fork-free. */
            float sx9 = s_wk.x - wk_cam(t);
            if (sx9 < 10.0f) {
                s_wk.st = WK_PACK;
                s_wk.timer = 8;
                break;
            }
        }
        if (s_wk.timer) s_wk.timer--;
        else { s_wk.st = WK_PACK; s_wk.timer = 8; }
        break;
    case WK_PACK:
        if (s_wk.timer) s_wk.timer--;
        else { s_wk.st = WK_WALK; s_wk.turn_cd = 90; }
        break;
    case WK_SHIMMY: {
        float cam2 = s_wk.x;               /* pinned via tgt below */
        (void)cam2;
        s_wk.phase++;
        s_wk.y -= 0.5f;
        int fy2 = (int)lroundf(s_wk.y);
        int32_t rid = (int32_t)floorf((s_wk.x + 250.0f) / 64.0f);
        for (int d = 0; d <= 1 && s_wk.st == WK_SHIMMY; d++) {
            const wchunk_t *c = wk_chunk(rid + d);
            for (int k = 0; k < c->np; k++) {
                if (c->p[k].y != fy2) continue;
                /* platform hugging the strip's right edge? */
                if ((float)(c->p[k].x + c->p[k].w) >
                    s_wk.tgt - 6.0f) {
                    s_wk.x = s_wk.tgt - 5.0f;      /* POP! */
                    s_wk.y = (float)c->p[k].y;
                    s_wk.st = WK_POP;
                    s_wk.timer = 6;
                    s_wk.dir = -1;
                    s_wk.turn_cd = 60;
                    break;
                }
            }
        }
        if (s_wk.st == WK_SHIMMY && s_wk.y < 13.0f) {
            s_wk.st = WK_FALL;             /* nothing up here: bail */
            s_wk.vy = 0.4f;
        }
        break;
    }
    case WK_POP:
        if (s_wk.timer) s_wk.timer--;
        else s_wk.st = WK_WALK;
        break;
    case WK_CLIMB:
        s_wk.y -= 1.0f;
        s_wk.phase++;
        if (s_wk.y <= s_wk.tgt) {
            s_wk.y = s_wk.tgt;
            if (s_wk.fresh && (wk_rnd() % 5) < 2) {
                s_wk.st = WK_IDLE;      /* take in the NEW view -
                                           always pays best */
                s_wk.timer = (uint16_t)(20 + wk_rnd() % 26);
            } else {
                s_wk.st = WK_WALK;
            }
            s_wk.fresh = 0;
        }
        break;
    case WK_LADDER:
        s_wk.phase++;
        if (s_wk.y > s_wk.tgt) {
            s_wk.y -= 0.8f;
            if (s_wk.y <= s_wk.tgt) { s_wk.y = s_wk.tgt;
                                      s_wk.st = WK_WALK; }
        } else {
            s_wk.y += 0.8f;
            if (s_wk.y >= s_wk.tgt) { s_wk.y = s_wk.tgt;
                                      s_wk.st = WK_WALK; }
        }
        break;
    case WK_SLIDE:
        s_wk.x += (float)s_wk.sdir * 1.3f;
        s_wk.y += 1.3f;
        s_wk.phase++;
        if (s_wk.y >= s_wk.tgt) {
            s_wk.y = s_wk.tgt;
            s_wk.dir = s_wk.sdir;
            s_wk.st = wk_support(s_wk.x, (int)s_wk.y) ? WK_WALK
                                                      : WK_FALL;
            s_wk.vy = 0.6f;
        }
        break;
    case WK_FALL:
        s_wk.vy += 0.2f;
        if (s_wk.vy > 2.6f) s_wk.vy = 2.6f;
        for (float dy2 = 0; dy2 < s_wk.vy; dy2 += 0.5f) {
            s_wk.y += 0.5f;
            if (s_wk.y >= (float)WK_GROUND) { s_wk.y = WK_GROUND; break; }
            if (wk_support(s_wk.x, (int)lroundf(s_wk.y))) break;
        }
        if (s_wk.y >= (float)WK_GROUND ||
            wk_support(s_wk.x, (int)lroundf(s_wk.y))) {
            if (s_wk.y >= (float)WK_GROUND && wk_in_open_gap(s_wk.x)) {
                s_wk.x += (float)s_wk.dir * 3.0f;    /* rim rescue */
            } else {
                s_wk.st = WK_LAND;
                s_wk.timer = 2;
                s_wk.vy = 0;
            }
        }
        break;
    case WK_IDLE:
        if (s_wk.timer) {
            s_wk.timer--;
            if (s_wk.timer % 30 == 0) s_wk.dir = -s_wk.dir;
        } else {
            s_wk.st = WK_WALK;
        }
        break;
    }
    WK_ST_TRACE();
}

static void wk_px(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= 0 && x < W && y >= 0 && y < H) {
        whm_display_set_pixel((uint16_t)x, (uint16_t)y, r, g, b);
    }
}

/* daylight factor from the REAL wall clock: 0 = night, 1 = day */
static bool wall_now(struct tm *lt, suseconds_t *usec);   /* fwd */
static float wk_daylight(void)
{
    struct tm lt;
    if (!wall_now(&lt, NULL)) return 1.0f;
    float hm = (float)lt.tm_hour + (float)lt.tm_min / 60.0f;
    if (hm < 6.0f || hm >= 20.0f) return 0.0f;
    if (hm < 8.0f)  return (hm - 6.0f) / 2.0f;
    if (hm < 17.0f) return 1.0f;
    return 1.0f - (hm - 17.0f) / 3.0f;
}

/* ================== MIDNIGHT FIREWORKS ============================
 * Every night: the walker finds his spot, unfolds the chair, and at
 * 00:00:00 the sky blooms for ten seconds. He lingers five more,
 * packs, and wanders on. Dec 31: a five-minute, three-act NYE
 * spectacular - building volleys, escalating barrages, a ten-count
 * of grand shells - then micro-bursts bloom along glyph coordinates
 * spelling HAPPY / NEW YEAR, hold as twinkling constellations,
 * cross-fade to solid text with the incoming year. Choreography is
 * seeded by that year: palettes, petal counts, rhythms - a different
 * show every year, forever. Hidden 'fw' command tests it all. */
#define FW_MAX   110
#define FWD_MAX  240
typedef struct { float x, y, vx, vy; uint8_t r, g, b;
                 uint16_t age, life; uint8_t kind; } fwp_t;
EXT_RAM_BSS_ATTR static fwp_t s_fw[FW_MAX];   /* s_fw_hold hoisted to top (WK_SIT) */
static int s_fwn;
EXT_RAM_BSS_ATTR static struct { uint8_t x, y, r, g, b; int32_t born_ms; } s_fwd[FWD_MAX];
static int s_fwdn;
static struct {
    uint8_t phase;        /* 0 off 1 stage 2 show 3 linger 4 nye */
    uint8_t test, fast, nye;
    uint8_t fleet, initiator;
    int64_t t0;           /* esp us at phase start (test) */
    int64_t start_tsf;    /* fleet: show T0 on the shared clock */
    int64_t last_ann;
    int year;
    char tz[8];
    uint32_t seed;
    int32_t last_launch_ms, ms_in, cd_last;
    float scene_x0;       /* plaza world-x; <0 = none */
    float banner;         /* hoist fraction 0..1 */
    struct { float x; int8_t dir; uint8_t on; } friendw;
} s_show;
static bool wk_nye_window(int32_t chunk)
{
    if (s_show.phase != 4 || s_show.scene_x0 < 0) return false;
    float c0 = (float)chunk * 64.0f;
    return c0 + 64.0f > s_show.scene_x0 - 8.0f &&
           c0 < s_show.scene_x0 + 136.0f;
}
static uint32_t fw_rnd(uint32_t *st)
{
    *st ^= *st << 13; *st ^= *st >> 17; *st ^= *st << 5;
    return *st;
}

static const uint8_t fw_font[][5] = {   /* 3x5 rows, bit2=left; A-Z */
    { 2, 5, 7, 5, 5 }, { 6, 5, 6, 5, 6 }, { 3, 4, 4, 4, 3 },
    { 6, 5, 5, 5, 6 }, { 7, 4, 6, 4, 7 }, { 7, 4, 6, 4, 4 },
    { 3, 4, 5, 5, 3 }, { 5, 5, 7, 5, 5 }, { 7, 2, 2, 2, 7 },
    { 1, 1, 1, 5, 2 }, { 5, 6, 4, 6, 5 }, { 4, 4, 4, 4, 7 },
    { 5, 7, 7, 5, 5 }, { 5, 7, 7, 7, 5 }, { 2, 5, 5, 5, 2 },
    { 6, 5, 6, 4, 4 }, { 2, 5, 5, 7, 3 }, { 6, 5, 6, 5, 5 },
    { 3, 4, 2, 1, 6 }, { 7, 2, 2, 2, 2 }, { 5, 5, 5, 5, 7 },
    { 5, 5, 5, 5, 2 }, { 5, 5, 7, 7, 5 }, { 5, 5, 2, 5, 5 },
    { 5, 5, 2, 2, 2 }, { 7, 1, 2, 4, 7 },
    { 0, 0, 0, 0, 0 },                                  /* space */
    { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 },
    { 7, 1, 3, 1, 7 }, { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
    { 7, 4, 7, 5, 7 }, { 7, 1, 2, 2, 2 }, { 7, 5, 7, 5, 7 },
    { 7, 5, 7, 1, 7 },
};
static int fw_glyph(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    return 26;
}

/* OTA STATUS SCREEN: the receiving unit takes over its own panel -
 * which also QUIESCES the pattern loop (walker/shadow/storms), the
 * exact load suspect in the 11KB truncation. Failure restores the
 * previous screen; success reboots, so nothing needs saving. */
static struct {
    volatile bool active;
    volatile uint32_t got_kb, total_kb;
    volatile uint8_t phase;      /* 0 pull, 1 verify, 2 boot, 3 fail,
                                    4 erase */
    volatile uint8_t count;      /* reboot countdown seconds */
    char from[8], to[8], why[20];
    int64_t fail_at;
} s_otui;

void whm_ui_ota_begin(const char *cur, const char *inc, uint32_t kb)
{
    strlcpy(s_otui.from, cur ? cur : "?", sizeof(s_otui.from));
    strlcpy(s_otui.to, inc && inc[0] ? inc : "?", sizeof(s_otui.to));
    s_otui.total_kb = kb;
    s_otui.got_kb = 0;
    s_otui.phase = 0;
    s_otui.why[0] = 0;
    s_otui.active = true;
}

void whm_ui_ota_target(const char *inc)
{
    if (inc && inc[0])
        strlcpy(s_otui.to, inc, sizeof(s_otui.to));
}

bool whm_ui_ota_active(void) { return s_otui.active; }

void whm_ui_walk_params_poke(void) { s_wkp_poke = true; }

uint32_t whm_ui_walk_step(void) { return s_wk_steps; }

void whm_ui_walk_input_rx(uint8_t act, uint32_t exec_step, float arg)
{
    s_wki[s_wki_w] = (__typeof__(s_wki[0])){ .step = exec_step,
        .act = act, .arg = arg, .valid = 1 };
    s_wki_w = (uint8_t)((s_wki_w + 1) & 31);
    for (int i = 0; i < WKF_N; i++)       /* input invalidates the
                                             future (doctrine 15) */
        s_wkf[i].valid = 0;
    if (exec_step <= s_wk_steps && s_wk_steps)
        s_wko.resync = 1;                 /* late: replay consumes
                                             the log at the right
                                             step */
}

void whm_ui_ota_progress(uint32_t kb) { s_otui.got_kb = kb; }

void whm_ui_ota_count(uint8_t n) { s_otui.count = n; }

void whm_ui_ota_phase(uint8_t ph, const char *why)
{
    s_otui.phase = ph;
    if (why) strlcpy(s_otui.why, why, sizeof(s_otui.why));
    if (ph == 3) s_otui.fail_at = esp_timer_get_time();
}

static void ota_text(const char *s, int x0, int y0, uint8_t r,
                     uint8_t g, uint8_t b)
{
    for (int k = 0; s[k]; k++) {
        char ch = s[k];
        if (ch == ' ') continue;
        if (ch == '.') { wk_px(x0 + k * 4 + 1, y0 + 4, r, g, b);
            continue; }
        if (ch == '%') {                 /* corner dots + slash */
            wk_px(x0 + k * 4, y0, r, g, b);
            wk_px(x0 + k * 4 + 2, y0, r, g, b);
            wk_px(x0 + k * 4 + 1, y0 + 2, r, g, b);
            wk_px(x0 + k * 4, y0 + 4, r, g, b);
            wk_px(x0 + k * 4 + 2, y0 + 4, r, g, b);
            continue;
        }
        if (ch == '>') {
            wk_px(x0 + k * 4, y0 + 1, r, g, b);
            wk_px(x0 + k * 4 + 1, y0 + 2, r, g, b);
            wk_px(x0 + k * 4, y0 + 3, r, g, b);
            continue;
        }
        if (ch == '/') {
            wk_px(x0 + k * 4 + 2, y0, r, g, b);
            wk_px(x0 + k * 4 + 1, y0 + 2, r, g, b);
            wk_px(x0 + k * 4, y0 + 4, r, g, b);
            continue;
        }
        const uint8_t *gl = fw_font[fw_glyph(ch)];
        for (int ry = 0; ry < 5; ry++)
            for (int rx = 0; rx < 3; rx++)
                if (gl[ry] & (4 >> rx))
                    wk_px(x0 + k * 4 + rx, y0 + ry, r, g, b);
    }
}

static bool ota_screen(int64_t t)
{
    if (!s_otui.active) return false;
    if (s_otui.phase == 3 && t - s_otui.fail_at > 6000000) {
        s_otui.active = false;           /* restore previous screen */
        return false;
    }
    whm_display_fill(2, 3, 8);
    ota_text("OTA", 26, 3, 240, 200, 90);
    char ln[20];
    snprintf(ln, sizeof(ln), "%s > %s", s_otui.from, s_otui.to);
    ota_text(ln, (int)(32 - (int)strlen(ln) * 2), 13, 150, 170, 220);
    uint32_t tot = s_otui.total_kb ? s_otui.total_kb : 1;
    uint32_t pc = s_otui.got_kb * 100u / tot;
    if (pc > 100) pc = 100;
    int bx = 5, by = 27, bw = 54, bh = 7;
    for (int x2 = bx - 1; x2 <= bx + bw; x2++) {
        wk_px(x2, by - 1, 70, 80, 110);
        wk_px(x2, by + bh, 70, 80, 110);
    }
    for (int y2 = by - 1; y2 <= by + bh; y2++) {
        wk_px(bx - 1, y2, 70, 80, 110);
        wk_px(bx + bw, y2, 70, 80, 110);
    }
    int fill = (int)(pc * (uint32_t)bw / 100u);
    for (int x2 = 0; x2 < fill; x2++)
        for (int y2 = 0; y2 < bh; y2++)
            wk_px(bx + x2, by + y2, 60, 190, 120);
    int sweep = (int)((t / 60000) % (int64_t)(bw + 12)) - 6;
    for (int d2 = -2; d2 <= 2; d2++) {   /* running light, house */
        int x2 = sweep + d2;
        if (x2 >= 0 && x2 < fill)
            for (int y2 = 1; y2 < bh - 1; y2++)
                wk_px(bx + x2, by + y2, 120, 235, 170);
    }
    snprintf(ln, sizeof(ln), "%lu%%", (unsigned long)pc);
    ota_text(ln, 27, 37, 220, 220, 230);
    snprintf(ln, sizeof(ln), "%lu/%luKB",
             (unsigned long)s_otui.got_kb, (unsigned long)tot);
    ota_text(ln, (int)(32 - (int)strlen(ln) * 2), 45, 120, 130,
             160);
    if (s_otui.phase == 4)
        ota_text("ERASING", 18, 55, 238, 180, 60);
    else if (s_otui.phase == 0)
        ota_text("PULL", 24, 55, 150, 170, 220);
    else if (s_otui.phase == 1)
        ota_text("VERIFYING", 14, 55, 240, 200, 90);
    else if (s_otui.phase == 2) {
        if (s_otui.count) {
            char rb[16];
            snprintf(rb, sizeof(rb), "REBOOT IN %u",
                     (unsigned)s_otui.count);
            ota_text(rb, 10, 55, 60, 220, 130);
        } else {
            ota_text("REBOOTING", 14, 55, 60, 220, 130);
        }
    }
    else {
        ota_text("FAILED", 20, 55, 240, 70, 70);
        ota_text(s_otui.why, 2, 21, 240, 100, 100);
    }
    return true;
}

static void fw_pal(uint32_t *st, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t pals[6][3] = {
        { 255, 60, 24 }, { 255, 170, 30 }, { 60, 220, 70 },
        { 60, 120, 255 }, { 220, 60, 220 }, { 240, 240, 200 },
    };
    int i = (int)(fw_rnd(st) % 6u);
    *r = pals[i][0]; *g = pals[i][1]; *b = pals[i][2];
}

static void fw_burst(float x, float y, int n, int style, uint8_t r,
                     uint8_t g, uint8_t b)
{
    for (int i = 0; i < n && s_fwn < FW_MAX; i++) {
        float a = 6.2832f * (float)i / (float)n;
        float sp = style == 2 ? 6.0f : 11.0f + (float)(i % 3) * 2.0f;
        fwp_t *p = &s_fw[s_fwn++];
        p->x = x; p->y = y;
        p->vx = cosf(a) * sp;
        p->vy = sinf(a) * sp - 2.0f;
        p->r = r; p->g = g; p->b = b;
        p->age = 0;
        p->life = style == 2 ? 1500 : 800 + (uint16_t)(i % 4) * 90;
        p->kind = 0;
    }
}

static void fw_launch(float x, int style, uint8_t r, uint8_t g,
                      uint8_t b, float apex)
{
    if (s_fwn >= FW_MAX) return;
    fwp_t *p = &s_fw[s_fwn++];
    p->x = x; p->y = (float)(WK_GROUND - 1);
    p->vx = 0; p->vy = -26.0f - apex;
    p->r = r; p->g = g; p->b = b;
    p->age = 0; p->life = 520; p->kind = 1;
    p->kind |= (uint8_t)(style << 4);
}

static void fw_step(int dt_ms)
{
    float dt = (float)dt_ms / 1000.0f;
    for (int i = 0; i < s_fwn; i++) {
        fwp_t *p = &s_fw[i];
        p->age += (uint16_t)dt_ms;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vy += ((p->kind & 15) == 1 ? 2.0f : 9.5f) * dt;
        p->vx *= 0.985f;
        if ((p->kind & 15) == 1 &&
            (p->vy > -3.0f || p->age >= p->life)) {
            int style = p->kind >> 4;
            float bx = p->x, by = p->y;
            uint8_t r = p->r, g = p->g, b = p->b;
            s_fw[i--] = s_fw[--s_fwn];
            fw_burst(bx, by, style == 2 ? 26 : 10 + style * 7, style,
                     r, g, b);
            continue;
        }
        if (p->age >= p->life || p->y > (float)(WK_GROUND + 2)) {
            s_fw[i--] = s_fw[--s_fwn];
        }
    }
}

static void fw_render(void)
{
    for (int i = 0; i < s_fwn; i++) {
        fwp_t *p = &s_fw[i];
        float k = 1.0f - (float)p->age / (float)p->life;
        if (k < 0) k = 0;
        int x = (int)p->x, y = (int)p->y;
        wk_px(x, y, (uint8_t)(p->r * k), (uint8_t)(p->g * k),
              (uint8_t)(p->b * k));
        if ((p->kind & 15) == 1) {
            wk_px(x, y + 1, (uint8_t)(120 * k), (uint8_t)(90 * k),
                  (uint8_t)(30 * k));
        }
    }
}

static void fw_text_dots(const char *s, int y0, int32_t born_ms,
                         int32_t stagger)
{
    int wpx = 0;
    for (const char *c = s; *c; c++) wpx += 4;
    int x = (64 - wpx + 1) / 2;
    int32_t bm = born_ms;
    for (const char *c = s; *c; c++, x += 4) {
        const uint8_t *gl = fw_font[fw_glyph(*c)];
        for (int ry = 0; ry < 5; ry++)
            for (int rx = 0; rx < 3; rx++)
                if ((gl[ry] >> (2 - rx)) & 1) {
                    if (s_fwdn < FWD_MAX) {
                        s_fwd[s_fwdn].x = (uint8_t)(x + rx);
                        s_fwd[s_fwdn].y = (uint8_t)(y0 + ry);
                        uint32_t st = s_show.seed + (uint32_t)s_fwdn;
                        fw_pal(&st, &s_fwd[s_fwdn].r,
                               &s_fwd[s_fwdn].g, &s_fwd[s_fwdn].b);
                        s_fwd[s_fwdn].born_ms = bm;
                        s_fwdn++;
                    }
                }
        bm += stagger;
    }
}

static void fw_dots_render(int32_t ms, int64_t t, float fade)
{
    for (int i = 0; i < s_fwdn; i++) {
        int32_t a = ms - s_fwd[i].born_ms;
        if (a < 0) continue;
        float k;
        if (a < 500) {
            k = (float)a / 500.0f;             /* bloom */
            if (a < 120 && s_fwn < FW_MAX - 4) {
                fw_burst((float)s_fwd[i].x, (float)s_fwd[i].y, 6, 0,
                         s_fwd[i].r, s_fwd[i].g, s_fwd[i].b);
                s_fwd[i].born_ms -= 1;          /* burst once trick */
            }
        } else {
            k = 0.75f + 0.25f * sinf((float)t / 2.2e5f + (float)i);
        }
        k *= fade;
        wk_px(s_fwd[i].x, s_fwd[i].y, (uint8_t)(s_fwd[i].r * k),
              (uint8_t)(s_fwd[i].g * k), (uint8_t)(s_fwd[i].b * k));
    }
}

/* virtual wall-seconds into the show day; test modes synthesize */
static bool fw_clock(int64_t t, int *sec_of_day, struct tm *lt)
{
    if (s_show.test) {
        int32_t el = (int32_t)((t - s_show.t0) / 1000000LL);
        if (s_show.fast) el *= 10;
        *sec_of_day = (s_show.nye ? (23 * 3600 + 55 * 60 - 20)
                                  : (23 * 3600 + 59 * 60 + 48 - 8))
                      + el;
        lt->tm_mon = 11; lt->tm_mday = 31;
        lt->tm_year = s_show.year - 1901;
        return true;
    }
    if (!wall_now(lt, NULL)) return false;
    *sec_of_day = lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;
    return true;
}

static void fw_choreo_nye(int32_t ms);
static void fw_tick(int64_t t)
{
    static int64_t last_t;
    int dt = last_t ? (int)((t - last_t) / 1000) : 16;
    if (dt > 66) dt = 66;
    last_t = t;
    struct tm lt;
    int sod;
    if (!fw_clock(t, &sod, &lt)) return;
    bool nye_day = (lt.tm_mon == 11 && lt.tm_mday == 31) ||
                   s_show.nye;
    int32_t mid = 24 * 3600;
    switch (s_show.phase) {
    case 0:
        if (nye_day && sod >= mid - 5 * 60 && sod < mid + 600) {
            /* INITIATOR: my local 23:55 - take the whole fleet */
            s_show.year = s_show.test ? s_show.year
                                      : lt.tm_year + 1900 + 1;
            char lbl[8] = "";
            whm_settings_get_str("tz_lbl", lbl, sizeof(lbl));
            if (!lbl[0]) strftime(lbl, sizeof(lbl), "%Z", &lt);
            int64_t tsf0 = whm_wifi_tsf_now() -
                           (int64_t)(sod - (mid - 300)) * 1000000LL;
            whm_ui_nye_join("", lbl, s_show.year, tsf0);
            s_show.initiator = 1;
            whm_sync_nye_send(lbl, (uint16_t)s_show.year, tsf0);
        } else if (sod >= mid - 22 && sod < mid - 12) {
            /* NIGHTLY = fleet mini-takeover (year 0): local-clock
               st-forcing hard-forked replicas across timezones every
               midnight; now ONE initiator broadcasts and every
               replica runs the identical TSF-locked 25 s script. */
            int64_t tsf0 = whm_wifi_tsf_now() -
                           (int64_t)(sod - (mid - 20)) * 1000000LL;
            whm_ui_nye_join("", "", 0, tsf0);
            s_show.initiator = 1;
            whm_sync_nye_send("", 0, tsf0);
        }
        break;
    case 1:
        if (nye_day && sod >= mid - 5 * 60) {
            s_show.phase = 4;
            s_show.ms_in = 0;
            s_fwdn = 0;
        } else if (!nye_day && (sod >= mid || sod < 2)) {
            s_show.phase = 2;
            s_show.ms_in = 0;
        }
        break;
    case 2: {                                  /* nightly: 10s */
        s_show.ms_in += dt;
        uint32_t st = s_show.seed + (uint32_t)(s_show.ms_in / 1400);
        if (s_show.ms_in - s_show.last_launch_ms >
                (int32_t)(900 + fw_rnd(&st) % 700) &&
            s_show.ms_in < 8500) {
            s_show.last_launch_ms = s_show.ms_in;
            uint8_t r, g, b;
            fw_pal(&st, &r, &g, &b);
            fw_launch(8.0f + (float)(fw_rnd(&st) % 48u),
                      (int)(fw_rnd(&st) % 3u), r, g, b,
                      (float)(fw_rnd(&st) % 8u));
        }
        if (s_show.ms_in > 10000 && s_fwn == 0) {
            s_show.phase = 3;
            s_show.ms_in = 0;
        }
        break;
    }
    case 3:
        s_show.ms_in += dt;
        if (s_show.ms_in > 5000) {
            s_fw_hold = false;
            s_show.phase = 0;
            s_show.test = 0;
            s_show.fleet = 0;
            s_show.initiator = 0;
            s_show.scene_x0 = -1.0f;
            wk_scroll_target(1.0f);
            if (s_wk.st == WK_SIT) { s_wk.st = WK_PACK; s_wk.timer = 8; }
        }
        break;
    case 4: {
        if (s_show.fleet) {
            int64_t el = whm_wifi_tsf_now() - s_show.start_tsf;
            if (s_show.test && s_show.fast) el *= 10;
            s_show.ms_in = (int32_t)(el / 1000);
        } else {
            s_show.ms_in += dt * (s_show.fast ? 10 : 1);
        }
        int32_t ms = s_show.ms_in;
        if (s_show.year == 0) {             /* NIGHTLY mini-script */
            if (ms >= 2000 && s_wk.st < WK_GOCHAIR) {
                s_fw_hold = true;
                /* THE CHAIR WITH NO ADDRESS (bench, twice): force-
                   entry skipped the state's entry contract - tgt is
                   a REUSED field, and GOCHAIR read a stale ladder-Y
                   as its X, marching him off-screen toward world
                   x~30 deterministically. Stage anchors to the
                   start-derived camera now (TSF-shared in fleet
                   mode, per-unit in test: identical replicas either
                   way). */
                /* THIRD CONVICTION (owner, twice-burned): phase-3
                   RESTORES tgt_scroll=1.0 - a freeze NOTHING in
                   the nightly ever applied (only NYE freezes, at
                   175 s). The 0.44.2 fix anchored chair+bursts to
                   the trigger camera... which then sailed on: he
                   marched to a fixed point drifting out of view,
                   and the bursts fired 1-2 screens behind. The
                   world now holds still for its own show. */
                wk_scroll_target(0.0f);
                if (s_show.scene_x0 < 0.0f)
                    s_show.scene_x0 =
                        wk_cam(whm_wifi_tsf_now()) + 8.0f;
                s_wk.tgt = s_show.scene_x0 + 30.0f;
                s_wk.dir = (s_wk.tgt > s_wk.x) ? 1 : -1;
                s_wk.st = WK_GOCHAIR;
            }
            uint32_t st2 = s_show.seed + (uint32_t)(ms / 1400);
            if (ms >= 8000 && ms < 18000 &&
                ms - s_show.last_launch_ms >
                    (int32_t)(900 + fw_rnd(&st2) % 700)) {
                s_show.last_launch_ms = ms;
                uint8_t r, g, b;
                fw_pal(&st2, &r, &g, &b);
                /* pre-unification relic: 8..56 was the world's
                   ORIGIN - the shows have been firing faithfully
                   behind everyone since the camera first scrolled.
                   Bursts now span the fleet over the stage. */
                fw_launch(s_show.scene_x0 - 8.0f +
                              (float)(fw_rnd(&st2) %
                                  (uint32_t)(64u *
                                      (s_w_n ? s_w_n : 1))),
                          (int)(fw_rnd(&st2) % 3u), r, g, b,
                          (float)(fw_rnd(&st2) % 8u));
            }
            if (s_show.initiator && !s_show.test &&
                t - s_show.last_ann > 5000000LL) {
                s_show.last_ann = t;
                whm_sync_nye_send("", 0, s_show.start_tsf);
            }
            fw_step(dt);
            if (ms > 25000) {
                s_show.phase = 3;
                s_show.ms_in = 0;
            }
            break;
        }
        if (s_show.initiator && !s_show.test &&
            t - s_show.last_ann > 5000000LL) {
            s_show.last_ann = t;
            whm_sync_nye_send(s_show.tz, (uint16_t)s_show.year,
                              s_show.start_tsf);
        }
        /* world speed: normal -> freeze at 23:58 -> ease back 12:12+ */
        wk_scroll_target(ms < 175000 ? 1.0f
                    : ms < 180000 ? (float)(180000 - ms) / 5000.0f
                    : ms < 1020000 ? 0.0f
                    : ms < 1080000 ? (float)(ms - 1020000) / 60000.0f
                    : 1.0f);
        if (ms >= 170000 && s_wk.st < WK_GOCHAIR) {
            s_fw_hold = true;
            s_wk.st = WK_GOCHAIR;
        }
        /* countdown: 23:59:50..:59, ember digits */
        if (ms >= 290000 && ms < 300000) {
            int n = 10 - (ms - 290000) / 1000;
            if (n != (int)s_show.cd_last) {
                s_show.cd_last = n;
                s_fwdn = 0;
                char d[3];
                snprintf(d, sizeof(d), "%d", n);
                fw_text_dots(d, 20, ms, 0);
            }
        }
        if (ms >= 300000 && s_show.cd_last) {
            s_show.cd_last = 0;
            s_fwdn = 0;
        }
        fw_choreo_nye(ms);
        /* friend script: on at 00:10:05, exits stage left by 00:11:50 */
        if (ms >= 905000 && ms < 1010000 && !s_show.friendw.on) {
            s_show.friendw.on = 1;
            s_show.friendw.x = -6.0f;
            s_show.friendw.dir = 1;
        }
        if (s_show.friendw.on) {
            float *fx = &s_show.friendw.x;
            if (ms < 925000) {                 /* walk in to pole */
                if (*fx < 6.0f) *fx += (float)dt * 0.010f;
            } else if (ms < 1000000) {         /* hoist together */
                if (s_show.banner < 1.0f)
                    s_show.banner += (float)dt / 20000.0f;
            } else {                           /* wave, exit left */
                s_show.friendw.dir = -1;
                *fx -= (float)dt * 0.012f;
                if (*fx < -8.0f) s_show.friendw.on = 0;
            }
        }
        if (ms > 1200000) {                    /* 00:15 - lights up */
            s_show.phase = 3;
            s_show.ms_in = 0;
            s_fwdn = 0;
        }
        break;
    }
    }
    fw_step(dt);
}

static void fw_choreo_nye(int32_t ms)
{
    /* t=0 at 23:55:00; midnight 300000; last shell 900000 */
    uint32_t st = s_show.seed + (uint32_t)(ms / 250);
    int32_t gap;
    int act = ms < 300000 ? 0
            : ms < 480000 ? 1 : ms < 780000 ? 2 : ms < 900000 ? 3 : 4;
    if (act == 0) {                            /* buildup: teasers */
        if (ms > 60000 && ms - s_show.last_launch_ms >
                              22000 + (int32_t)(fw_rnd(&st) % 9000u)) {
            s_show.last_launch_ms = ms;
            uint8_t r, g, b;
            fw_pal(&st, &r, &g, &b);
            fw_launch(10.0f + (float)(fw_rnd(&st) % 44u), 0, r, g, b,
                      2.0f);
        }
        return;
    }
    if (act == 1) gap = 1400 - (ms - 300000) / 300;
    else if (act == 2) gap = 800 - (ms - 480000) / 700;
    else if (act == 3) gap = 420 - (ms - 780000) / 600;
    else gap = 99999;
    if (gap < 200) gap = 200;
    if (act <= 3 && ms - s_show.last_launch_ms > gap) {
        s_show.last_launch_ms = ms;
        uint8_t r, g, b;
        fw_pal(&st, &r, &g, &b);
        int n = act == 2 ? 1 + (int)(fw_rnd(&st) % 2u) : 1;
        for (int i = 0; i < n; i++) {
            fw_pal(&st, &r, &g, &b);
            fw_launch(6.0f + (float)(fw_rnd(&st) % 52u),
                      (int)(fw_rnd(&st) % 3u), r, g, b,
                      (float)(fw_rnd(&st) % 10u));
        }
    }
    if (ms >= 300000 && ms < 302000 &&
        ms - s_show.last_launch_ms > 320) {    /* midnight salvo */
        s_show.last_launch_ms = ms;
        uint8_t r, g, b;
        fw_pal(&st, &r, &g, &b);
        fw_launch(8.0f + (float)(fw_rnd(&st) % 48u), 2, r, g, b, 8.0f);
    }
    if (ms >= 312000 && s_fwdn == 0 &&
        s_show.cd_last == 0) {                 /* opening statement */
        fw_text_dots("HAPPY", 4, 312600, 260);
        fw_text_dots("NEW YEAR", 12, 314200, 200);
        char yr[6];
        snprintf(yr, sizeof(yr), "%d", s_show.year);
        fw_text_dots(yr, 22, 317400, 400);
    }
    if (ms > 430000 && s_fwdn && ms < 900000) {
        /* constellation has faded (fw_render_all); free the dots */
        if (ms > 460000) s_fwdn = 0;
    }
    if (act == 3 && ms > 880000 &&
        ms - s_show.last_launch_ms > 300) {    /* finale wall */
        s_show.last_launch_ms = ms;
        uint8_t r, g, b;
        fw_pal(&st, &r, &g, &b);
        fw_launch(6.0f + (float)(fw_rnd(&st) % 52u), 2, r, g, b, 7.0f);
    }
}

static void fw_render_all(int64_t t)
{
    if (s_show.phase == 0) return;
    if (s_show.phase == 4 || s_fwdn) {
        float fade = 1.0f;
        if (s_show.nye || s_show.fleet) {
            if (s_show.ms_in > 400000) {
                fade = 1.0f - (float)(s_show.ms_in - 400000) / 50000.0f;
                if (fade < 0) fade = 0;
            }
        }
        fw_dots_render(s_show.ms_in, t, fade);
    }
    fw_render();
}

static void fw_draw_text(const char *s, int x, int y, uint8_t r,
                         uint8_t g, uint8_t b)
{
    for (; *s; s++, x += 4) {
        const uint8_t *gl = fw_font[fw_glyph(*s)];
        for (int ry = 0; ry < 5; ry++)
            for (int rx = 0; rx < 3; rx++)
                if ((gl[ry] >> (2 - rx)) & 1) wk_px(x + rx, y + ry,
                                                    r, g, b);
    }
}

void whm_ui_nye_join(const char *from, const char *tz, int year,
                     int64_t start_tsf)
{
    if (s_show.phase == 4 && s_show.fleet &&
        s_show.start_tsf == start_tsf) return;       /* dupe */
    bool keep_test = s_show.test && !from[0];
    memset(&s_show, 0, sizeof(s_show));
    s_fwn = 0;
    s_fwdn = 0;
    s_show.phase = 4;
    s_show.fleet = 1;
    s_show.nye = 1;
    s_show.test = keep_test ? 1 : 0;
    s_show.start_tsf = start_tsf;
    s_show.year = year;                 /* year 0 = the NIGHTLY */
    strlcpy(s_show.tz, tz && tz[0] ? tz : "LOCAL", sizeof(s_show.tz));
    s_show.seed = year ? (uint32_t)year * 2654435761u
                       : (uint32_t)(start_tsf / 86400000000LL) * 977u;
    int64_t el = whm_wifi_tsf_now() - start_tsf;
    float cam_t0 = wk_cam(esp_timer_get_time()) -
                   (float)((double)el / 1e6) * WK_CAM_SPD;
    s_show.scene_x0 = cam_t0 + 148.0f;
    s_show.banner = 0;
    if (from[0]) printf("NYE takeover from %s (%s %d) - joining the "
                        "show\n", from, s_show.tz, year);
}

static void wk_nye_scene(float cam, int64_t t)
{
    if (s_show.phase != 4 || s_show.scene_x0 < 0) return;
    int p1 = (int)(s_show.scene_x0 + 34.0f - cam);
    int p2 = (int)(s_show.scene_x0 + 94.0f - cam);
    if (p2 < -4 || p1 > 68) return;
    for (int y = 18; y < WK_GROUND; y++) {           /* poles */
        wk_px(p1, y, 96, 66, 30);
        wk_px(p2, y, 96, 66, 30);
    }
    for (int i = 0; i <= 12; i++) {                  /* string lights */
        float f2 = (float)i / 12.0f;
        int lx = p1 + (int)(f2 * (float)(p2 - p1));
        int ly = 18 + (int)(sinf(f2 * 3.1416f) * 4.0f);
        uint32_t st = s_show.seed + (uint32_t)i * 71u;
        uint8_t r, g, b;
        fw_pal(&st, &r, &g, &b);
        float tw = 0.6f + 0.4f * sinf((float)t / 2.6e5f + (float)i);
        wk_px(lx, ly, (uint8_t)(r * tw), (uint8_t)(g * tw),
              (uint8_t)(b * tw));
    }
    if (s_show.banner > 0.01f) {                     /* the banner */
        int ytop = 18 - (int)(s_show.banner * 12.0f);
        for (int y = ytop; y < ytop + 8; y++)
            for (int x = p1 + 1; x < p2; x++) {
                if (y == ytop + 7 && ((x - p1) & 3) == 3) continue;
                wk_px(x, y, 150, 34, 40);            /* cloth */
            }
        char msg[16];
        snprintf(msg, sizeof(msg), "HAPPY %d", s_show.year);
        int wpx = (int)strlen(msg) * 4 - 1;
        fw_draw_text(msg, p1 + 1 + (p2 - p1 - wpx) / 2, ytop + 1,
                     255, 214, 120);
        wk_px(p1, ytop, 220, 200, 140);              /* rope knots */
        wk_px(p2, ytop, 220, 200, 140);
    }
    if (s_show.friendw.on) {
        int8_t sd = s_wk.dir;
        s_wk.dir = s_show.friendw.dir;
        s_wk_alt_scarf = true;
        wk_sprite((int)s_show.friendw.x, WK_GROUND - 1, t);
        s_wk_alt_scarf = false;
        s_wk.dir = sd;
    }
    fw_draw_text(s_show.tz, 63 - (int)strlen(s_show.tz) * 4, 1,
                 210, 170, 60);                      /* whose midnight */
}

void whm_ui_walk_pure(bool on) { s_wk_pure = on; }

void whm_ui_walk_stats(uint32_t *ok, uint32_t *snap, uint32_t *stale)
{
    *ok = s_wkf_ok;
    *snap = s_wkf_snap;
    *stale = s_wkf_stale;
}

float whm_ui_walk_cam(void) { return s_cam_acc; }


void whm_ui_wkb_rx(uint8_t owner, float x, int8_t y, uint8_t st,
                   int8_t dir, uint16_t timer, uint32_t step,
                   int64_t btsf, float tgt, float vx)
{
    if (s_w_n <= 1 || owner >= s_w_n) return;
    int64_t nowu = esp_timer_get_time();
    s_wko.rx_us = nowu;          /* ANY authentic beacon = owner alive
                                    (liveness before all filtering) */
    /* OWNERSHIP ARBITRATION by step-freshness: a claim is accepted
       only from a NEWER step than the last accepted claim - crossed
       stale beacons can never resurrect a dead owner (the flip-flop
       that turned two correctors loose on each other). */
    if (btsf > s_wko.own_tsf) {
        s_wko.own_tsf = btsf;
        bool was_me = wk_i_own();
        s_wko.owner = owner;
        if (was_me && !wk_i_own()) {
            /* DEMOTION GRACE: keep beaconing the NEW belief so two
               units demoting each other can never form the mutual
               silence that machine-gunned the seizure (33 in a row).
               Silence windows are structurally impossible now. */
            s_wko.grace_until = esp_timer_get_time() + 600000;
        }
        if (!was_me && wk_i_own()) {
            s_wko.burst = 2;
            printf("walker: adopted - I own strip %u now\n", s_w_idx);
        }
    }
    if (wk_i_own()) return;      /* owner never corrects to itself */
    /* EVIDENCE WATERMARK: strictly increasing step - burst
       triplicates and reordered delivery apply exactly once. */
    if (step <= s_wko.ev_step) return;
    s_wko.ev_step = step;
    if (step <= s_wk_steps) { s_wkf_stale++; return; }   /* past */
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    int oldi = 0;
    for (int i = 0; i < WKF_N; i++) {
        if (s_wkf[i].valid && s_wkf[i].step == step) { slot = i; break; }
        if (slot < 0 && !s_wkf[i].valid) slot = i;
        if (s_wkf[i].valid && s_wkf[i].step < oldest) {
            oldest = s_wkf[i].step;
            oldi = i;
        }
    }
    if (slot < 0) slot = oldi;           /* evict the true oldest */
    s_wkf[slot].step = step;
    s_wkf[slot].x = x;
    s_wkf[slot].yq1 = y;
    s_wkf[slot].st = st;
    s_wkf[slot].dir = dir;
    s_wkf[slot].timer = timer;
    s_wkf[slot].tgt = tgt;
    s_wkf[slot].vx = vx;
    s_wkf[slot].valid = 1;
    return;
    /* EQUAL-STEP comparison: find MY pose at the beacon's step and
       judge like against like. Latency cannot false-trigger; a hit
       here is real nondeterminism, corrected as a DELTA (the offset
       persists into the present) and confessed with the step. */
}

void whm_ui_walk_speed(float v)
{
    if (v > 2.0f) v = 2.0f;
    if (v < -2.0f) v = -2.0f;
    wk_scroll_target(v);
}

void whm_ui_walk_speed_get(float *cur, float *tgt)
{
    *cur = s_wk_scroll;
    *tgt = s_wk_tgt_scroll;
}

void whm_ui_fw_sparkle(int secs)
{
    /* render-layer ONLY: bursts ride the LIVE camera over the
       undisturbed scene - no chair, no hold, no scroll change,
       no state force. The isolation instrument that would have
       caught the drifting-camera bug in seconds. */
    s_fw_sparkle_until = esp_timer_get_time() +
                         (int64_t)(secs > 0 ? secs : 10) * 1000000;
    s_fw_sparkle_last = 0;
}

void whm_ui_fw_test(int mode, int fast)   /* hidden: 'fw' console */
{
    if (mode == 2) {
        struct tm lt2;
        int yr = wall_now(&lt2, NULL) ? lt2.tm_year + 1901 : 2027;
        s_show.test = 1;                   /* keep_test sees this */
        whm_ui_nye_join("", "TEST", yr, whm_wifi_tsf_now());
        s_show.fast = (uint8_t)(fast ? 1 : 0);
        return;
    }
    memset(&s_show, 0, sizeof(s_show));
    s_fwn = 0;
    s_fwdn = 0;
    s_show.scene_x0 = -1.0f;
    if (mode == 0) { s_fw_hold = false; s_wk_tgt_scroll = 1.0f; return; }
    /* mode 1: nightly = the same fleet script (year 0), local-only */
    s_show.test = 1;
    whm_ui_nye_join("", "TEST", 0, whm_wifi_tsf_now());
    s_show.test = 1;
}

/* CORRECTED PARALLAX + THE ART PASS: screen = wx - (k*cam + 64*idx).
 * Everything below is RENDER-ONLY - chunk hashes and wall-clock read
 * freely, sim state touched never (doctrines 13-15 make art safe).
 * Sun r11->15 dusk (1.67x), moon r9, rim glimmer on a hash timer;
 * three cloud species with bob + edge shimmer; hash-gated flora;
 * and the SYNTHWAVE HOUR, 3-5am local, 15-min fades. */
/* COMET REGISTRY - real sky, real dates (researched 2026-09-07):
 *   2P/Encke        2026-12-15 .. 2027-02-20  peak 2027-01-25
 *   C/2026 C1 Tsuchinshan 2028-10-01 .. 2028-12-20  peak 2028-11-15
 *   46P/Wirtanen    2029-09-15 .. 2029-12-10  peak 2029-10-27
 *   103P/Hartley 2  2030-03-01 .. 2030-05-15  peak 2030-04-05
 * Dates are astronomy (reliable); brightness is weather (comets
 * fizzle, and the great ones are often found months out) - the
 * registry updates by commit + OTA when the sky changes. */
static const struct { int32_t d0, dp, d1; const char *name; }
    s_comets[] = {
    { 20261215, 20270125, 20270220, "ENCKE" },
    { 20281001, 20281115, 20281220, "TSUCHINSHAN" },
    { 20290915, 20291027, 20291210, "WIRTANEN" },
    { 20300301, 20300405, 20300515, "HARTLEY 2" },
};

static void wk_comet(int64_t t, float cam, int idx, float f,
                     float sw)
{
    if (f >= 0.45f || s_show.phase) return;
    struct tm lt;
    if (!wall_now(&lt, NULL)) return;
    int32_t d = (lt.tm_year + 1900) * 10000 +
                (lt.tm_mon + 1) * 100 + lt.tm_mday;
    int ci = -1;
    for (int i = 0; i < (int)(sizeof(s_comets) /
                              sizeof(s_comets[0])); i++)
        if (d >= s_comets[i].d0 && d <= s_comets[i].d1) { ci = i;
            break; }
    if (ci < 0) return;
    float ramp = d <= s_comets[ci].dp
        ? (float)(d - s_comets[ci].d0) /
          (float)(s_comets[ci].dp - s_comets[ci].d0 + 1)
        : (float)(s_comets[ci].d1 - d) /
          (float)(s_comets[ci].d1 - s_comets[ci].dp + 1);
    float inten = (0.35f + 0.65f * ramp) * (1.0f - f / 0.45f);
    uint32_t nh = wk_h((uint32_t)ci * 40503u ^ 0xC03E7u);
    float base = cam * 0.06f + 64.0f * (float)idx;
    float wx = (float)(140u + nh % 360u) -
               (float)((double)t / 6.0e7);   /* slow proper motion */
    int cx = (((int)(wx - base)) % 560 + 560) % 560 - 30;
    int cy = 5 + (int)((nh >> 9) % 8u);
    if (cx < -18 || cx > 80) return;
    uint8_t kr = 255, kg = 250, kb = 225;
    uint8_t dr = 235, dg = 215, db = 170;   /* dust tail */
    uint8_t ir = 110, ig = 205, ib = 255;   /* ion tail */
    if (sw > 0.3f) { dr = 230; dg = 80; db = 200; }
    int dl = 6 + (int)(inten * 8.0f);       /* dust length */
    for (int s2 = 1; s2 <= dl; s2++) {      /* tail sweeps up-right */
        float fade = inten * (1.0f - (float)s2 / (float)(dl + 2));
        uint32_t sh = wk_h((uint32_t)(t / 400000) * 31u +
                           (uint32_t)s2 * 7u + nh);
        if ((sh & 7) == 0) continue;        /* shimmer gaps */
        int px2 = cx + s2;
        int py2 = cy - s2 / 3;
        wk_px(px2, py2, (uint8_t)(dr * fade), (uint8_t)(dg * fade),
              (uint8_t)(db * fade));
        if (s2 > 3 && s2 < dl - 1)
            wk_px(px2, py2 + 1, (uint8_t)(dr * fade * 0.55f),
                  (uint8_t)(dg * fade * 0.55f),
                  (uint8_t)(db * fade * 0.5f));
    }
    int il = 4 + (int)(inten * 5.0f);       /* ion: thin, straighter */
    for (int s2 = 2; s2 <= il; s2++) {
        float fade = inten * (1.0f - (float)s2 / (float)(il + 1));
        uint32_t sh = wk_h((uint32_t)(t / 180000) * 53u +
                           (uint32_t)s2 * 13u + nh);
        if ((sh & 3) == 0) continue;        /* faster flicker */
        wk_px(cx + s2, cy - 1 - s2 / 5, (uint8_t)(ir * fade),
              (uint8_t)(ig * fade), (uint8_t)(ib * fade));
    }
    wk_px(cx, cy, kr, kg, kb);              /* coma core 2x2 */
    wk_px(cx + 1, cy, kr, kg, kb);
    wk_px(cx, cy + 1, (uint8_t)(kr * 0.9f), (uint8_t)(kg * 0.9f),
          (uint8_t)(kb * 0.85f));
    wk_px(cx + 1, cy + 1, (uint8_t)(kr * 0.9f),
          (uint8_t)(kg * 0.9f), (uint8_t)(kb * 0.85f));
    wk_px(cx - 1, cy, (uint8_t)(150 * inten), (uint8_t)(150 * inten),
          (uint8_t)(140 * inten));          /* halo hint */
    wk_px(cx, cy - 1, (uint8_t)(150 * inten),
          (uint8_t)(150 * inten), (uint8_t)(140 * inten));
    /* NAME NOD: first 12s of every 10th minute, dim gold */
    if ((lt.tm_min % 10) == 0 && lt.tm_sec < 12) {
        const char *nm = s_comets[ci].name;
        int len = 0;
        while (nm[len]) len++;
        int x0 = cx - len * 2;
        if (x0 < 1) x0 = 1;
        if (x0 + len * 4 > 63) x0 = 63 - len * 4;
        uint8_t gr = (uint8_t)(120 * inten),
                gg = (uint8_t)(96 * inten),
                gb = (uint8_t)(30 * inten);
        for (int k2 = 0; k2 < len; k2++) {
            if (nm[k2] == ' ') continue;
            const uint8_t *gl = fw_font[fw_glyph(nm[k2])];
            for (int ry = 0; ry < 5; ry++)
                for (int rx = 0; rx < 3; rx++)
                    if (gl[ry] & (4 >> rx))
                        wk_px(x0 + k2 * 4 + rx, cy + 4 + ry,
                              gr, gg, gb);
        }
    }
}

static float wk_synth(void)
{
    static int64_t ck;
    static float v;
    int64_t now = esp_timer_get_time();
    if (now - ck > 1000000) {
        ck = now;
        struct tm lt;
        v = 0.0f;
        if (wall_now(&lt, NULL)) {
            float hm = (float)lt.tm_hour + (float)lt.tm_min / 60.0f;
            if (hm >= 3.0f && hm < 5.0f) {
                v = 1.0f;
                if (hm < 3.25f) v = (hm - 3.0f) / 0.25f;
                if (hm > 4.75f) v = (5.0f - hm) / 0.25f;
            }
        }
    }
    return v;
}

static void wk_sky(int64_t t, float cam, int idx, float f)
{
    float sw = wk_synth();
    uint8_t sr = (uint8_t)(0 + f * 16.0f);
    uint8_t sg = (uint8_t)(0 + f * 42.0f);
    uint8_t sb = (uint8_t)(2 + f * 100.0f);
    if (sw > 0.0f) {                       /* purple night, pink low */
        for (int y = 0; y < WK_GROUND; y++) {
            float g2 = (float)y / (float)WK_GROUND;
            uint8_t rr = (uint8_t)((1 - sw) * sr +
                                   sw * (26.0f + g2 * 120.0f));
            uint8_t gg = (uint8_t)((1 - sw) * sg + sw * (4.0f +
                                   g2 * 18.0f));
            uint8_t bb = (uint8_t)((1 - sw) * sb +
                                   sw * (48.0f + g2 * 60.0f));
            for (int x = 0; x < W; x++) wk_px(x, y, rr, gg, bb);
        }
    } else {
        whm_display_fill(sr, sg, sb);
    }
    float glow = f * (1.0f - f) * 4.0f;
    if (glow > 1.0f) glow = 1.0f;
    if (glow > 0.05f && sw < 0.5f) {
        for (int y = 46; y < WK_GROUND; y++) {
            float g2 = glow * (float)(y - 45) / 12.0f;
            uint8_t rr = (uint8_t)(sr + g2 * 90.0f);
            uint8_t gg = (uint8_t)(sg + g2 * 24.0f);
            for (int x = 0; x < W; x++) wk_px(x, y, rr, gg, sb / 2);
        }
    }
    float base_slow = cam * 0.04f + 64.0f * (float)idx;
    float base_star = cam * 0.10f + 64.0f * (float)idx;
    float base_cld  = cam * 0.15f + 64.0f * (float)idx;
    if (f < 0.3f) {
        float dim = 1.0f - f / 0.3f;
        for (int i = 0; i < 18; i++) {
            uint32_t h2 = wk_h(0x51A2u + (uint32_t)i * 747u);
            float wx = (float)(h2 % 512u);
            int sx = ((int)(wx - base_star) % 512 + 512) % 512;
            if (sx >= W) continue;
            int sy = 2 + (int)((h2 >> 12) % 26u);
            float tw = 0.7f + 0.3f * sinf((float)t / 3.0e5f +
                                          (float)(h2 & 63));
            uint8_t v = (uint8_t)(dim * tw * 150.0f);
            if (sw > 0.3f) {               /* cyan/magenta stars */
                if (h2 & 1) wk_px(sx, sy, (uint8_t)(v / 3),
                                  (uint8_t)(v * 0.9f), v);
                else wk_px(sx, sy, v, (uint8_t)(v / 4),
                           (uint8_t)(v * 0.8f));
            } else {
                wk_px(sx, sy, v, v, (uint8_t)(v / 2 + 40 * dim));
            }
        }
        int mxi = (((int)(300.0f - base_slow)) % 900 + 900) % 900
                  - 100;
        if (mxi > -12 && mxi < W + 12) {   /* MOON r9, scaled bite */
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
                    wk_px(mxi + dx2, 12 + dy2, mr, mg, mb);
                }
            for (int ai = 0; ai < 28; ai++) {  /* living rim */
                float ga = 6.2832f * (float)ai / 28.0f;
                int gx = (int)(cosf(ga) * 9.0f);
                int gy = (int)(sinf(ga) * 9.0f);
                if ((gx - 6) * (gx - 6) + gy * gy <= 55) continue;
                float run = 0.5f + 0.5f *
                            cosf(ga - (float)t / 1.6e6f);
                float lum = dim * (0.8f + 0.5f * run);
                if (lum > 1.0f) lum = 1.0f;
                wk_px(mxi + gx, 12 + gy, (uint8_t)(150 * lum),
                      (uint8_t)(150 * lum), (uint8_t)(120 * lum));
            }
        }
    }
    if (f > 0.02f || sw > 0.3f) {
        struct tm lt2;
        if (wall_now(&lt2, NULL)) {
            float hm = (float)lt2.tm_hour + (float)lt2.tm_min / 60.0f;
            if (hm >= 6.0f && hm < 20.0f) {
                float p = (hm - 6.0f) / 14.0f;
                float sunw = 30.0f + p * 80.0f;
                /* THE SUN IS NOT A CLOUD (owner): it used to ride
                   full slow-parallax, so world scroll dragged it
                   across the sky like scenery. Parked near
                   infinity now (15% parallax) - it crosses the
                   sky because the DAY passes, not because the
                   walker walks. */
                int sxi = (((int)(sunw -
                           (cam * 0.006f + 64.0f * (float)idx)))
                           % 900 + 900) % 900 - 100;
                /* strip offset stays FULL (seam continuity);
                   only the camera coupling shrinks to 15%. */
                float dusk = hm > 17.0f ? (hm - 17.0f) / 3.0f : 0.0f;
                float dawn = hm < 8.0f ? (8.0f - hm) / 2.0f : 0.0f;
                float lowr = dusk > dawn ? dusk : dawn;
                int sy = 12 + (int)(sinf((p - 0.5f) * 3.1416f) *
                                    sinf((p - 0.5f) * 3.1416f) *
                                    6.0f) + (int)(lowr * 30.0f);
                int rr2 = 11 + (int)(dusk * 4.0f);   /* 1.67x sun */
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
                            dy2 > -2) continue;   /* synth lines */
                        if (q2 > (rr2 - 1) * (rr2 - 1)) {
                            /* LIVING RIM: a soft hot-spot rotates
                               (~8s/rev) over a breathing glow */
                            float ang = atan2f((float)dy2,
                                               (float)dx2);
                            /* TWO-SIDED GLOW (owner): the single
                               sharpened hot-spot becomes two
                               SMOOTH counter-traveling lobes -
                               brighter, wide, running around both
                               sides and crossing like breath. */
                            float g1 = 0.5f + 0.5f *
                                cosf(ang - (float)t / 8.0e6f);
                            float g2 = 0.5f + 0.5f *
                                cosf(ang + (float)t / 8.0e6f);
                            float brth = 0.86f + 0.14f *
                                sinf((float)t / 9.0e6f);
                            float lum = brth * (0.52f +
                                0.42f * g1 + 0.42f * g2);
                            if (lum > 1.30f) lum = 1.30f;
                            wk_px(sxi + dx2, py2,
                                  (uint8_t)(sunr * lum > 255 ? 255
                                   : sunr * lum),
                                  (uint8_t)(sung * lum > 255 ? 255
                                   : sung * lum),
                                  (uint8_t)(sunb * lum));
                        } else {
                            wk_px(sxi + dx2, py2, sunr, sung, sunb);
                        }
                    }
                {   /* THE CROWN v3 (owner's brief): SIXTEEN rays,
                       UNIFORM length (symmetry restored), fixed
                       angular homes - they SWAY +-6deg on their
                       own gentle phases and GLIMMER brighter, but
                       never rotate. And the length is a CLOCK:
                       short punchy crown at noon, long golden
                       rays at dawn and dusk - the sun's character
                       tells the hour, because golden-hour light
                       IS long light. */
                    float Lf = 2.6f + 3.4f * lowr + 0.7f *
                        (0.5f + 0.5f * sinf((float)t / 7.0e6f));
                    int L9 = (int)Lf;
                    if (L9 < 2) L9 = 2;
                    for (int k9 = 0; k9 < 16; k9++) {
                        float sway = 0.10f *
                            sinf((float)t / 4.5e6f +
                                 (float)k9 * 2.4f);
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
                                wk_px(rx2, ry2, (uint8_t)m9,
                                      (uint8_t)n9,
                                      (uint8_t)(tw * sunb > 40 ?
                                       40 : tw * sunb));
                        }
                    }
                }
            }
        }
    }
    wk_comet(t, cam, idx, f, sw);          /* real-sky visitor */
    if (f > 0.35f || sw > 0.3f) {          /* CLOUD SPECIES x3 */
        float cf = f > 0.35f ? (f - 0.35f) / 0.65f : sw * 0.5f;
        for (int i = 0; i < 4; i++) {
            uint32_t h2 = wk_h(0xC10Du + (uint32_t)i * 991u);
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
            /* RUNNING EDGE-LIGHT: a warm 3px spot travels the top
               edge (per-cloud phase); no fixed blinking pixel. */
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
                         wk_px((px2), (py2),                          \
                               (uint8_t)(cr + 26 - 8 * d2),           \
                               (uint8_t)(cg + 25 - 8 * d2),           \
                               (uint8_t)(cb + 22 - 7 * d2));          \
                } while (0)
            #define CLD_EDGE(px2, py2, ex)                            \
                do { int d2 = (ex) - epos;                            \
                     if (d2 < 0) d2 = -d2;                            \
                     if (d2 <= 1)                                     \
                         wk_px((px2), (py2),                          \
                               (uint8_t)(cr + 60 - 20 * d2),          \
                               (uint8_t)(cg + 58 - 20 * d2),          \
                               (uint8_t)(cb + 50 - 18 * d2));         \
                } while (0)
            if (kind == 0) {               /* wisp */
                for (int dx2 = 0; dx2 < 10; dx2++) {
                    wk_px(sx + dx2, sy, cr, cg, cb);
                    CLD_EDGE(sx + dx2, sy - 1, dx2);
                }
            } else if (kind == 1) {        /* double puff */
                for (int dx2 = 0; dx2 < 16; dx2++)
                    wk_px(sx + dx2, sy + 1, cr, cg, cb);
                for (int dx2 = 1; dx2 < 7; dx2++)
                    wk_px(sx + dx2, sy, cr + 20, cg + 20, cb + 20);
                for (int dx2 = 9; dx2 < 15; dx2++)
                    wk_px(sx + dx2, sy, cr + 20, cg + 20, cb + 20);
                for (int dx2 = 1; dx2 < 15; dx2++)
                    CLD_EDGE(sx + dx2, dx2 < 7 || dx2 > 8 ? sy - 1
                                                          : sy,
                             dx2 - 1);
                for (int dx2 = 2; dx2 < 14; dx2++) {
                    wk_px(sx + dx2, sy + 2, cr - 22, cg - 22,
                          cb - 18);
                    CLD_BOT(sx + dx2, sy + 2, dx2 - 2);
                }
            } else {                       /* big cumulus */
                for (int dx2 = 0; dx2 < 24; dx2++)
                    wk_px(sx + dx2, sy + 2, cr, cg, cb);
                for (int dx2 = 2; dx2 < 9; dx2++)
                    wk_px(sx + dx2, sy + 1, cr + 18, cg + 18,
                          cb + 18);
                for (int dx2 = 8; dx2 < 17; dx2++) {
                    wk_px(sx + dx2, sy, cr + 26, cg + 26, cb + 26);
                    wk_px(sx + dx2, sy + 1, cr + 18, cg + 18,
                          cb + 18);
                }
                for (int dx2 = 16; dx2 < 22; dx2++)
                    wk_px(sx + dx2, sy + 1, cr + 18, cg + 18,
                          cb + 18);
                for (int dx2 = 1; dx2 < 23; dx2++) {
                    int ey = dx2 >= 8 && dx2 <= 16 ? sy - 1 :
                             (dx2 >= 2 && dx2 <= 21 ? sy : sy + 1);
                    CLD_EDGE(sx + dx2, ey, dx2 - 1);
                }
                for (int dx2 = 1; dx2 < 23; dx2++) {
                    wk_px(sx + dx2, sy + 3, cr - 26, cg - 26,
                          cb - 20);
                    CLD_BOT(sx + dx2, sy + 3, dx2 - 1);
                }
            }
            #undef CLD_BOT
            #undef CLD_EDGE
        }
    }
}

/* FAUNA: seagulls drift the daytime sky; synthwave pigeons haunt
 * the 3-5am hour. Two-frame wing glyphs, star-parallax (k=0.10),
 * hash-spawned in 15s epochs - render-only. */
static void wk_birds(int64_t t, float cam, int idx, float f,
                     float sw)
{
    bool day = f > 0.5f && sw < 0.3f;
    bool syn = sw > 0.3f;
    if (!day && !syn) return;
    float base = cam * 0.10f + 64.0f * (float)idx;
    uint32_t ep = (uint32_t)(t / 15000000LL);
    for (int i = 0; i < 2; i++) {
        uint32_t h = wk_h(ep * 2654435761u ^ (uint32_t)i * 977u);
        uint32_t gate = h % 100u;
        if (day && gate >= 30u) continue;
        if (syn && (gate >= 18u || i > 0)) continue;
        float spd = syn ? -2.2f : -4.5f;
        float wx = (float)(h % 640u) +
                   (float)((double)(t % 15000000LL) / 1e6) * spd;
        int sx = (((int)(wx - base)) % 640 + 640) % 640 - 20;
        if (sx < -6 || sx > 70) continue;
        int sy = 7 + (int)((h >> 10) % 14u);
        int flap = (int)(t / (syn ? 620000 : 380000) +
                         (int64_t)(h & 7)) & 1;
        if (day) {
            uint8_t v = 205, g2 = 205, b2 = 210;
            /* V-BEAT (owner's note: three peaks = noise, one
               vertex = bird). Shallow V glide, deep V flap with
               wingtips swept up-and-out - the child's-drawing gull. */
            if (flap) {                    /* deep V: tips up a row */
                wk_px(sx, sy - 1, v, g2, b2);
                wk_px(sx + 1, sy, v, g2, b2);
                wk_px(sx + 2, sy + 1, 160, 160, 170);
                wk_px(sx + 3, sy, v, g2, b2);
                wk_px(sx + 4, sy - 1, v, g2, b2);
            } else {                       /* shallow V glide */
                wk_px(sx, sy, v, g2, b2);
                wk_px(sx + 1, sy, v, g2, b2);
                wk_px(sx + 2, sy + 1, 160, 160, 170);
                wk_px(sx + 3, sy, v, g2, b2);
                wk_px(sx + 4, sy, v, g2, b2);
            }
        } else {                           /* synthwave pigeon */
            uint8_t pr = 200, pg = 60, pb = 170;
            wk_px(sx + 1, sy + 1, 30, 8, 44);
            wk_px(sx + 2, sy + 1, 30, 8, 44);
            wk_px(sx + 3, sy + 1, 30, 8, 44);
            wk_px(sx + 4, sy, 40, 12, 56);     /* head */
            wk_px(sx + 1, sy + 2, pr, pg, pb); /* underlight */
            wk_px(sx + 2, sy + 2, pr, pg, pb);
            if (flap) {                    /* V over the body */
                wk_px(sx + 1, sy - 1, 60, 200, 220);
                wk_px(sx + 3, sy - 1, 60, 200, 220);
            } else {
                wk_px(sx, sy + 1, 60, 200, 220);
            }
        }
    }
}

/* FLORA v2: the walker walks THROUGH trees now. Large oak/pine
 * (24-40px) on ~30%% of chunks, mediums on ~25%%, the old tiny kinds
 * demoted to dimmed distant accents. Chunk window widened c0-1..c0+2
 * so nothing pops at the right edge; drawn BEFORE platforms so
 * canopies tuck behind structures. Render-only, as ever. */
static void wk_tree_oak(int sx, int gy, float li, float sw,
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
    int th = 10 + (int)(h % 4u);            /* trunk 10-13 */
    for (int y2 = 0; y2 < th; y2++) {
        wk_px(sx, gy - y2, tr, tg, tb);
        wk_px(sx + 1, gy - y2, tr, tg, tb);
        if (y2 < 3) wk_px(sx + 2, gy - y2,
                          (uint8_t)(tr * 0.8f),
                          (uint8_t)(tg * 0.8f),
                          (uint8_t)(tb * 0.8f));
    }
    int cw = 15 + (int)((h >> 4) % 3u);     /* canopy 15-17 wide */
    int ch = 15 + (int)((h >> 6) % 5u);     /* 15-19 tall */
    int cy0 = gy - th - ch + 3;
    for (int y2 = 0; y2 < ch; y2++) {
        float yr = (float)y2 / (float)(ch - 1);
        float half = (float)cw * 0.5f *
                     sinf(yr * 3.1416f);
        int hw = (int)(half + 0.5f);
        uint32_t jr = wk_h(h ^ (uint32_t)y2 * 131u);
        hw += (int)(jr % 3u) - 1;           /* ragged edge */
        if (hw < 1) hw = 1;
        for (int dx2 = -hw; dx2 <= hw; dx2++) {
            int py2 = cy0 + y2;
            uint8_t rr = l1r, gg = l1g, bb = l1b;
            if (y2 < ch / 3 && dx2 < 0) { rr = l2r; gg = l2g;
                bb = l2b; }
            else if (y2 > (2 * ch) / 3) { rr = l0r; gg = l0g;
                bb = l0b; }
            if (((jr >> (dx2 & 15)) & 7u) == 0) { rr = l0r;
                gg = l0g; bb = l0b; }       /* inner speck */
            wk_px(sx + 1 + dx2, py2, rr, gg, bb);
        }
    }
}

static void wk_tree_pine(int sx, int gy, float li, float sw,
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
        wk_px(sx, gy - y2, tr, tg, tb);
        wk_px(sx + 1, gy - y2, tr, tg, tb);
    }
    int base = gy - 3;
    for (int ti = 0; ti < tiers; ti++) {
        int ty0 = base - ti * (tierh - 2);
        int maxw = 3 + (tiers - ti) * 2;
        for (int y2 = 0; y2 < tierh; y2++) {
            int hw = maxw - (y2 * maxw) / tierh;
            if (hw < 1) hw = 1;
            for (int dx2 = -hw; dx2 <= hw; dx2++)
                wk_px(sx + dx2, ty0 - y2,
                      (ti + y2) & 1 ? d1r : d0r,
                      (ti + y2) & 1 ? d1g : d0g,
                      (ti + y2) & 1 ? d1b : d0b);
        }
    }
    (void)total;
}

static void wk_flora(float cam, int idx, float f, int64_t t)
{
    (void)t;
    float sw = wk_synth();
    float li = 0.35f + 0.65f * f;
    /* TREE PARALLAX k=0.7, same k*cam+64*idx form as every sky
       layer (seam-continuous), lroundf like the platforms - one
       shared beat, no more off-phase chunk-origin stepping. */
    float base = 0.7f * cam + 64.0f * (float)idx;
    int32_t c0 = (int32_t)floorf(base / 64.0f);
    int32_t cw0 = (int32_t)floorf((cam + 64.0f * (float)idx)
                                  / 64.0f);
    for (int cj = cw0 - 1; cj <= cw0 + 2; cj++)
        if (wk_nye_window(cj)) return;   /* keep the plaza clean */
    for (int ci = c0 - 1; ci <= c0 + 2; ci++) {
        uint32_t h = wk_h((uint32_t)ci * 2246822519u ^ 0xF10FAu);
        uint32_t roll = h % 100u;
        int sxbase = (int)lroundf((float)(ci * 64) - base);
        int gy = WK_GROUND - 1;
        if (roll < 30u) {                   /* LARGE tree */
            int wx = 10 + (int)((h >> 8) % 40u);
            int sx = sxbase + wx;
            if (sx > -20 && sx < 84) {
                if ((h >> 16) & 1)
                    wk_tree_oak(sx, gy, li, sw, h);
                else
                    wk_tree_pine(sx, gy, li, sw, h);
            }
        } else if (roll < 55u) {            /* MEDIUM tree */
            int wx = 8 + (int)((h >> 8) % 46u);
            int sx = sxbase + wx;
            if (sx > -14 && sx < 78) {
                uint32_t h2 = wk_h(h ^ 0xBEEFu);
                if ((h >> 16) & 1) {
                    for (int y2 = 0; y2 < 6; y2++)
                        wk_px(sx, gy - y2, (uint8_t)(90 * li),
                              (uint8_t)(60 * li),
                              (uint8_t)(28 * li));
                    for (int y2 = 0; y2 < 10; y2++) {
                        float yr = (float)y2 / 9.0f;
                        int hw = (int)(4.5f * sinf(yr * 3.1416f)
                                       + 0.5f);
                        if (hw < 1) hw = 1;
                        for (int dx2 = -hw; dx2 <= hw; dx2++)
                            wk_px(sx + dx2, gy - 5 - y2,
                                  (uint8_t)((30 + ((h2 >> (dx2 &
                                   7)) & 1) * 26) * li),
                                  (uint8_t)((116 + ((h2 >> (dx2 &
                                   7)) & 1) * 30) * li),
                                  (uint8_t)(36 * li));
                    }
                } else {
                    wk_px(sx, gy, (uint8_t)(88 * li),
                          (uint8_t)(58 * li), (uint8_t)(26 * li));
                    for (int ti = 0; ti < 3; ti++)
                        for (int y2 = 0; y2 < 6; y2++) {
                            int hw = (3 + (3 - ti) * 2) * (6 - y2)
                                     / 6;
                            if (hw < 1) hw = 1;
                            for (int dx2 = -hw; dx2 <= hw; dx2++)
                                wk_px(sx + dx2,
                                      gy - 1 - ti * 4 - y2,
                                      (uint8_t)(16 * li),
                                      (uint8_t)((84 + (ti & 1) *
                                                 24) * li),
                                      (uint8_t)(40 * li));
                        }
                }
            }
        } else if (roll < 78u) {            /* distant accents */
            int npc = 1 + (int)((h >> 8) & 1u);
            for (int k = 0; k < npc; k++) {
                uint32_t hk = wk_h(h ^ (uint32_t)(k + 1) * 40503u);
                int sx = sxbase + 6 + (int)(hk % 52u);
                if (sx < -8 || sx > 72) continue;
                int kind = (int)((hk >> 16) % 4u);
                float dl = li * 0.55f;      /* dimmed = distance */
                if (kind == 0) {            /* far pine */
                    for (int y2 = 0; y2 < 5; y2++) {
                        int hw = (5 - y2) / 2;
                        for (int dx2 = -hw; dx2 <= hw; dx2++)
                            wk_px(sx + dx2, gy - y2,
                                  (uint8_t)(18 * dl),
                                  (uint8_t)(88 * dl),
                                  (uint8_t)(36 * dl));
                    }
                } else if (kind == 1) {     /* bush */
                    for (int dx2 = -2; dx2 <= 2; dx2++)
                        wk_px(sx + dx2, gy, (uint8_t)(28 * li),
                              (uint8_t)(112 * li),
                              (uint8_t)(34 * li));
                    for (int dx2 = -1; dx2 <= 1; dx2++)
                        wk_px(sx + dx2, gy - 1,
                              (uint8_t)(30 * li),
                              (uint8_t)(130 * li),
                              (uint8_t)(36 * li));
                } else if (kind == 2) {     /* rocks */
                    wk_px(sx, gy, (uint8_t)(92 * li),
                          (uint8_t)(92 * li), (uint8_t)(98 * li));
                    wk_px(sx + 1, gy, (uint8_t)(74 * li),
                          (uint8_t)(74 * li), (uint8_t)(80 * li));
                    wk_px(sx, gy - 1, (uint8_t)(110 * li),
                          (uint8_t)(110 * li),
                          (uint8_t)(116 * li));
                } else {                    /* flowers */
                    for (int fx2 = 0; fx2 < 3; fx2++) {
                        uint32_t fh = wk_h(hk ^ (uint32_t)fx2 *
                                           77u);
                        uint8_t fr = (uint8_t)(180 * li),
                                fg2 = (uint8_t)(60 * li),
                                fb = (uint8_t)(120 * li);
                        if (fh & 1) { fr = (uint8_t)(220 * li);
                            fg2 = (uint8_t)(190 * li);
                            fb = (uint8_t)(40 * li); }
                        wk_px(sx + (int)(fh % 5u) - 2, gy, fr,
                              fg2, fb);
                    }
                }
            }
        }
    }
}

static void wk_sprite(int lx, int y, int64_t t)
{
    bool a = (s_wk.phase >> 2) & 1;
    uint8_t br = 213, bg = 194, bb = 167;
    uint8_t sr, sg, sb;
    hsv_rgb((uint16_t)((t / 90000) % 360), 230, 255, &sr, &sg, &sb);
    int squat = (s_wk.st == WK_CROUCH || s_wk.st == WK_LAND) ? 2
              : (s_wk.st == WK_SIT || s_wk.st == WK_SETUP ||
                 s_wk.st == WK_PACK) ? 3 : 0;
    for (int dy2 = -11 + squat; dy2 <= -9 + squat; dy2++)
        for (int dx2 = -1; dx2 <= 1; dx2++)
            wk_px(lx + dx2, y + dy2, br, bg, bb);
    int scarf_stream = (s_wk.st == WK_JUMP) ? 2 : 0;
    if (s_wk_alt_scarf) { sr = 22; sg = 148; sb = 138; }
    wk_px(lx, y - 8 + squat, sr, sg, sb);
    wk_px(lx - s_wk.dir, y - 8 + squat, sr, sg, sb);
    wk_px(lx - (2 + scarf_stream) * s_wk.dir,
          y - 8 + squat + (a ? 1 : 0), sr, sg, sb);
    if (scarf_stream)
        wk_px(lx - 4 * s_wk.dir, y - 7, sr, sg, sb);
    for (int dy2 = -7 + squat; dy2 <= -3; dy2++)
        wk_px(lx, y + dy2, br, bg, bb);
    if (s_wk.st == WK_CROUCH) {            /* wind-up: power sparkles */
        wk_px(lx - 1, y - 2, br, bg, bb);
        wk_px(lx + 1, y - 2, br, bg, bb);
        wk_px(lx - 1, y - 1, br, bg, bb);
        wk_px(lx + 1, y - 1, br, bg, bb);
        int ph = s_wk.phase & 7;
        uint8_t pr, pg, pb;
        hsv_rgb((uint16_t)((t / 20000) % 360), 255, 255, &pr, &pg, &pb);
        wk_px(lx, y - 12 - (ph & 3), pr, pg, pb);
        wk_px(lx + ((ph & 1) ? 1 : -1), y - 13 - (ph >> 2), pr, pg, pb);
    } else if (s_wk.st == WK_JUMP) {       /* legs tucked, flying */
        wk_px(lx - s_wk.dir, y - 2, br, bg, bb);
        wk_px(lx, y - 1, br, bg, bb);
        wk_px(lx - 1, y - 9, br, bg, bb);  /* arms back */
        wk_px(lx + 1, y - 9, br, bg, bb);
    } else if (s_wk.st == WK_LAND) {       /* squash */
        wk_px(lx - 2, y - 1, br, bg, bb);
        wk_px(lx + 2, y - 1, br, bg, bb);
        wk_px(lx - 1, y - 1, br, bg, bb);
        wk_px(lx + 1, y - 1, br, bg, bb);
    } else if (s_wk.st == WK_SIT || s_wk.st == WK_SETUP ||
               s_wk.st == WK_PACK) {
        /* seated: legs forward, hands in lap, watching the world */
        wk_px(lx - 1, y - 4, br, bg, bb);          /* lap hands */
        wk_px(lx + s_wk.dir, y - 2, br, bg, bb);   /* legs fwd */
        wk_px(lx + 2 * s_wk.dir, y - 1, br, bg, bb);
    } else if (s_wk.st == WK_POP) {
        uint8_t pr, pg, pb;                        /* arrival sparkle */
        hsv_rgb((uint16_t)((t / 15000) % 360), 255, 255, &pr, &pg, &pb);
        wk_px(lx - 3, y - 6, pr, pg, pb);
        wk_px(lx + 3, y - 8, pr, pg, pb);
        wk_px(lx - 2, y - 11, pr, pg, pb);
        wk_px(lx + 2, y - 3, pr, pg, pb);
        wk_px(lx - 1, y - 1, br, bg, bb);
        wk_px(lx + 1, y - 1, br, bg, bb);
    } else if (s_wk.st == WK_LADDER || s_wk.st == WK_SHIMMY) {
        wk_px(lx - 1, y + (a ? -8 : -6), br, bg, bb);
        wk_px(lx + 1, y + (a ? -6 : -8), br, bg, bb);
        wk_px(lx - 1, y - (a ? 1 : 2), br, bg, bb);
        wk_px(lx + 1, y - (a ? 2 : 1), br, bg, bb);
    } else if (s_wk.st == WK_SLIDE) {
        wk_px(lx - 1, y - 9, br, bg, bb);
        wk_px(lx + 1, y - 9, br, bg, bb);
        wk_px(lx - s_wk.sdir * 3, y - 5, 48, 66, 89);
        wk_px(lx - s_wk.sdir * 5, y - 4, 26, 36, 58);
        wk_px(lx - 1, y - 1, br, bg, bb);
        wk_px(lx + 1, y - 1, br, bg, bb);
    } else if (s_wk.st == WK_FALL) {
        wk_px(lx - 1, y - 8, br, bg, bb);
        wk_px(lx + 1, y - 8, br, bg, bb);
        wk_px(lx - 1, y - 1, br, bg, bb);
        wk_px(lx + 1, y - 1, br, bg, bb);
    } else if (s_wk.st == WK_IDLE) {
        wk_px(lx - 1, y - 5, br, bg, bb);
        wk_px(lx + 1, y - 5, br, bg, bb);
        wk_px(lx - 1, y - 1, br, bg, bb);
        wk_px(lx + 1, y - 1, br, bg, bb);
    } else {
        int8_t d = s_wk.dir;
        if (a) {
            wk_px(lx + d, y - 2, br, bg, bb);
            wk_px(lx + 2 * d, y - 1, br, bg, bb);
            wk_px(lx - d, y - 2, br, bg, bb);
            wk_px(lx - 2 * d, y - 1, br, bg, bb);
            wk_px(lx - d, y - 6, br, bg, bb);
            wk_px(lx + d, y - 5, br, bg, bb);
        } else {
            wk_px(lx, y - 2, br, bg, bb);
            wk_px(lx, y - 1, br, bg, bb);
            wk_px(lx + d, y - 2, br, bg, bb);
            wk_px(lx + d, y - 6, br, bg, bb);
            wk_px(lx - d, y - 5, br, bg, bb);
        }
    }
}

static void pat_walker(int64_t t)
{
    wk_cam_tick(t);
    wander_load();
    uint8_t n = s_w_n;
    int64_t anchor = t / WK_ANCHOR_US;
    if (anchor != s_wk_anchor) {
        if (s_wk_anchor > 0 &&
            (anchor - s_wk_anchor > 1000000 ||
             s_wk_anchor - anchor > 1000000))
            printf("walker: RE-ANCHOR delta %+.1fs (epoch moved - "
                   "world will rebase)\n",
                   (double)(anchor - s_wk_anchor) / 1e6);
        s_wk_anchor = anchor;
        wk_respawn(anchor * WK_ANCHOR_US, n);
        /* ANCHOR ROLLOVER HYGIENE: step numbers restart each window,
           so old-window watermarks would reject every new beacon
           (referee deaf, ownership frozen ~10 min) and stale ring
           entries would false-compare on colliding small steps.
           Clear all step-keyed state at the shared boundary - every
           unit does this at the same anchor, symmetrically. */
        s_wko.ev_step = 0;
        s_wko.own_tsf = 0;
        memset(s_wkf, 0, sizeof(s_wkf));
        s_wk_last_chunk = INT32_MIN;
        memset(s_wkh, 0, sizeof(s_wkh));
        s_wkh_w = 0;
    }
    uint32_t want = (uint32_t)((t - s_wk_anchor * WK_ANCHOR_US)
                               / WK_TICK_US);
    if (s_wko.resync) {          /* ownership just arrived: the
                                     adopted pose IS current - pay no
                                     step debt from this window */
        s_wko.resync = 0;
        s_wk_steps = want;   /* closed-form cam needs no escort */
    }
    while (s_wk_steps < want) {
        int64_t ts = s_wk_anchor * WK_ANCHOR_US +
                     (int64_t)(s_wk_steps + 1) * WK_TICK_US;
        if (s_wk.st == WK_SHIMMY) {        /* cling to the left bezel */
            s_wk.x = wk_cam(ts) + 2.0f;
            s_wk.tgt = wk_cam(ts) + (float)n * 64.0f;  /* right edge */
        }
        wk_step(ts, n);
        wk_cam_sync();                 /* exact to this step */
        if (s_w_n > 1 && !wk_i_own() && s_show.phase == 0) {
            for (int i = 0; i < WKF_N; i++) {
                if (s_wkf[i].valid && s_wkf[i].step < s_wk_steps)
                    s_wkf[i].valid = 0;      /* expired unverified */
                if (!s_wkf[i].valid || s_wkf[i].step != s_wk_steps)
                    continue;
                s_wkf[i].valid = 0;
                float dx = s_wkf[i].x - s_wk.x;
                int dyv = (int)s_wkf[i].yq1 -
                          (int)lroundf(s_wk.y * 2.0f);
                float dtg = s_wkf[i].tgt - s_wk.tgt;
                float dvx = s_wkf[i].vx - s_wk.vx;
                /* ADOPT ALWAYS (owner's clones): the old gate
                   adopted only past-threshold, so between
                   adoptions each follower's sim wandered its own
                   path and both panels drew their own walker -
                   two men at the bezel. The owner's keyframe is
                   now the follower's pose unconditionally; the
                   mismatch test below is pure DIAGNOSTICS. Forks
                   live at most one step (~0.3 px). */
                s_wk.x = s_wkf[i].x;
                s_wk.y = (float)s_wkf[i].yq1 * 0.5f;
                s_wk.st = s_wkf[i].st;
                s_wk.dir = s_wkf[i].dir;
                s_wk.timer = s_wkf[i].timer;
                s_wk.tgt = s_wkf[i].tgt;
                s_wk.vx = s_wkf[i].vx;
                if (dx > 0.75f || dx < -0.75f || dyv > 1 ||
                    dyv < -1 || s_wkf[i].st != (uint8_t)s_wk.st ||
                    s_wkf[i].dir != (int8_t)s_wk.dir ||
                    s_wkf[i].timer != (uint16_t)s_wk.timer ||
                    dtg > 0.5f || dtg < -0.5f ||
                    dvx > 0.05f || dvx < -0.05f) {
                    printf("walker: at-step SNAP @%lu (dx %.2f, "
                           "%u vs %u, ptgt=%.1f cam=%.2f) "
                           "mine{x=%.2f "
                           "y=%.2f tgt=%.1f "
                           "vx=%.2f dir=%d tm=%u}\n",
                           (unsigned long)s_wk_steps, (double)dx,
                           (unsigned)s_wkf[i].st,
                           (unsigned)s_wk.st,
                           (double)s_wkf[i].tgt, s_cam_acc,
                           (double)s_wk.x,
                           (double)s_wk.y, (double)s_wk.tgt,
                           (double)s_wk.vx, (int)s_wk.dir,
                           (unsigned)s_wk.timer);
                    for (int h2 = 0; h2 < 8; h2++) {
                        int q2 = (s_wkh_w + h2) % 8;
                        if (!s_wkh[q2].step) continue;
                        printf("  st %u->%u @%lu\n",
                               (unsigned)s_wkh[q2].from,
                               (unsigned)s_wkh[q2].to,
                               (unsigned long)s_wkh[q2].step);
                    }
                    s_wk.x = s_wkf[i].x;      /* complete state,   */
                    s_wk.y = (float)s_wkf[i].yq1 * 0.5f;
                    s_wk.st = s_wkf[i].st;    /* exactly at the    */
                    s_wk.dir = s_wkf[i].dir;  /* step it describes */
                    s_wk.timer = s_wkf[i].timer;
                    s_wk.tgt = s_wkf[i].tgt;    /* COMPLETE snap */
                    s_wk.vx = s_wkf[i].vx;
                    s_wkf_snap++;
                    int64_t nu = esp_timer_get_time();
                    if (nu - s_wko.storm_t0 > 3000000) {
                        s_wko.storm_t0 = nu;
                        s_wko.storms = 0;
                    }
                    if (++s_wko.storms > 5) {
                        s_wko.storms = 0;
                        printf("walker: anchor INVALIDATED (site %d)\n", __LINE__);
        s_wk_anchor = INT64_MIN;
                        printf("walker: snap storm - replay-resync"
                               "\n");
                    }
                } else {
                    s_wkf_ok++;
                }
                break;
                /* P1 hook: on user input, invalidate ALL s_wkf and
                   the owner recomputes short-lead references. */
            }
        }
        if (s_w_n > 1 && wk_i_own()) {
            int ns = (int)floorf((s_wk.x - wk_cam(ts)) / 64.0f);
            float frh = (s_wk.x - wk_cam(ts)) - (float)ns * 64.0f;
            /* DEADBAND (owner's flap): for n=2 the leash center
               IS the bezel - he lives at the seam, ownership
               flipped at walking frequency, every flip opened a
               keyframe gap that tripped "silent". Hand off only
               once he is genuinely INSIDE the new strip. */
            if (ns >= 0 && ns < (int)s_w_n && ns != (int)s_w_idx
                && frh >= 4.0f && frh <= 60.0f) {
                s_wko.owner = (uint8_t)ns;      /* HANDOFF */
                s_wko.own_tsf = whm_wifi_tsf_now();
                s_wko.burst = 2;
                s_wko.grace_until = ts + 600000;
                printf("walker: handoff -> strip %d\n", ns);
            }
        }
        s_wk_steps++;
    }

    float cam = wk_cam(t);
    int ox = (int)lroundf(cam) + (int)s_w_idx * 64;
    float f = wk_daylight();
    wk_sky(t, cam, (int)s_w_idx, f);
    wk_flora(cam, (int)s_w_idx, f, t);   /* behind platforms */
    wk_birds(t, cam, (int)s_w_idx, f, wk_synth());
    {   /* fw sparkle: background-only fireworks test */
        int64_t nw = esp_timer_get_time();
        static int64_t sp_t;
        int spdt = sp_t ? (int)((nw - sp_t) / 1000) : 33;
        sp_t = nw;
        if (nw < s_fw_sparkle_until) {
            if (nw - s_fw_sparkle_last > 750000) {
                s_fw_sparkle_last = nw;
                uint32_t sr = (uint32_t)nw;
                uint8_t r2, g2, b2;
                fw_pal(&sr, &r2, &g2, &b2);
                fw_launch(cam + 4.0f +
                              (float)(fw_rnd(&sr) %
                                  (uint32_t)(64u * s_w_n - 8u)),
                          (int)(fw_rnd(&sr) % 3u), r2, g2, b2,
                          (float)(fw_rnd(&sr) % 8u));
            }
            if (s_show.phase != 4) fw_step(spdt);
        } else if (s_fwn && s_show.phase != 4) {
            fw_step(spdt);           /* let strays finish */
        }
    }

    /* ground - carved by gaps, planked by bridges */
    int32_t id0 = (int32_t)floorf((float)ox / 64.0f);
    for (int x = 0; x < W; x++) {
        int32_t wx = ox + x;
        bool gap = false, bridge = false;
        for (int d = 0; d <= 2; d++) {
            const wchunk_t *c = wk_chunk(id0 + d);
            if (c->gap_w && wx >= c->gap_x &&
                wx < c->gap_x + c->gap_w) {
                gap = true;
                bridge = c->bridged;
            }
        }
        if (!gap) {
            wk_px(x, 58, 11, 16, 11);
            wk_px(x, 59, 5, 8, 5);
            wk_px(x, 60, 2, 3, 3);
        } else if (bridge) {
            wk_px(x, 58, 79, 42, 11);      /* planks */
            wk_px(x, 59, 40, 22, 6);
            if ((wx & 3) == 0) wk_px(x, 57, 96, 52, 14);  /* rail */
        } else {
            wk_px(x, 60, 1, 1, 2);          /* the pit yawns */
            wk_px(x, 61, 1, 1, 2);
        }
    }
    for (int d = 0; d <= 2; d++) {
        const wchunk_t *c = wk_chunk(id0 + d);
        if (c->house_x >= 0) {              /* a house, windows warm */
            int hx = (int)(c->house_x - ox);
            for (int yy = 46; yy <= 57; yy++)
                for (int xx = 0; xx < 14; xx++)
                    wk_px(hx + xx, yy, 34, 22, 12);
            for (int rr = 0; rr < 5; rr++)   /* roof */
                for (int xx = rr; xx < 14 - rr; xx++)
                    wk_px(hx + xx, 45 - rr + 4, 70, 16, 8);
            for (int yy = 52; yy <= 57; yy++) {
                wk_px(hx + 10, yy, 8, 5, 3);
                wk_px(hx + 11, yy, 8, 5, 3);
            }
            float wl = 1.0f - f;             /* window glows at night */
            uint8_t wr = (uint8_t)(6 + wl * 150.0f);
            uint8_t wg = (uint8_t)(6 + wl * 95.0f);
            for (int yy = 49; yy <= 51; yy++)
                for (int xx = 3; xx <= 5; xx++)
                    wk_px(hx + xx, yy, wr, wg, 8);
        }
        for (int k = 0; k < c->np; k++) {
            uint8_t py = c->p[k].y;
            for (int32_t px = c->p[k].x;
                 px < c->p[k].x + c->p[k].w; px++) {
                int lx = (int)(px - ox);
                int32_t rel = px - c->p[k].x;
                bool rim = rel == 0 || rel == c->p[k].w - 1;
                if (py <= 14) {              /* walkable cloud puff */
                    wk_px(lx, py, rim ? 120 : 185, rim ? 122 : 187,
                          rim ? 132 : 198);
                    wk_px(lx, py + 1, 95, 98, 112);
                    if (!rim && (px & 3) == 1)
                        wk_px(lx, py - 1, 150, 152, 165);
                } else if (py <= 19) {       /* treetop canopy */
                    wk_px(lx, py, 34, 150, 44);
                    wk_px(lx, py + 1, 18, 92, 26);
                    wk_px(lx, py + 2, 8, 44, 14);
                    if (rel == c->p[k].w / 2) {  /* trunk hint */
                        wk_px(lx, py + 3, 52, 28, 8);
                        wk_px(lx, py + 4, 52, 28, 8);
                    }
                } else {                     /* mossy slab */
                    wk_px(lx, py, 29, 133, 36);
                    wk_px(lx, py + 1, 17, 20, 35);
                    wk_px(lx, py + 2, 10, 12, 22);
                    wk_px(lx, py + 3, 5, 6, 12);
                }
            }
        }
        for (int k = 0; k < c->nl; k++) {
            int lx = (int)(c->l[k].x - ox);
            for (int y = c->l[k].ytop; y <= c->l[k].ybot; y++) {
                wk_px(lx - 1, y, 79, 42, 11);
                wk_px(lx + 1, y, 79, 42, 11);
                if (((y - c->l[k].ytop) & 3) == 1)
                    wk_px(lx, y, 143, 89, 30);
            }
        }
        for (int k = 0; k < c->ns; k++) {
            int steps = c->s[k].ybot - c->s[k].ytop;
            for (int q = 0; q <= steps; q++) {
                int lx = (int)(c->s[k].x + c->s[k].dir * q - ox);
                wk_px(lx, c->s[k].ytop + q, 98, 126, 158);
                wk_px(lx + c->s[k].dir, c->s[k].ytop + q, 48, 66, 89);
            }
        }
    }
    int wlx = (int)lroundf(s_wk.x) - ox;
    if (s_wk.st == WK_SETUP || s_wk.st == WK_SIT ||
        s_wk.st == WK_PACK) {
        int chx = wlx - s_wk.dir;          /* the little red chair */
        wk_px(chx - 1, (int)s_wk.y - 3, 90, 20, 12);
        wk_px(chx, (int)s_wk.y - 3, 90, 20, 12);
        wk_px(chx + 1, (int)s_wk.y - 3, 90, 20, 12);
        wk_px(chx - 1, (int)s_wk.y - 5, 90, 20, 12);   /* backrest */
        wk_px(chx - 1, (int)s_wk.y - 4, 90, 20, 12);
        wk_px(chx - 1, (int)s_wk.y - 1, 60, 13, 8);    /* legs */
        wk_px(chx + 1, (int)s_wk.y - 1, 60, 13, 8);
    }
    if (wlx >= -6 && wlx <= 70) {
        wk_sprite(wlx, (int)lroundf(s_wk.y), t);
        if (s_w_n > 1) {
            bool talk = wk_i_own() || t < s_wko.grace_until;
            if (talk && (s_wko.burst ||
                         t - s_wko.last_tx > 250000)) {
                s_wko.last_tx = t;
                if (s_wko.burst) s_wko.burst--;
                /* SHADOW SIM: snapshot the COMPLETE sim state, run K
                   future steps through the identical pure machinery
                   (camera advanced per-step so the prediction rides
                   the future camera), capture the promise, restore.
                   Determinism makes clairvoyance free. */
                uint32_t K = (whm_sync_lead_ms() * 3 * 1000)
                             / (uint32_t)WK_TICK_US;
                if (K < 6) K = 6;
                if (K > 120) K = 120;
                /* never promise past the window: the live world
                   respawns at the anchor boundary, the shadow
                   would not - clamp K inside this window */
                uint32_t win = (uint32_t)(WK_ANCHOR_US / WK_TICK_US);
                if (s_wk_steps + K >= win) {
                    if (win > s_wk_steps + 2) K = win - s_wk_steps - 2;
                    else K = 0;
                }
                bool emit = K > 0;
                if (emit) {
                __typeof__(s_wk) save = s_wk;
                uint32_t sv_steps = s_wk_steps;
                uint8_t sv_draws = s_wk_draws;
                int32_t sv_chunk = s_wk_last_chunk;
                double sv_cam = s_cam_acc;
                uint8_t sv_seen[sizeof(s_wk_seen)];
                memcpy(sv_seen, s_wk_seen, sizeof(s_wk_seen));
                uint8_t sv_wr = s_wk_seen_wr;
                s_wk_shadowing = true;   /* the trace ring proved
                    shadow leakage (triplicated @321/321/320 entries
                    in his log): futures must not write history */
                for (uint32_t k2 = 0; k2 < K; k2++) {
                    s_wk_steps++;
                    s_cam_acc += ((double)WK_TICK_US / 1e6) *
                                 (double)WK_CAM_SPD *
                                 (double)s_wk_scroll;
                    int64_t fts = s_wk_anchor * WK_ANCHOR_US +
                                  (int64_t)s_wk_steps * WK_TICK_US;
                    wk_step(fts, n);
                }
                float fx = s_wk.x;
                int8_t fy = (int8_t)lroundf(s_wk.y * 2.0f);
                uint8_t fst = (uint8_t)s_wk.st;
                int8_t fdir = (int8_t)s_wk.dir;
                uint16_t ftm = (uint16_t)s_wk.timer;
                float ftg = s_wk.tgt;
                float fvx = s_wk.vx;
                uint32_t fstep = s_wk_steps;
                s_wk_shadowing = false;
                s_wk = save;
                s_wk_steps = sv_steps;
                s_wk_draws = sv_draws;
                s_wk_last_chunk = sv_chunk;
                s_cam_acc = sv_cam;
                memcpy(s_wk_seen, sv_seen, sizeof(s_wk_seen));
                s_wk_seen_wr = sv_wr;
                {
                    static uint8_t wkp_ctr;
                    if (s_wkp_poke || ++wkp_ctr >= 20) {
                        wkp_ctr = 0;
                        s_wkp_poke = false;
                        whm_sync_wkparams_send(s_wk_anchor,
                            (float)s_cam_acc,   /* SEMANTIC: cam
                                                   POSITION now */
                            (uint8_t)s_w_n);
                    }
                }
                whm_sync_wkb_send(s_wko.owner, fx, fy, fst, fdir,
                                  ftm, fstep, ftg, fvx);
                }
            }
            if (!wk_i_own() && t - s_wko.rx_us > 1200000) {
                int ns = (int)floorf((s_wk.x - cam) / 64.0f);
                float fz = (s_wk.x - cam) - (float)ns * 64.0f;
                /* symmetric deadband (audit R3): claim only when
                   genuinely inside - the seam band belongs to the
                   incumbent. */
                if (fz < 4.0f || fz > 60.0f) ns = -1;
                if (ns == (int)s_w_idx) {
                    s_wko.owner = s_w_idx;      /* SEIZE: owner gone */
                    s_wko.own_tsf = whm_wifi_tsf_now();
                    s_wko.burst = 2;
                    s_wko.resync = 1;
                    printf("walker: owner silent - seizing (strip "
                           "%u)\n", s_w_idx);
                }
            }
        }
        wk_nye_scene(cam, t);
        fw_tick(t);
        fw_render_all(t);
    }
    char tag[8];
    snprintf(tag, sizeof(tag), "%u/%u", (unsigned)(s_w_idx + 1),
             (unsigned)n);
    gfx_text(2, 2, tag, 1, 6, 6, 15);
}

/* -------- game-of-life palettes -------- */
static int64_t s_frame_t = 0;   /* screens-line frame t (also
   read by prism, which lives above the render loop) */

#define LIFE_PAL_N 8
static uint8_t s_life_pal = 4;   /* rainbow default */
static uint32_t s_life_wx = 0;              /* strip world-x offset */
static int64_t s_life_toast_until = 0;
static const char *k_life_pal_names[LIFE_PAL_N] = {
    "classic", "blue", "green", "red", "rainbow",
    "purple", "ember", "prism",
};

static void hsv_rgb(uint16_t h, uint8_t s, uint8_t v,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t i = (uint8_t)((h / 60) % 6);
    uint16_t f = (uint16_t)((h % 60) * 255 / 60);
    uint8_t p = (uint8_t)((uint16_t)v * (255 - s) / 255);
    uint8_t q = (uint8_t)((uint32_t)v * (255 - (uint32_t)s * f / 255) / 255);
    uint8_t t = (uint8_t)((uint32_t)v *
                          (255 - (uint32_t)s * (255 - f) / 255) / 255);
    switch (i) {
    case 0: *r = v; *g = t; *b = p; break;
    case 1: *r = q; *g = v; *b = p; break;
    case 2: *r = p; *g = v; *b = t; break;
    case 3: *r = p; *g = q; *b = v; break;
    case 4: *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

static void life_color(uint8_t age, uint8_t x, uint8_t y,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (s_life_pal) {
    case 5:                                   /* purple */
        if (age == 1)      { *r = 235; *g = 210; *b = 255; }
        else if (age < 6)  { *r = 190; *g = 120; *b = 255; }
        else if (age < 20) { *r = 130; *g = 60;  *b = 220; }
        else               { *r = 70;  *g = 25;  *b = 120; }
        break;
    case 6:                                   /* ember: heat-death ramp */
        if (age == 1)      { *r = 255; *g = 255; *b = 230; }
        else if (age < 6)  { *r = 255; *g = 200; *b = 60;  }
        else if (age < 20) { *r = 255; *g = 90;  *b = 20;  }
        else if (age < 60) { *r = 180; *g = 30;  *b = 10;  }
        else               { *r = 70;  *g = 22;  *b = 24;  }
        break;
    case 7: {                                 /* prism: hue from position,
                                                 slow global rotation */
        /* WORLD x + frame-grid rotation: phase-locked fleet-wide AND
           continuous across the strip */
        uint16_t hue = (uint16_t)((((uint32_t)x + s_life_wx) * 5u +
                       (uint32_t)y * 11u +
                       (uint32_t)(s_frame_t / 150000)) % 360);
        hsv_rgb(hue, age == 1 ? 90 : 235, age == 1 ? 255 : 215, r, g, b);
        break;
    }
    case 1:                                   /* blue */
        if (age == 1)      { *r = 200; *g = 230; *b = 255; }
        else if (age < 6)  { *r = 90;  *g = 150; *b = 255; }
        else if (age < 20) { *r = 40;  *g = 80;  *b = 230; }
        else               { *r = 20;  *g = 30;  *b = 110; }
        break;
    case 2:                                   /* green */
        if (age == 1)      { *r = 210; *g = 255; *b = 210; }
        else if (age < 6)  { *r = 90;  *g = 240; *b = 120; }
        else if (age < 20) { *r = 40;  *g = 180; *b = 70;  }
        else               { *r = 20;  *g = 90;  *b = 40;  }
        break;
    case 3:                                   /* red */
        if (age == 1)      { *r = 255; *g = 225; *b = 210; }
        else if (age < 6)  { *r = 255; *g = 140; *b = 90;  }
        else if (age < 20) { *r = 230; *g = 70;  *b = 40;  }
        else               { *r = 120; *g = 30;  *b = 25;  }
        break;
    case 4: {                                 /* rainbow: hue = age + drift */
        /* v2: world-x gradient joins the age animation - rainbow sweeps
           flow through the bezel; drift now rides the frame grid (the
           old esp_timer term was the same local-phase bug prism had). */
        uint16_t hue = (uint16_t)(((uint32_t)age * 7 +
                       ((uint32_t)x + s_life_wx) * 2 +
                       (uint32_t)(s_frame_t / 80000)) % 360);
        hsv_rgb(hue, age == 1 ? 90 : 255, age == 1 ? 255 : 210, r, g, b);
        break;
    }
    default:                                  /* classic (original look) */
        if (age == 1)      { *r = 255; *g = 255; *b = 255; }
        else if (age < 6)  { *r = 60;  *g = 220; *b = 220; }
        else if (age < 20) { *r = 40;  *g = 90;  *b = 220; }
        else               { *r = 30;  *g = 40;  *b = 90;  }
        break;
    }
}

static void life_tap2(void)
{
    s_life_pal = (uint8_t)((s_life_pal + 1) % LIFE_PAL_N);
    whm_settings_set_u8("life_pal", s_life_pal);
    s_life_toast_until = esp_timer_get_time() + 1500000;
}
static void life_seed(void);
static void oracle_tap(void);
static void life_hold(void) { life_seed(); }
static uint8_t *s_life_a;   /* tentative; defined with the life screen */

#ifndef WHM_VERSION_STR
#define WHM_VERSION_STR "dev"
#endif
#define WHM_VERSION    WHM_VERSION_STR

#define TICK_MS        33          /* ~30fps base tick                    */
#define AUTO_DWELL_MS  10000       /* screen rotation period              */

/* ------------------------------------------------------------------ modes */

typedef enum { M_SCR_AUTO, M_SCR_HOLD, M_PAT_HOLD, M_PAT_CYCLE, M_TEXT, M_BOOT } ui_mode_t;

typedef enum {
    SCR_HOME,
    SCR_CLOCK, SCR_TIMER, SCR_WATCH, SCR_MUSIC,
    SCR_LEVEL, SCR_ENV, SCR_NET, SCR_SYS, SCR_SD, SCR_VU, SCR_LIFE,
    SCR_ORACLE,
    SCR_COUNT
} screen_t;

typedef enum {
    P_CORNERS, P_RED, P_GREEN, P_BLUE, P_WHITE,
    P_GRADIENT, P_GRAY, P_CHECKER, P_SWEEP, P_CYCLE, P_WANDER, P_WALKER,
    P_COUNT
} pattern_t;

static const char *k_scr_names[SCR_COUNT] = {
    "home",
    "clock", "timer", "stopwatch", "music",
    "level", "env", "net", "sys", "sd", "vu", "life", "oracle",
};

/* contextual short-press: a screen may consume the button (return true).
 * This is the app-control hook - Oracle/games plug in here later. */
static void timer_tap2(void);
static void timer_hold(void);
static void watch_tap2(void);
static void watch_hold(void);
static void music_tap2(void);
static void music_hold(void);
static void life_tap2(void);
static void life_hold(void);
static bool colon_phase_on(void);
static void draw_hhmm(int16_t x, uint16_t y, uint8_t sc, uint8_t h, uint8_t m,
                      bool colon_on, uint8_t r, uint8_t g, uint8_t b);
/* Button grammar v2: SINGLE press is ALWAYS next-screen (universal nav).
 * DOUBLE tap and LONG hold are the contextual app channel; screens with no
 * double handler dispatch single instantly (no disambiguation latency). */
static void (*k_tap2[SCR_COUNT])(void) = {
    [SCR_TIMER] = timer_tap2,
    [SCR_WATCH] = watch_tap2,
    [SCR_MUSIC] = music_tap2,
    [SCR_LIFE]  = life_tap2,
    [SCR_ORACLE] = oracle_tap,
};
static void (*k_hold[SCR_COUNT])(void) = {
    [SCR_TIMER] = timer_hold,
    [SCR_WATCH] = watch_hold,
    [SCR_MUSIC] = music_hold,
    [SCR_LIFE]  = life_hold,
};
static const char *k_pat_names[P_COUNT] = {
    "corners", "red", "green", "blue", "white",
    "gradient", "gray", "checker", "sweep", "cycle", "wander", "walker",
};

static volatile ui_mode_t s_mode = M_BOOT;      /* boot: Ignition */
static int64_t s_boot0 = 0;
static volatile screen_t s_screen = SCR_HOME;
static volatile pattern_t s_pattern = P_CYCLE;
static volatile screen_t s_return_scr = SCR_HOME;   /* where to resume */
static volatile ui_mode_t s_return_mode = M_SCR_HOLD;
static volatile bool s_name_dirty = false;

void whm_ui_name_dirty(void) { s_name_dirty = true; }


static volatile uint32_t s_gen = 0;     /* bumped on any selection change */

/* Fleet-correct time: system wall clock (SNTP/flock-disciplined), never
 * raw RTC registers - the RTC's second boundary sits at whatever
 * sub-second phase the last write truncated to, DIFFERENT per unit, which
 * is exactly why two panels' seconds bars stepped apart while their
 * colons (already wall-driven) blinked together. RTC = boot seed +
 * persistence only. */
static bool wall_now(struct tm *lt, suseconds_t *usec)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, lt);
    if (usec) *usec = tv.tv_usec;
    return lt->tm_year + 1900 >= 2025;
}

static void ui_resume_screens(void);

bool whm_ui_pattern_active(const char **name)
{
    bool act = (s_mode == M_PAT_HOLD || s_mode == M_PAT_CYCLE);
    if (name) *name = act ? k_pat_names[s_pattern] : NULL;
    return act;
}

void whm_ui_pattern_off(void)
{
    if (s_mode == M_PAT_HOLD || s_mode == M_PAT_CYCLE) {
        ui_resume_screens();
    }
}

/* ---- global frame grid (R1) -----------------------------------------
 * 60fps MASTER GRID on the show clock: every unit renders content for
 * the SAME t and flips at the same gridline. Cross-panel residual is
 * the panel-local 150Hz flip quantization (6.7ms) + sub-ms wake jitter
 * - the honest floor, now actually achieved instead of merely
 * advertised. Screens ride the /2 line (33.3ms) to keep sensor-poll
 * cadence sane; motion-critical renderers ride /1. */
#define FRAME_US 16667LL
static int64_t ui_frame_wait_div(int div)
{
    int64_t per = FRAME_US * (int64_t)div;
    int64_t t = whm_wifi_tsf_now();
    if (t <= 0) t = esp_timer_get_time();     /* solo fallback grid */
    int64_t next = ((t / per) + 1) * per;
    int64_t left = next - t;
    if (left > 3000) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((left - 2000) / 1000)));
    }
    for (;;) {
        t = whm_wifi_tsf_now();
        if (t <= 0) t = esp_timer_get_time();
        if (t >= next) break;
        esp_rom_delay_us(200);
    }
    return next;
}

static void ui_resume_screens(void)
{
    s_screen = s_return_scr;
    s_mode = s_return_mode;
    s_gen++;
    ESP_LOGI(TAG, "resumed %s (%s)", k_scr_names[s_screen],
             s_return_mode == M_SCR_AUTO ? "auto" : "held");
}

const char *whm_ui_screen_names(void) { return "auto next home clock timer stopwatch music level env net sys sd vu life"; }
const char *whm_ui_pattern_names(void) { return "cycle corners red green blue white gradient gray checker sweep"; }

bool whm_ui_screen_set(const char *name)
{
    if (strcmp(name, "auto") == 0) {
        s_mode = M_SCR_AUTO; s_gen++;
        ESP_LOGI(TAG, "screens: auto-rotate");
        return true;
    }
    if (strcmp(name, "next") == 0) {
        s_screen = (screen_t)((s_screen + 1) % SCR_COUNT);
        if (s_mode != M_SCR_AUTO) s_mode = M_SCR_HOLD;
        s_gen++;
        ESP_LOGI(TAG, "screen -> %s", k_scr_names[s_screen]);
        return true;
    }
    if (strcmp(name, "back") == 0 || strcmp(name, "prev") == 0) {
        s_screen = (screen_t)((s_screen + SCR_COUNT - 1) % SCR_COUNT);
        if (s_mode != M_SCR_AUTO) s_mode = M_SCR_HOLD;
        s_gen++;
        ESP_LOGI(TAG, "screen -> %s", k_scr_names[s_screen]);
        return true;
    }
    for (int i = 0; i < SCR_COUNT; i++) {
        if (strcmp(name, k_scr_names[i]) == 0) {
            s_screen = (screen_t)i;
            s_mode = M_SCR_HOLD;
            s_gen++;
            ESP_LOGI(TAG, "screen -> %s (held; 'screen auto' resumes)", name);
            return true;
        }
    }
    return false;
}

static char s_text[128] = "";
typedef struct {
    bool span;
    uint8_t size;           /* 0 auto, 1, 2 */
    uint8_t scroll;         /* 0 auto, 1 off, 2 read, 3 bounce, 4 loop */
    uint16_t speed;
} txtcfg_t;
static txtcfg_t s_txtcfg = { false, 0, 0, 30 };

void whm_ui_text_cfg(bool span, uint8_t size, uint8_t scroll,
                     uint16_t speed)
{
    s_txtcfg.span = span;
    s_txtcfg.size = size;
    s_txtcfg.scroll = scroll;
    s_txtcfg.speed = speed ? speed : 30;
}

void whm_ui_text_clear(void)
{
    if (s_mode == M_TEXT) ui_resume_screens();
}

void whm_ui_show_text(const char *msg)
{
    strlcpy(s_text, msg && msg[0] ? msg : " ", sizeof(s_text));
    if (s_mode == M_SCR_AUTO || s_mode == M_SCR_HOLD) {
        s_return_scr = s_screen;
        s_return_mode = s_mode;
    }
    s_mode = M_TEXT;
    s_gen++;
    ESP_LOGI(TAG, "text mode: \"%s\"", s_text);
}

bool whm_ui_pattern_set(const char *name)
{
    for (int i = 0; i < P_COUNT; i++) {
        if (strcmp(name, k_pat_names[i]) == 0) {
            s_pattern = (pattern_t)i;
            if (s_mode == M_SCR_AUTO || s_mode == M_SCR_HOLD) {
                s_return_scr = s_screen;
                s_return_mode = s_mode;
            }
            s_mode = (i == P_CYCLE) ? M_PAT_CYCLE : M_PAT_HOLD;
            s_gen++;
            ESP_LOGI(TAG, "pattern -> %s", name);
    if (s_pattern == P_WALKER) {
        /* ENTRY = REPLAY POINT (doctrine 14): a resuming unit and a
           fresh one must rebuild identically. Force respawn + full
           catch-up from the shared anchor on every entry. */
        printf("walker: anchor INVALIDATED (site %d)\n", __LINE__);
        s_wk_anchor = INT64_MIN;
    }
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------- helpers */


static void chrome(const char *title, screen_t idx)
{
    gfx_text_center(W / 2, 1, title, 1, C_DIM);
    /* page dots */
    uint16_t total_w = SCR_COUNT * 4 - 2;
    uint16_t x0 = (W - total_w) / 2;
    for (int i = 0; i < SCR_COUNT; i++) {
        uint8_t v = (i == (int)idx) ? 200 : 40;
        whm_display_fill_rect(x0 + i * 4, H - 2, 2, 2, v, v, v);
    }
}

/* centered when it fits; otherwise ping-pong marquee with end holds.
 * Stateless: phase derives from esp_timer, so no per-line bookkeeping. */
/* Reading-order marquee: present the START of the text, hold so the eye
 * lands naturally, scroll left at reading pace to reveal the rest, hold at
 * the end, snap home, repeat. Per-slot phase state resets whenever the
 * text changes, so every new string is met at its first word - never
 * mid-sentence at a random phase of a global clock. */
#define MARQ_SLOTS 7   /* slot 6 = overlay */
typedef enum { MQ_READ, MQ_BOUNCE } marq_mode_t;
static void marquee_m(uint8_t slot, marq_mode_t mode, uint16_t x, uint16_t y,
                      uint16_t w, const char *s, uint8_t scale,
                      uint8_t r, uint8_t g, uint8_t b)
{
    static struct { char sig[10]; uint16_t tw; int64_t t0; } st[MARQ_SLOTS];
    if (slot >= MARQ_SLOTS) slot = 0;

    uint16_t tw = gfx_text_width(s, scale);
    if (tw <= w) {
        gfx_text(x + (w - tw) / 2, y, s, scale, r, g, b);
        return;
    }
    if (strncmp(st[slot].sig, s, sizeof(st[slot].sig) - 1) != 0 ||
        st[slot].tw != tw) {
        strlcpy(st[slot].sig, s, sizeof(st[slot].sig));
        st[slot].tw = tw;
        st[slot].t0 = esp_timer_get_time();
    }
    uint32_t over = tw - w;
    const uint32_t hold = 1200, per_px = 45;   /* ~22 px/s reading pace */
    uint32_t run = over * per_px;
    uint32_t off;
    if (mode == MQ_BOUNCE) {
        /* start at the beginning, glide left, glide back, repeat */
        uint32_t period = 2 * (hold + run);
        uint32_t t = (uint32_t)(((esp_timer_get_time() - st[slot].t0)
                                 / 1000) % period);
        if (t < hold) off = 0;
        else if (t < hold + run) off = (t - hold) / per_px;
        else if (t < 2 * hold + run) off = over;
        else off = over - (t - 2 * hold - run) / per_px;
    } else {
        uint32_t period = hold + run + hold;
        uint32_t t = (uint32_t)(((esp_timer_get_time() - st[slot].t0)
                                 / 1000) % period);
        if (t < hold) off = 0;
        else if (t < hold + run) off = (t - hold) / per_px;
        else off = over;
    }

    gfx_set_clip(x, x + w);
    gfx_text((int16_t)x - (int16_t)off, y, s, scale, r, g, b);
    gfx_clear_clip();
}

static void marquee(uint8_t slot, uint16_t x, uint16_t y, uint16_t w,
                    const char *s, uint8_t scale,
                    uint8_t r, uint8_t g, uint8_t b)
{
    marquee_m(slot, MQ_READ, x, y, w, s, scale, r, g, b);
}

/* unified progress bar: one dim track constant, bright fills - the single
 * contrast knob for every indicator on the panel */
#define BAR_TRACK 13, 13, 22
static void bar(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                uint32_t num, uint32_t den,
                uint8_t r, uint8_t g, uint8_t b)
{
    whm_display_fill_rect(x, y, w, h, BAR_TRACK);
    if (!den) return;
    uint32_t fw = (uint32_t)w * num / den;
    if (fw > w) fw = w;
    if (fw) whm_display_fill_rect(x, y, (uint16_t)fw, h, r, g, b);
}

static void fmt_uptime(char *out, size_t n)
{
    int64_t s = esp_timer_get_time() / 1000000;
    snprintf(out, n, "%02d:%02d:%02d",
             (int)(s / 3600), (int)((s / 60) % 60), (int)(s % 60));
}

/* ---------------------------------------------------------------- screens */

/* ------------------------------ timer + stopwatch (run across screens) */

typedef enum { T_IDLE, T_RUN, T_PAUSE, T_DONE } timer_state_t;
static timer_state_t s_t_state = T_IDLE;
static uint32_t s_t_default_s = 180;
static int64_t s_t_total_us = 0;
static int64_t s_t_remain_us = 0;      /* valid in IDLE/PAUSE/DONE */
static uint32_t s_t_alarm_n = 0;
static int64_t s_t_alarm_next_us = 0;
static int64_t s_t_deadline_us = 0;    /* valid in RUN */

static bool s_w_running = false;
static int64_t s_w_accum_us = 0;
static int64_t s_w_started_us = 0;

static void timer_load_default(void)
{
    uint32_t v = 180;
    whm_settings_get_u32("timer_def", &v);
    if (v < 1) v = 1;
    if (v > 5999) v = 5999;
    s_t_default_s = v;
    s_t_total_us = (int64_t)v * 1000000LL;
    s_t_remain_us = s_t_total_us;
}

void whm_timer_set_default(uint32_t seconds)
{
    if (seconds < 1) seconds = 1;
    if (seconds > 5999) seconds = 5999;
    whm_settings_set_u32("timer_def", seconds);
    if (s_t_state == T_IDLE || s_t_state == T_DONE) {
        timer_load_default();
        s_t_state = T_IDLE;
    } else {
        s_t_default_s = seconds;
    }
}

void whm_timer_set(uint32_t seconds)          /* one-off, not saved */
{
    if (seconds < 1) seconds = 1;
    if (seconds > 5999) seconds = 5999;
    s_t_total_us = (int64_t)seconds * 1000000LL;
    s_t_remain_us = s_t_total_us;
    s_t_state = T_IDLE;
}

void whm_timer_start(void)
{
    s_t_alarm_n = 0;

    if (s_t_state == T_RUN) return;
    if (s_t_state == T_DONE || s_t_remain_us <= 0) {
        s_t_remain_us = s_t_total_us;
    }
    s_t_deadline_us = esp_timer_get_time() + s_t_remain_us;
    s_t_state = T_RUN;
}

void whm_timer_pause(void)
{
    if (s_t_state != T_RUN) return;
    s_t_remain_us = s_t_deadline_us - esp_timer_get_time();
    if (s_t_remain_us < 0) s_t_remain_us = 0;
    s_t_state = T_PAUSE;
}

void whm_timer_stop(void)
{
    /* abort: back to the CONFIGURED duration, idle (a custom `timer set`
       survives) */
    s_t_remain_us = s_t_total_us;
    s_t_state = T_IDLE;
    s_t_alarm_n = 0;
}

void whm_timer_reset(void)
{
    /* full reset: restore the NVS default duration */
    s_t_total_us = (int64_t)s_t_default_s * 1000000LL;
    s_t_remain_us = s_t_total_us;
    s_t_state = T_IDLE;
    s_t_alarm_n = 0;
}

uint32_t whm_timer_get_default(void) { return s_t_default_s; }

int whm_ui_life_pal_count(void) { return LIFE_PAL_N; }
const char *whm_ui_life_pal_name(int i)
{
    return (i >= 0 && i < LIFE_PAL_N) ? k_life_pal_names[i] : "?";
}
int whm_ui_life_get_pal(void) { return s_life_pal; }
esp_err_t whm_ui_life_set_pal(int i)
{
    if (i < 0 || i >= LIFE_PAL_N) return ESP_ERR_INVALID_ARG;
    s_life_pal = (uint8_t)i;
    whm_settings_set_u8("life_pal", s_life_pal);
    s_life_toast_until = esp_timer_get_time() + 1500000;
    return ESP_OK;
}
void whm_ui_life_reseed(void)
{
    if (s_life_a) life_seed();
}

void whm_timer_status(uint32_t *remain_s, uint32_t *total_s, int *state)
{
    int64_t rem = (s_t_state == T_RUN)
                      ? s_t_deadline_us - esp_timer_get_time()
                      : s_t_remain_us;
    if (rem < 0) rem = 0;
    if (remain_s) *remain_s = (uint32_t)((rem + 999999) / 1000000);
    if (total_s) *total_s = (uint32_t)(s_t_total_us / 1000000);
    if (state) *state = (int)s_t_state;
}

/* runs every ui tick regardless of mode: expiry beeps + pulls focus */
static void timer_service(void)
{
    /* repeating alarm: re-beep every 2.5s while DONE, capped at 8 rounds
       (~20s) so an unattended panel doesn't chirp forever; any stop/
       reset/start acknowledges */
    if (s_t_state == T_DONE && s_t_alarm_n && s_t_alarm_n < 8 &&
        esp_timer_get_time() >= s_t_alarm_next_us) {
        whm_audio_beep_async();
        s_t_alarm_n++;
        s_t_alarm_next_us += 2500000LL;
    }

    if (s_t_state == T_RUN && esp_timer_get_time() >= s_t_deadline_us) {
        s_t_state = T_DONE;
        s_t_alarm_n = 1;
        s_t_alarm_next_us = esp_timer_get_time() + 2500000LL;
        s_t_remain_us = 0;
        whm_audio_beep_async();
        ESP_LOGI(TAG, "timer expired");
        if (!((s_mode == M_SCR_AUTO || s_mode == M_SCR_HOLD) &&
              s_screen == SCR_TIMER)) {
            s_screen = SCR_TIMER;
            s_mode = M_SCR_HOLD;
            s_gen++;
        }
    }
}

void whm_watch_start(void)
{
    if (s_w_running) return;
    s_w_started_us = esp_timer_get_time();
    s_w_running = true;
}

void whm_watch_stop(void)
{
    if (!s_w_running) return;
    s_w_accum_us += esp_timer_get_time() - s_w_started_us;
    s_w_running = false;
}

void whm_watch_reset(void)
{
    s_w_running = false;
    s_w_accum_us = 0;
}

int64_t whm_watch_elapsed_us(bool *running)
{
    if (running) *running = s_w_running;
    return s_w_accum_us +
           (s_w_running ? esp_timer_get_time() - s_w_started_us : 0);
}

static void timer_tap2(void)
{
    switch (s_t_state) {
    case T_IDLE:
    case T_PAUSE: whm_timer_start(); break;
    case T_RUN:   whm_timer_pause(); break;
    case T_DONE:  whm_timer_stop(); break;
    }
}
static void timer_hold(void) { whm_timer_reset(); }

static void watch_tap2(void)
{
    bool run;
    whm_watch_elapsed_us(&run);
    if (run) whm_watch_stop();
    else whm_watch_start();               /* start or resume */
}
static void watch_hold(void) { whm_watch_reset(); }

static void music_tap2(void) { whm_mp3_toggle(); }
static void music_hold(void) { whm_mp3_next(); }



/* console `text` payload: big if it fits, otherwise small, otherwise marquee */
static void text_render(int64_t tf, const char *msg,
                        const txtcfg_t *cfg, int16_t yy_fixed,
                        int8_t dx, int8_t dy,
                        uint8_t cr, uint8_t cg, uint8_t cb,
                        uint8_t slot)
{
    uint8_t sidx, sn;
    uint16_t sspd;
    whm_ui_wander_get(&sidx, &sn, &sspd);
    (void)sspd;
    bool span = cfg->span && sn > 1;
    float vw = span ? (float)sn * 64.0f : (float)W;
    int ox = span ? (int)sidx * 64 : 0;

    uint8_t sc = cfg->size;
    if (sc == 0) {
        sc = gfx_text_width(msg, 2) <= (uint16_t)(vw - 4) ? 2 : 1;
    }
    uint16_t tw = gfx_text_width(msg, sc);
    uint16_t yy = yy_fixed >= 0 ? (uint16_t)yy_fixed
                : (uint16_t)((H - (sc == 2 ? 16 : 8)) / 2);
    yy = (uint16_t)((int)yy + dy);
    bool fits = tw <= (uint16_t)(vw - 2);

    uint8_t mode = cfg->scroll;
    if (mode == 0) mode = span ? 4 : 2;
    if (span && (mode == 2 || mode == 3)) mode = 4;

    if (fits || mode == 1) {
        float xv = fits ? (vw - (float)tw) / 2.0f : 1.0f;
        gfx_set_clip(0, W);
        gfx_text((int16_t)(lroundf(xv) - ox + dx), yy, msg, sc,
                 cr, cg, cb);
        gfx_clear_clip();
        return;
    }
    if (!span && mode == 2) {
        marquee_m(slot, MQ_READ, (uint16_t)dx, yy, W, msg, sc,
                  cr, cg, cb);
        return;
    }
    if (!span && mode == 3) {
        marquee_m(slot, MQ_BOUNCE, (uint16_t)dx, yy, W, msg, sc,
                  cr, cg, cb);
        return;
    }
    int64_t t = tf;
    if (t <= 0) {
        t = whm_wifi_tsf_now();
        if (t <= 0) t = esp_timer_get_time();
    }
    double period = (double)tw + (double)vw;
    double off = fmod(((double)t / 1e6) * (double)cfg->speed, period);
    gfx_set_clip(0, W);
    gfx_text((int16_t)(lroundf((float)vw - (float)off) - ox + dx), yy,
             msg, sc, cr, cg, cb);
    gfx_clear_clip();
}

static void scr_text(int64_t tf)
{
    text_render(tf, s_text, &s_txtcfg, -1, 0, 0, 255, 235, 200, 0);
}

/* -------- Ignition (WS4): ~1.35s boot flourish, phase-locked to the
 * chime's note onsets (0/130/260/390/540ms -> chord). Nonblocking by
 * construction - it IS the UI task while wifi/SD/sync rise beneath. */
static void ignition_render(int ms)
{
    static const int k_on[5] = { 0, 130, 260, 390, 540 };
    static const uint16_t k_hue[5] = { 210, 170, 130, 80, 25 };
    whm_display_fill(0, 0, 1);
    int cx = 32, cy = 27;
    if (ms < 620) {                          /* the waiting spark */
        int v = 130 + (int)(100.0f * sinf((float)ms * 0.02f));
        whm_display_set_pixel((uint16_t)cx, (uint16_t)cy,
                              (uint8_t)v, (uint8_t)v, (uint8_t)(v / 2));
        whm_display_set_pixel((uint16_t)(cx + 1), (uint16_t)cy,
                              (uint8_t)(v / 2), (uint8_t)(v / 2), 20);
        whm_display_set_pixel((uint16_t)cx, (uint16_t)(cy - 1),
                              (uint8_t)(v / 2), (uint8_t)(v / 2), 20);
    }
    for (int i = 0; i < 5; i++) {            /* one ring per note */
        int e = ms - k_on[i];
        if (e < 0) continue;
        float rr = (float)e * 0.030f;   /* slower bloom: rings live on */
        int v = 255 - (int)(rr * 5.2f);
        if (v < 10) continue;
        uint8_t r, g, b;
        hsv_rgb((uint16_t)((k_hue[i] + ms / 14) % 360), 235,
                (uint8_t)v, &r, &g, &b);
        for (int a = 0; a < 72; a++) {
            float th = (float)a * 0.0873f;
            int px = cx + (int)lroundf(cosf(th) * rr);
            int py = cy + (int)lroundf(sinf(th) * rr * 0.9f);
            if (px >= 0 && px < W && py >= 0 && py < H) {
                whm_display_set_pixel((uint16_t)px, (uint16_t)py,
                                      r, g, b);
            }
        }
    }
    if (ms >= 540) {                         /* the name, on the chord */
        static char nm[17] = "";
        if (!nm[0]) {
            whm_settings_get_str("node", nm, sizeof(nm));
            if (!nm[0]) strlcpy(nm, "WHM", sizeof(nm));
        }
        int v = (ms - 540) * 255 / 450;   /* deeper dissolve */
        if (v > 255) v = 255;
        uint8_t sc = gfx_text_width(nm, 2) <= W - 4 ? 2 : 1;
        gfx_text_center(W / 2, 22, nm, sc,
                        (uint8_t)(213 * v / 255),
                        (uint8_t)(194 * v / 255),
                        (uint8_t)(167 * v / 255));
        if (ms >= 1000) {                    /* scarf-orange underline */
            uint16_t tw = gfx_text_width(nm, sc);
            int ux = (ms - 1000) * (int)tw / 400;
            if (ux > (int)tw) ux = (int)tw;
            whm_display_fill_rect((uint16_t)((W - tw) / 2),
                                  (uint16_t)(22 + sc * 8 + 2),
                                  (uint16_t)ux, 2, 150, 45, 8);
        }
    }
    {   /* boot version: bottom-center, fading in ~0.7-1.1 s */
        float a = ((float)ms - 700.0f) / 400.0f;
        if (a > 0.0f) {
            if (a > 1.0f) a = 1.0f;
            const char *v = WHM_VERSION_STR;
            int len = 0;
            while (v[len]) len++;
            int x0 = 32 - len * 2;
            ota_text(v, x0, 57, (uint8_t)(198.0f * a),
                     (uint8_t)(168.0f * a), (uint8_t)(72.0f * a));
        }
    }
}

/* -------- overlay compositor (WS3): text riding ABOVE any base -------- */
static struct {
    bool on, band, logged;
    uint8_t pos;                 /* 0 top, 1 mid, 2 low */
    char msg[96];
    txtcfg_t cfg;
} s_ovl;

void whm_ui_overlay(const char *msg, bool span, uint8_t size,
                    uint8_t scroll, uint16_t speed, bool band,
                    uint8_t pos)
{
    if (!msg || !msg[0]) {
        s_ovl.on = false;
        return;
    }
    strlcpy(s_ovl.msg, msg, sizeof(s_ovl.msg));
    s_ovl.cfg.span = span;
    s_ovl.cfg.size = size;
    s_ovl.cfg.scroll = scroll;
    s_ovl.cfg.speed = speed ? speed : 30;
    s_ovl.band = band;
    s_ovl.pos = pos;
    s_ovl.logged = false;
    s_ovl.on = true;
    ESP_LOGI(TAG, "overlay set: '%s' span=%d size=%d scroll=%d",
             s_ovl.msg, (int)span, (int)size, (int)scroll);
}

bool whm_ui_overlay_active(void) { return s_ovl.on; }

static void ui_overlay_draw(int64_t tf);   /* defined just below */
static void ui_present(int64_t t)
{
    ui_overlay_draw(t);
    whm_display_flip();
}

static void ui_present(int64_t t);   /* fwd: single flip choke point */

static void ui_overlay_draw(int64_t tf)
{
    if (!s_ovl.on) return;
    uint8_t sidx, sn;
    uint16_t sspd;
    whm_ui_wander_get(&sidx, &sn, &sspd);
    (void)sidx; (void)sspd;
    bool span = s_ovl.cfg.span && sn > 1;
    float vw = span ? (float)sn * 64.0f : (float)W;
    uint8_t sc = s_ovl.cfg.size;
    if (sc == 0) {
        sc = gfx_text_width(s_ovl.msg, 2) <= (uint16_t)(vw - 4) ? 2 : 1;
    }
    uint8_t gh = sc == 2 ? 16 : 8;
    int16_t yy = s_ovl.pos == 0 ? 3
               : s_ovl.pos == 1 ? (int16_t)((H - gh) / 2)
                                : (int16_t)(H - gh - 4);
    if (s_ovl.band) {
        whm_display_fill_rect(0, (uint16_t)(yy - 2), W,
                              (uint16_t)(gh + 4), 2, 2, 5);
    }
    if (!s_ovl.logged) {
        s_ovl.logged = true;
        ESP_LOGI(TAG, "overlay draw: yy=%d sc=%d tw=%d vw=%d",
                 (int)yy, (int)sc, (int)gfx_text_width(s_ovl.msg, sc),
                 (int)vw);
    }
    /* drop shadow, then ink */
    text_render(tf, s_ovl.msg, &s_ovl.cfg, yy, 1, 1, 0, 0, 0, 6);
    text_render(tf, s_ovl.msg, &s_ovl.cfg, yy, 0, 0, 255, 238, 205, 6);
}

static void health_dot(uint16_t x, const char *lbl, bool ok)
{
    gfx_text(x, 46, lbl, 1, C_HINT);
    uint16_t w = gfx_text_width(lbl, 1);
    whm_display_fill_rect(x + w / 2 - 1, 55, 3, 3,
                          ok ? 40 : 220, ok ? 220 : 50, 40);
}

/* BLE SAS pairing overlay (wh-link): six digits on the panel, the
 * BOOT button decides - the display is the trusted screen. */
static struct {
    volatile bool active;
    char digits[8];
    volatile int done;        /* -1 pending, 0 rejected, 1 paired */
    int64_t done_at;
} s_sas;

void whm_ui_sas_show(const char *digits)
{
    strlcpy(s_sas.digits, digits, sizeof(s_sas.digits));
    s_sas.done = -1;
    s_sas.done_at = 0;
    s_sas.active = true;
}

void whm_ui_sas_done(bool ok)
{
    s_sas.done = ok ? 1 : 0;
    s_sas.done_at = esp_timer_get_time();
}

static bool sas_screen(int64_t t)
{
    if (!s_sas.active) return false;
    if (s_sas.done >= 0 && t - s_sas.done_at > 1400000) {
        s_sas.active = false;
        return false;
    }
    whm_display_fill(3, 2, 10);
    ota_text("BLE PAIR", 16, 4, 240, 200, 90);
    gfx_text_center(W / 2, 22, s_sas.digits, 2, 255, 255, 255);
    if (s_sas.done < 0) {
        ota_text("TAP YES", 18, 46, 60, 220, 130);
        ota_text("HOLD NO", 18, 54, 240, 120, 90);
    } else if (s_sas.done == 1) {
        ota_text("PAIRED", 20, 50, 60, 220, 130);
    } else {
        ota_text("REJECTED", 14, 50, 240, 80, 80);
    }
    return true;
}

/* FLEET INDICATOR (owner's design): role glyph + follower-count
 * digit for anchor/conductor; a state circle for followers. A digit
 * reads across the room and scales to unit Three. */
static void fleet_glyph_anchor(int x, int y, uint8_t r, uint8_t g,
                               uint8_t b)
{
    static const uint8_t A[5] = { 0x04, 0x0E, 0x04, 0x15, 0x0E };
    for (int ry = 0; ry < 5; ry++)
        for (int rx = 0; rx < 5; rx++)
            if (A[ry] & (0x10 >> rx)) wk_px(x + rx, y + ry, r, g, b);
}

static void fleet_glyph_baton(int x, int y, uint8_t r, uint8_t g,
                              uint8_t b)
{
    for (int i = 0; i < 5; i++)
        wk_px(x + 4 - i, y + i, r, g, b);
    wk_px(x + 4, y, 255, 240, 200);      /* tip */
}

static void fleet_glyph_circle(int x, int y, bool fill, uint8_t r,
                               uint8_t g, uint8_t b)
{
    static const uint8_t O[5] = { 0x0E, 0x11, 0x11, 0x11, 0x0E };
    static const uint8_t F[5] = { 0x0E, 0x1F, 0x1F, 0x1F, 0x0E };
    const uint8_t *G = fill ? F : O;
    for (int ry = 0; ry < 5; ry++)
        for (int rx = 0; rx < 5; rx++)
            if (G[ry] & (0x10 >> rx)) wk_px(x + rx, y + ry, r, g, b);
}

static void fleet_indicator(int x, int y)
{
    int role = 0, fresh = 0, stale = 0;
    uint32_t aage = 0xFFFFFFFFu;
    whm_sync_brief(&role, &fresh, &stale, &aage);
    bool lead = (role >= 1);   /* 1 = elected anchor, 2 = conductor */
    if (lead) {
        uint8_t r = 120, g = 130, b = 150;      /* solo: neutral */
        if (fresh > 0 && stale == 0) { r = 60; g = 220; b = 120; }
        else if (stale > 0) { r = 240; g = 190; b = 70; }
        if (role == 2)
            fleet_glyph_baton(x, y, r, g, b);
        else
            fleet_glyph_anchor(x, y, r, g, b);
        int known = fresh + stale;          /* digit = peers known;
                                               color carries health -
                                               amber 0 was a lie */
        char d[2] = { (char)('0' + (known > 9 ? 9 : known)), 0 };
        ota_text(d, x + 1, y + 7, r, g, b);   /* stacked: 5-char
                                                 size-2 names reach
                                                 x~7; the column
                                                 below stays free */
    } else {
        if (aage < 8000)
            fleet_glyph_circle(x, y, true, 60, 220, 120);
        else if (aage < 20000)
            fleet_glyph_circle(x, y, false, 240, 190, 70);
        else {
            /* Prohibition slash, ISO 3864 canonical 10:30 -> 4:30
               (owner reversal after a live A/B on LEDs - canon
               won). ENDPOINTS TRIMMED q=1..3: the ring's corner
               cells are deliberately empty (that emptiness is
               what makes 5 px read as ROUND), and a full
               corner-to-corner diagonal filled exactly those
               cells - the O grew corners. Three inner pixels pass
               through without capping the ring. */
            fleet_glyph_circle(x, y, false, 240, 80, 80);
            for (int q = 1; q <= 3; q++)
                wk_px(x + q, y + q, 240, 80, 80);
        }
    }
}

static void scr_home(bool entering)
{
    static char name[17] = "WHM";
    if (entering || s_name_dirty) {
        name[0] = 0;
        whm_settings_get_str("node", name, sizeof(name));
        if (!name[0]) strlcpy(name, "WHM", sizeof(name));
        s_name_dirty = false;
    }
    chrome("", SCR_HOME);
    gfx_text_center(W / 2, 2, name, strlen(name) <= 5 ? 2 : 1, 255, 255, 255);
    fleet_indicator(1, 2);

    char buf[48], ip[20];
    bool trusted;
    ui_status_t hst = status_level(&trusted);
    struct tm hlt;
    if (wall_now(&hlt, NULL)) {
        uint8_t mo = (uint8_t)(hlt.tm_mon + 1), d = (uint8_t)hlt.tm_mday;
        uint8_t h = (uint8_t)hlt.tm_hour, mi = (uint8_t)hlt.tm_min;
        /* conventional status colors: green = disciplined time (SNTP or
           flock), yellow = acquiring, red = link trouble, dim slate =
           off-grid free-run. Colon blinks on the shared phase. */
        uint8_t cr = hst == UST_RED ? 235 : hst == UST_YELLOW ? 235
                     : trusted ? 110 : 120;
        uint8_t cg = hst == UST_RED ? 60 : hst == UST_YELLOW ? 190
                     : trusted ? 225 : 120;
        uint8_t cb = hst == UST_RED ? 50 : hst == UST_YELLOW ? 40
                     : trusted ? 130 : 140;
        int16_t x0 = (int16_t)((W - 59) / 2);
        draw_hhmm(x0, 23, 1, h, mi, colon_phase_on(), cr, cg, cb);
        snprintf(buf, sizeof(buf), "%02u-%02u", (unsigned)mo, (unsigned)d);
        gfx_text((int16_t)(x0 + 30), 23, buf, 1, cr, cg, cb);
    } else {
        gfx_text_center(W / 2, 23, "--:--", 1, 90, 90, 110);
    }

    if (whm_wifi_ip_str(ip, sizeof(ip)) == ESP_OK && ip[0] && strcmp(ip, "0.0.0.0")) {
        marquee_m(1, MQ_BOUNCE, 0, 34, W, ip, 1, 90, 200, 90);
    } else {
        snprintf(buf, sizeof(buf), "%s %s", whm_wifi_is_sta() ? "->" : "AP",
                 whm_wifi_ssid());
        marquee_m(2, MQ_BOUNCE, 0, 34, W, buf, 1, 200, 160, 60);
    }

    bool imu, env, rtc;
    whm_sensors_status(&imu, &env, &rtc);
    health_dot(3, "SD", whm_storage_mounted());
    health_dot(22, "RTC", rtc);
    health_dot(43, "IMU", imu);
    health_dot(60, "T", env);
}

/* Colon blink phase from the DISCIPLINED system clock (SNTP now, the TSF
 * show clock at P2): 2s period, an edge every second. Synced units blink in
 * lockstep; an unsynced unit visibly free-runs against its neighbors - the
 * blink IS the fleet sync gauge. */
static bool colon_phase_on(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec & 1) == 0;
}

/* HH [blink] MM with tabular digits; advance = 26*scale, width 25*scale */
static void draw_hhmm(int16_t x, uint16_t y, uint8_t sc, uint8_t h, uint8_t m,
                      bool colon_on, uint8_t r, uint8_t g, uint8_t b)
{
    char d[12];
    snprintf(d, sizeof(d), "%02u", (unsigned)h);
    gfx_text(x, y, d, sc, r, g, b);
    x += (int16_t)(12 * sc);
    if (colon_on) {
        whm_display_fill_rect((uint16_t)x, y + 2 * sc, sc, sc, r, g, b);
        whm_display_fill_rect((uint16_t)x, y + 5 * sc, sc, sc, r, g, b);
    }
    x += (int16_t)(2 * sc);
    snprintf(d, sizeof(d), "%02u", (unsigned)m);
    gfx_text(x, y, d, sc, r, g, b);
}

static const char *k_dow[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

static void scr_clock(void)
{
    /* dedicated clock face: no title, just time. The old corner NTP dot
       is gone - the global 2px top bar alerts when something is wrong,
       and monotone means monotone. */
    /* page dots only */
    uint16_t total_w = SCR_COUNT * 4 - 2;
    uint16_t x0 = (W - total_w) / 2;
    for (int i = 0; i < SCR_COUNT; i++) {
        uint8_t v = (i == SCR_CLOCK) ? 200 : 40;
        whm_display_fill_rect(x0 + i * 4, H - 2, 2, 2, v, v, v);
    }

    char buf[24];
    struct tm lt;
    suseconds_t us;
    if (!wall_now(&lt, &us)) {
        gfx_text_center(W / 2, 26, "NO TIME", 1, C_BAD);
        return;
    }
    uint8_t h = (uint8_t)lt.tm_hour, mi = (uint8_t)lt.tm_min,
            s = (uint8_t)lt.tm_sec;
    {   /* 12/24-hour, serial-toggled ('clock 12|24'), cached 3 s
           like the label. 12-mode is bare wall-clock convention:
           hour%12 with 0 -> 12, no AM/PM - no clock face on a
           wall carries one. */
        static uint8_t clk24 = 1;
        static int64_t clk_t = 0;
        if (esp_timer_get_time() - clk_t > 3000000) {
            clk24 = 1;
            whm_settings_get_u8("clk24", &clk24);
            clk_t = esp_timer_get_time();
        }
        if (!clk24) {
            h = (uint8_t)(h % 12);
            if (h == 0) h = 12;
        }
    }
    uint8_t mo = (uint8_t)(lt.tm_mon + 1), d = (uint8_t)lt.tm_mday;
    uint16_t y = (uint16_t)(lt.tm_year + 1900);
    (void)y;
    /* label above the face: custom string, tz name, or off */
    {
        static char lbl[16];
        static int64_t lbl_t = 0;
        if (esp_timer_get_time() - lbl_t > 3000000) {   /* refresh 3s */
            lbl[0] = 0;
            whm_settings_get_str("clk_lbl", lbl, sizeof(lbl));
            if (!lbl[0]) strlcpy(lbl, whm_timesync_tz_label(), sizeof(lbl));
            lbl_t = esp_timer_get_time();
        }
        if (lbl[0] && lbl[0] != '-') {
            gfx_text_center(W / 2, 1, lbl, 1, 135, 135, 160);
        }
    }
    bool synced = (whm_timesync_state() == WHM_TS_SYNCED) ||
                  whm_sync_wall_from_flock();
    uint8_t tr = synced ? 255 : 130, tg = synced ? 245 : 130,
            tb = synced ? 220 : 150;

    /* big time; colon phase-locked to the disciplined clock (sync gauge) */
    draw_hhmm((int16_t)(W / 2 - 25), 12, 2, h, mi, colon_phase_on(),
              tr, tg, tb);

    /* seconds bar: 60px wide, 2px tall, exactly one pixel per second
       (second 0 lights pixel 1; full at :59; clears on the minute) */
    /* lit-pixel count == seconds: :01 -> 1px ... :59 -> 59px, and the
       top of the minute displays as 60 (full) with a ~100ms brightness
       pop - phase-locked via gettimeofday like the colon, so synced
       units flash together */
    uint32_t shown = (s == 0) ? 60 : (uint32_t)s;
    bool flash = (s == 0) && us < 100000;
    bar(2, 33, 60, 2, shown, 60,
        flash ? 255 : (synced ? 215 : 130),
        flash ? 255 : (synced ? 215 : 130),
        flash ? 255 : (synced ? 225 : 150));
    /* quarter notches ride the unfilled track as a ruler */
    for (int q = 15; q < 60; q += 15) {
        if ((uint32_t)q >= shown) {
            whm_display_fill_rect((uint16_t)(2 + q), 33, 1, 2, 30, 30, 48);
        }
    }

    /* day + date */
    uint16_t yy2; uint8_t dow = 0;
    (void)yy2;
    /* derive dow from the calendar rather than trusting a stale register */
    {
        static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
        unsigned yr = y - (mo < 3 ? 1 : 0);
        dow = (uint8_t)((yr + yr / 4 - yr / 100 + yr / 400 + t[mo - 1] + d) % 7);
    }
    snprintf(buf, sizeof(buf), "%s %02u-%02u", k_dow[dow], (unsigned)mo,
             (unsigned)d);
    gfx_text_center(W / 2, 40, buf, 1, 140, 170, 210);
    snprintf(buf, sizeof(buf), "%04u", (unsigned)y);
    gfx_text_center(W / 2, 51, buf, 1, 90, 100, 130);
}

static void scr_timer(void)
{
    chrome("TIMER", SCR_TIMER);
    uint32_t rem, tot;
    int st;
    whm_timer_status(&rem, &tot, &st);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu",
             (unsigned long)(rem / 60), (unsigned long)(rem % 60));

    bool flash_off = (st == (int)T_DONE) &&
                     ((esp_timer_get_time() / 400000) & 1);
    if (!flash_off) {
        if (st == (int)T_DONE)      gfx_text_center(W / 2, 14, buf, 2, 255, 60, 40);
        else if (st == (int)T_RUN)  gfx_text_center(W / 2, 14, buf, 2, 255, 255, 255);
        else if (st == (int)T_PAUSE) gfx_text_center(W / 2, 14, buf, 2, 255, 190, 60);
        else                         gfx_text_center(W / 2, 14, buf, 2, 130, 130, 150);
    }

    bar(4, 35, W - 8, 3, rem, tot,
        st == (int)T_DONE ? 255 : 70,
        st == (int)T_DONE ? 60 : 255,
        st == (int)T_DONE ? 40 : 70);

    const char *hint = st == (int)T_RUN ? "2TAP PAUSE"
                     : st == (int)T_PAUSE ? "2TAP GO"
                     : st == (int)T_DONE ? "2TAP STOP"
                                          : "2TAP START";
    gfx_text_center(W / 2, 45, hint, 1, C_HINT);
    if (st == (int)T_RUN || st == (int)T_PAUSE) {
        gfx_text_center(W / 2, 54, "HOLD RESET", 1, 70, 70, 90);
    }
}

static void scr_watch(void)
{
    chrome("STOPWATCH", SCR_WATCH);
    bool run;
    int64_t el = whm_watch_elapsed_us(&run);
    uint32_t cs = (uint32_t)(el / 10000);           /* centiseconds */
    uint32_t mins = cs / 6000, secs = (cs / 100) % 60;
    char buf[16];

    if (mins < 100) {
        snprintf(buf, sizeof(buf), "%02lu:%02lu",
                 (unsigned long)mins, (unsigned long)secs);
        gfx_text_center(W / 2, 14, buf, 2,
                        run ? 255 : 180, run ? 255 : 180, run ? 255 : 200);
    } else {
        snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu",
                 (unsigned long)(mins / 60), (unsigned long)(mins % 60),
                 (unsigned long)secs);
        gfx_text_center(W / 2, 18, buf, 1, 255, 255, 255);
    }

    /* sub-second sweep: smooth motion instead of jittery digits */
    if (run) {
        bar(4, 35, W - 8, 2, (uint32_t)((el / 1000) % 1000), 1000,
            110, 200, 255);
    } else if (el > 0) {
        bar(4, 35, W - 8, 2, 1, 1, 255, 200, 70);
    } else {
        bar(4, 35, W - 8, 2, 0, 1, 0, 0, 0);
    }

    const char *hint = run ? "2TAP STOP"
                     : el > 0 ? "2TAP GO"
                              : "2TAP START";
    gfx_text_center(W / 2, 45, hint, 1, C_HINT);
    if (el > 0) gfx_text_center(W / 2, 54, "HOLD RESET", 1, 70, 70, 90);
}

static uint32_t marq_read_ms(const char *s, uint8_t sc, uint16_t w)
{
    uint16_t tw = gfx_text_width(s, sc);
    if (tw <= w) return 4000;
    return 1200 + (uint32_t)(tw - w) * 45 + 1200 + 500;
}

/* alternate title/artist, but let an overflowing string finish its
   full marquee read before rotating - no more mid-scroll guillotine */
static const char *music_meta_pick(const char *t, const char *a,
                                   uint8_t sc, uint16_t w)
{
    static uint8_t which = 0;
    static int64_t since = 0;
    if (!a[0]) return t;
    const char *cur = which ? a : t;
    uint32_t need = marq_read_ms(cur, sc, w);
    if (esp_timer_get_time() - since > (int64_t)need * 1000) {
        which ^= 1;
        since = esp_timer_get_time();
        cur = which ? a : t;
    }
    return cur;
}

static uint8_t s_mus_lay = 0xFF, s_mus_ovl = 0xFF;
static void mus_cfg_load(void)
{
    if (s_mus_lay == 0xFF) {
        uint8_t v = 0;
        s_mus_lay = whm_settings_get_u8("mus_lay", &v) == ESP_OK ? v : 0;
        v = 1;
        whm_settings_get_u8("mus_ovl", &v);
        s_mus_ovl = v ? 1 : 0;
    }
}

void whm_ui_music_layout(uint8_t lay, int8_t ovl)
{
    mus_cfg_load();
    if (lay <= 2) {
        s_mus_lay = lay;
        whm_settings_set_u8("mus_lay", lay);
    }
    if (ovl >= 0) {
        s_mus_ovl = ovl ? 1 : 0;
        whm_settings_set_u8("mus_ovl", (uint8_t)(ovl ? 1 : 0));
    }
}

static void music_sync_line(void)
{
    int64_t e = whm_mp3_sync_err_us();
    if (e == INT64_MIN) return;
    char b[20];
    long ms10 = (long)(e / 100);            /* 0.1ms units */
    float ppm = whm_mp3_sync_ppm();
    if (whm_mp3_is_master()) {
        snprintf(b, sizeof(b), "M free-run");
    } else {
        snprintf(b, sizeof(b), "S%+ld.%01ld %+dp", ms10 / 10,
                 labs(ms10) % 10, (int)ppm);
    }
    bool tight = e > -1000 && e < 1000;
    gfx_text(1, 57, b, 1, tight ? 80 : 235, tight ? 220 : 190,
             tight ? 100 : 60);
}

static void scr_music(void)
{
    mus_cfg_load();
    const uint8_t *art = whm_mp3_art();
    int idx;
    whm_mp3_state_t st = whm_mp3_state(&idx);
    char title[48], artist[48];
    if (idx >= 0) {
        whm_mp3_track_info(idx, title, sizeof(title), artist,
                           sizeof(artist));
    } else {
        title[0] = artist[0] = 0;
    }
    uint32_t pos, dur;
    whm_mp3_progress(&pos, &dur);

    if (s_mus_lay == 2) {                    /* INFO layout: no art */
        whm_display_fill(6, 6, 12);
        if (idx >= 0) {
            marquee(3, 1, 6, W - 2, title, 2, 235, 235, 245);
            if (artist[0]) {
                marquee(0, 1, 26, W - 2, artist, 1, 160, 165, 200);
            }
            bar(2, 40, W - 4, 4, pos, dur ? dur : 1,
                st == WHM_MP3_PLAYING ? 70 : 255,
                st == WHM_MP3_PLAYING ? 255 : 190,
                st == WHM_MP3_PLAYING ? 110 : 40);
            char tb[36];
            snprintf(tb, sizeof(tb), "%lu:%02lu / %lu:%02lu",
                     (unsigned long)(pos / 60), (unsigned long)(pos % 60),
                     (unsigned long)(dur / 60), (unsigned long)(dur % 60));
            gfx_text_center(W / 2, 48, tb, 1, 150, 155, 180);
        } else {
            gfx_text_center(W / 2, 24, "NO TRACK", 1, C_DIM);
        }
        music_sync_line();
        return;
    }

    bool artlay = (s_mus_lay == 1);
    uint16_t art_h = artlay ? H : 49;
    if (art) {
        whm_display_blit_rgb888(0, 0, W, art_h > W ? W : art_h,
                                (const uint8_t *)art);
        if (st == WHM_MP3_PAUSED) {
            for (uint16_t y = 0; y < art_h; y++) {
                for (uint16_t x = (uint16_t)(y & 1); x < W; x += 2) {
                    whm_display_set_pixel(x, y, 0, 0, 0);
                }
            }
        }
    } else {
        whm_display_fill(6, 6, 12);
        gfx_text_center(W / 2, 14, "MUSIC", 2, 200, 200, 220);
        gfx_text_center(W / 2, 34, "put .mp3 in", 1, C_DIM);
        gfx_text_center(W / 2, 43, "/sdcard/media", 1, C_DIM);
    }

    bool strip = !artlay || s_mus_ovl;
    if (strip) {
        whm_display_fill_rect(0, 49, W, 15, 6, 6, 12);
        if (idx >= 0) {
            const char *m = music_meta_pick(title, artist, 1, W - 2);
            marquee(3, 1, 50, W - 2, m, 1, 230, 230, 240);
            bar(2, 60, W - 4, 2, pos, dur ? dur : 1,
                st == WHM_MP3_PLAYING ? 70 : 255,
                st == WHM_MP3_PLAYING ? 255 : 190,
                st == WHM_MP3_PLAYING ? 110 : 40);
        } else {
            gfx_text_center(W / 2, 52, "2TAP PLAY", 1, C_HINT);
        }
    }
    music_sync_line();
}

typedef enum { OR_IDLE, OR_CONSULT, OR_REVEAL } or_state_t;
static or_state_t s_or_state = OR_IDLE;
static int64_t s_or_t0 = 0, s_or_last_shake = 0;
static int s_or_ans = -1, s_or_hits = 0;
static int64_t s_or_hit_t0 = 0;
static float s_or_base = 0.0f;       /* self-learned rest |a| - the
                                        old detector assumed rest =
                                        1.0 (g) while the driver
                                        speaks m/s2: gravity itself
                                        read as prophecy, 60x/s */
static int64_t s_or_quiet_t0 = 0;
static bool s_or_armed = true;

static const char *k_oracle[] = {
    "IT IS CERTAIN", "WITHOUT A DOUBT", "YES DEFINITELY",
    "SIGNS POINT TO YES", "OUTLOOK GOOD", "MOST LIKELY",
    "ASK AGAIN LATER", "REPLY HAZY TRY AGAIN", "CANNOT PREDICT NOW",
    "BETTER NOT TELL YOU", "DONT COUNT ON IT", "MY REPLY IS NO",
    "OUTLOOK NOT SO GOOD", "VERY DOUBTFUL", "SHIP IT",
    "REFLASH AND RETRY", "GREP THE LOGS", "42", "73",
    "CHECK YOUR SOLDER", "PACKET LOST ASK AGAIN", "TSF SAYS YES",
    "RACE CONDITION", "ITS A FEATURE", "REBOOT THE UNIVERSE",
};
#define ORACLE_N (sizeof(k_oracle) / sizeof(k_oracle[0]))

static void oracle_consult(float mag)
{
    int64_t gnow = esp_timer_get_time();
    if (s_or_state == OR_CONSULT) return;          /* mid-gather */
    if (s_or_state == OR_REVEAL &&
        gnow - s_or_t0 < 1800000) return;          /* let it land */
    int pick = (int)((esp_random() ^ (uint32_t)(mag * 1000.0f)) % ORACLE_N);
    if (pick == s_or_ans) pick = (pick + 1) % ORACLE_N;
    s_or_ans = pick;
    s_or_state = OR_CONSULT;
    s_or_t0 = esp_timer_get_time();
    whm_audio_tone_async(392, 500);
}

void whm_ui_oracle_ask(void)
{
    s_or_last_shake = esp_timer_get_time();
    oracle_consult(1.7f);
}

static void oracle_tap(void) { whm_ui_oracle_ask(); }

/* -------- the oracle v2: it speaks Ignition -------------------------
 * Fourteen hue-cycling sparks spiral inward as the hum builds; one
 * ring blooms outward on the ding, birthing the answer, which
 * dissolves in exactly like the boot name - two-line split for long
 * prophecies, scarf-orange underline beneath. Consults are STATE-
 * GATED: mid-theater shakes are ignored; a fresh shake after the
 * answer settles is the ritual re-ask. */
static void orc_split2(const char *s, char l1[20], char l2[20])
{
    l2[0] = 0;
    if (gfx_text_width(s, 1) <= 62) {
        strlcpy(l1, s, 20);
        return;
    }
    int n = (int)strlen(s), best = -1, bestw = 999;
    for (int i2 = 1; i2 < n - 1; i2++) {
        if (s[i2] != ' ') continue;
        char a[20], b[20];
        size_t la = (size_t)i2 + 1;
        strlcpy(a, s, la < 20 ? la : 20);
        strlcpy(b, s + i2 + 1, 20);
        int w1 = gfx_text_width(a, 1), w2 = gfx_text_width(b, 1);
        int w = w1 > w2 ? w1 : w2;
        if (w < bestw) { bestw = w; best = i2; }
    }
    if (best < 0) { strlcpy(l1, s, 20); return; }
    size_t la = (size_t)best + 1;
    strlcpy(l1, s, la < 20 ? la : 20);
    strlcpy(l2, s + best + 1, 20);
}

static void scr_oracle(void)
{
    chrome("", SCR_ORACLE);
    int64_t now = esp_timer_get_time();
    float t = (float)now / 1e6f;
    int cx = 32, cy = 27;

    /* shake detect, state-gated */
    float ax, ay, az, gx, gy, gz;
    if (whm_sensors_read_imu(&ax, &ay, &az, &gx, &gy, &gz) == ESP_OK) {
        float mag = sqrtf(ax * ax + ay * ay + az * az);
        if (s_or_base <= 0.01f) s_or_base = mag;   /* seed on entry */
        s_or_base += 0.02f * (mag - s_or_base);    /* slow gravity LP */
        float dev = fabsf(mag - s_or_base);
        float th = 0.45f * (s_or_base > 0.1f ? s_or_base : 1.0f);
        if (!s_or_armed) {                 /* quiet-rearm: must rest
                                              before counting again */
            if (dev < th * 0.3f) {
                if (!s_or_quiet_t0) s_or_quiet_t0 = now;
                if (now - s_or_quiet_t0 > 400000) {
                    s_or_armed = true;
                    s_or_hits = 0;
                }
            } else {
                s_or_quiet_t0 = 0;
            }
        } else if (dev > th) {
            if (now - s_or_hit_t0 > 600000) s_or_hits = 0;
            if (!s_or_hits) s_or_hit_t0 = now;
            s_or_hits++;
            if (s_or_hits >= 3 &&
                now - s_or_last_shake > 3500000) {
                s_or_last_shake = now;
                s_or_hits = 0;
                s_or_armed = false;
                s_or_quiet_t0 = 0;
                oracle_consult(dev / th);  /* guard inside */
            }
        }
    }

    if (s_or_state == OR_IDLE) {          /* quiet stage, drifting sparks */
        for (int i2 = 0; i2 < 3; i2++) {
            float ang = t * 0.8f + (float)i2 * 2.094f;
            int px = cx + (int)lroundf(cosf(ang) * 22.0f);
            int py = cy + (int)lroundf(sinf(ang) * 18.0f);
            uint8_t r, g, b;
            hsv_rgb((uint16_t)((i2 * 120 + (int)(t * 30)) % 360), 200,
                    70, &r, &g, &b);
            if (px >= 0 && px < W && py >= 0 && py < H) {
                whm_display_set_pixel((uint16_t)px, (uint16_t)py,
                                      r, g, b);
            }
        }
        gfx_text_center(W / 2, 50, "SHAKE ME", 1, 40, 40, 62);
        return;
    }

    if (s_or_state == OR_CONSULT) {
        int e = (int)((now - s_or_t0) / 1000);
        if (e >= 900) {                   /* the ding births the bloom */
            s_or_state = OR_REVEAL;
            s_or_t0 = now;
            whm_audio_tone_async(1318, 90);
            e = 0;
        } else {
            float p = (float)e / 900.0f;
            float pr = p * 1.15f;
            if (pr > 1.0f) pr = 1.0f;
            for (int i2 = 0; i2 < 14; i2++) {   /* the gathering */
                float ang = (float)i2 * 2.399f + t * 3.2f;
                float rr = 2.0f + 28.0f * (1.0f - pr);
                int px = cx + (int)lroundf(cosf(ang) * rr);
                int py = cy + (int)lroundf(sinf(ang) * rr * 0.9f);
                uint8_t r, g, b;
                hsv_rgb((uint16_t)((i2 * 26 + e / 6) % 360), 235,
                        (uint8_t)(90.0f + 165.0f * p), &r, &g, &b);
                if (px >= 0 && px < W && py >= 0 && py < H) {
                    whm_display_set_pixel((uint16_t)px, (uint16_t)py,
                                          r, g, b);
                }
            }
            uint8_t gv = (uint8_t)(p * 220.0f);
            whm_display_set_pixel(cx, cy, gv, gv, (uint8_t)(gv / 2));
            whm_display_set_pixel(cx + 1, cy, gv / 2, gv / 2, 20);
            whm_display_set_pixel(cx - 1, cy, gv / 2, gv / 2, 20);
            whm_display_set_pixel(cx, cy + 1, gv / 2, gv / 2, 20);
            whm_display_set_pixel(cx, cy - 1, gv / 2, gv / 2, 20);
            return;
        }
    }

    /* OR_REVEAL: bloom, dissolve, settle */
    int e2 = (int)((now - s_or_t0) / 1000);
    float rr = (float)e2 * 0.05f;
    int rv = 235 - (int)(rr * 6.5f);
    if (rv >= 8) {
        uint8_t r, g, b;
        hsv_rgb((uint16_t)(((200 - e2 / 12) % 360 + 360) % 360), 230,
                (uint8_t)rv, &r, &g, &b);
        for (int a2 = 0; a2 < 72; a2++) {
            float th = (float)a2 * 0.0873f;
            int px = cx + (int)lroundf(cosf(th) * rr);
            int py = cy + (int)lroundf(sinf(th) * rr * 0.9f);
            if (px >= 0 && px < W && py >= 0 && py < H) {
                whm_display_set_pixel((uint16_t)px, (uint16_t)py,
                                      r, g, b);
            }
        }
    }
    int av = (e2 - 120) * 255 / 350;
    if (av < 0) av = 0;
    if (av > 255) av = 255;
    const char *ans = k_oracle[s_or_ans];
    char l1[20], l2[20];
    uint8_t ar = (uint8_t)(213 * av / 255);
    uint8_t ag = (uint8_t)(194 * av / 255);
    uint8_t ab = (uint8_t)(167 * av / 255);
    uint16_t tw;
    int uy;
    if (gfx_text_width(ans, 2) <= 62) {
        gfx_text_center(W / 2, 21, ans, 2, ar, ag, ab);
        tw = gfx_text_width(ans, 2);
        uy = 21 + 16 + 2;
    } else {
        orc_split2(ans, l1, l2);
        if (l2[0]) {
            gfx_text_center(W / 2, 19, l1, 1, ar, ag, ab);
            gfx_text_center(W / 2, 29, l2, 1, ar, ag, ab);
            uint16_t w1 = gfx_text_width(l1, 1);
            uint16_t w2 = gfx_text_width(l2, 1);
            tw = w1 > w2 ? w1 : w2;
            uy = 29 + 8 + 2;
        } else {
            gfx_text_center(W / 2, 24, l1, 1, ar, ag, ab);
            tw = gfx_text_width(l1, 1);
            uy = 24 + 8 + 2;
        }
    }
    if (e2 >= 900) {                      /* scarf-orange underline */
        int ux = (e2 - 900) * (int)tw / 300;
        if (ux > (int)tw) ux = (int)tw;
        whm_display_fill_rect((uint16_t)((W - tw) / 2), (uint16_t)uy,
                              (uint16_t)ux, 2, 150, 45, 8);
    }
    if (e2 > 1400) {                      /* settled: sparks return */
        for (int i2 = 0; i2 < 3; i2++) {
            float ang = t * 0.8f + (float)i2 * 2.094f;
            int px = cx + (int)lroundf(cosf(ang) * 26.0f);
            int py = cy + (int)lroundf(sinf(ang) * 20.0f);
            uint8_t r, g, b;
            hsv_rgb((uint16_t)((i2 * 120 + (int)(t * 30)) % 360), 200,
                    70, &r, &g, &b);
            if (px >= 0 && px < W && py >= 0 && py < H &&
                (py < 15 || py > 40)) {
                whm_display_set_pixel((uint16_t)px, (uint16_t)py,
                                      r, g, b);
            }
        }
    }
}

static void net_flock_line(uint16_t yy)
{
    whm_sync_role_t r = whm_sync_role();
    char l[28];
    if (r == WHM_SYNC_MEMBER) {
        int64_t age = whm_sync_conductor_age_us();
        bool ok = age >= 0 && age < 10000000LL;
        snprintf(l, sizeof(l), "flock:%s %s", whm_sync_anchor_name(),
                 ok ? "ok" : "lost");
        gfx_text(2, yy, l, 1, ok ? 90 : 235, ok ? 220 : 80,
                 ok ? 110 : 60);
    } else if (r == WHM_SYNC_CONDUCTOR) {
        int stn = whm_wifi_ap_stations();
        snprintf(l, sizeof(l), "flock:%d joined", stn < 0 ? 0 : stn);
        gfx_text(2, yy, l, 1, 130, 190, 220);
    }
}

static void scr_level(void)
{
    static float fx = 0, fy = 0;
    chrome("LEVEL", SCR_LEVEL);

    float ax, ay, az, gx, gy, gz;
    if (whm_sensors_read_imu(&ax, &ay, &az, &gx, &gy, &gz) != ESP_OK) {
        gfx_text_center(W / 2, 26, "NO IMU", 1, C_BAD);
        return;
    }
    /* lowpass; flip signs to taste on the bench if bubble feels inverted */
    fx += 0.25f * (ax - fx);
    fy += 0.25f * (ay - fy);

    int cx = W / 2, cy = H / 2 + 2;
    /* rings + crosshair */
    for (int r = 8; r <= 24; r += 8) {
        whm_display_fill_rect(cx - r, cy - r, 2 * r, 1, 35, 35, 55);
        whm_display_fill_rect(cx - r, cy + r, 2 * r + 1, 1, 35, 35, 55);
        whm_display_fill_rect(cx - r, cy - r, 1, 2 * r, 35, 35, 55);
        whm_display_fill_rect(cx + r, cy - r, 1, 2 * r + 1, 35, 35, 55);
    }
    whm_display_fill_rect(cx - 26, cy, 53, 1, 60, 60, 90);
    whm_display_fill_rect(cx, cy - 26, 1, 53, 60, 60, 90);

    /* axes swapped per bench report 2026-09-06: panel-X responds to the
       IMU's Y and vice versa. Signs verified separately; flip here if the
       bubble still runs the wrong way on either axis. */
    /* bench 2026-09-07: X correct after the swap, Y inverted - negate.
       Readouts flipped to agree with the bubble's directions. */
    float k = 24.0f / 9.81f;
    int dx = (int)(fy * k), dy = -(int)(fx * k);
    if (dx > 24) dx = 24;
    if (dx < -24) dx = -24;
    if (dy > 24) dy = 24;
    if (dy < -24) dy = -24;
    bool centered = (abs(dx) <= 2 && abs(dy) <= 2);
    whm_display_fill_rect(cx + dx - 1, cy + dy - 1, 3, 3,
                          centered ? 40 : 255, centered ? 255 : 180, 40);

    char buf[16];
    float pitch = -atan2f(fx, fabsf(az) + 0.01f) * 57.296f;
    float roll = atan2f(fy, fabsf(az) + 0.01f) * 57.296f;
    snprintf(buf, sizeof(buf), "P%+03d R%+03d", (int)pitch, (int)roll);
    gfx_text_center(W / 2, H - 11, buf, 1, 100, 100, 120);
}

static void scr_env(void)
{
    chrome("ENV", SCR_ENV);
    float t, rh;
    char buf[16];
    if (whm_sensors_read_th(&t, &rh) != ESP_OK) {
        gfx_text_center(W / 2, 26, "NO SHTC3", 1, C_BAD);
        return;
    }
    /* display hysteresis on top of the sensor-side filter: the shown value
       only moves once the filtered value clears the deadband, so digits
       never oscillate on a rounding boundary */
    static bool sh_init = false;
    static float sh_t, sh_rh;
    if (!sh_init) { sh_t = t; sh_rh = rh; sh_init = true; }
    if (fabsf(t - sh_t) >= 0.15f) sh_t = t;
    if (fabsf(rh - sh_rh) >= 0.8f) sh_rh = rh;

    snprintf(buf, sizeof(buf), "%.1fC", sh_t);
    gfx_text_center(W / 2, 14, buf, 2, 255, 180, 60);
    bar(4, 39, 56, 4, (uint32_t)(sh_rh * 10.0f), 1000, 70, 170, 255);
    snprintf(buf, sizeof(buf), "%.0f%%RH", sh_rh);
    gfx_text_center(W / 2, 48, buf, 1, 120, 170, 255);
}

static void scr_net(void)
{
    chrome("NET", SCR_NET);
    char buf[48], ip[20] = "";
    bool sta = whm_wifi_is_sta();

    snprintf(buf, sizeof(buf), "%s %s", sta ? "STA" : "AP", whm_wifi_ssid());
    marquee(4, 1, 11, W - 2, buf, 1, 200, 200, 200);

    if (whm_wifi_ip_str(ip, sizeof(ip)) == ESP_OK && ip[0] && strcmp(ip, "0.0.0.0")) {
        marquee_m(5, MQ_BOUNCE, 1, 21, W - 2, ip, 1, 90, 200, 90);
    } else {
        gfx_text(1, 21, sta ? "connecting" : "no ip", 1, 200, 160, 60);
    }

    if (sta) {
        int rssi = whm_wifi_rssi();
        if (rssi != 127) {
            int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
            for (int i = 0; i < 4; i++) {
                uint8_t on = i < bars;
                whm_display_fill_rect(2 + i * 5, 40 - i * 3, 3, 3 + i * 3,
                                      on ? 60 : 25, on ? 220 : 25, on ? 60 : 25);
            }
            snprintf(buf, sizeof(buf), "%ddBm", rssi);
            gfx_text(26, 33, buf, 1, 150, 150, 150);
        }
    } else {
        int n = whm_wifi_ap_stations();
        snprintf(buf, sizeof(buf), "%d joined", n < 0 ? 0 : n);
        gfx_text(1, 33, buf, 1, 150, 150, 150);
    }

    double ppm;
    if (whm_wifi_drift(&ppm)) {
        snprintf(buf, sizeof(buf), "TSF%+.1fppm", ppm);
        gfx_text(1, 50, buf, 1, 160, 120, 220);
    } else {
        gfx_text(1, 50, "TSF --", 1, 90, 90, 110);
    }
    net_flock_line(54);
}

static void scr_sys(void)
{
    chrome("SYS", SCR_SYS);
    char buf[24], up[16];
    gfx_text_center(W / 2, 11, "WHM " WHM_VERSION, 1, 255, 255, 255);

    snprintf(buf, sizeof(buf), "HEAP %uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    gfx_text(1, 23, buf, 1, 90, 200, 90);
    snprintf(buf, sizeof(buf), "PSRM %uK",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    gfx_text(1, 33, buf, 1, 90, 170, 220);

    fmt_uptime(up, sizeof(up));
    snprintf(buf, sizeof(buf), "UP %s", up);
    gfx_text(1, 43, buf, 1, 200, 160, 60);
    gfx_text(1, 53, "IDF 5.5.2", 1, C_HINT);
}

static void scr_sd(void)
{
    chrome("SD", SCR_SD);
    char buf[24];

    if (whm_storage_maintenance()) {
        gfx_text_center(W / 2, 16, "FORMATTING", 1, 255, 190, 60);
        gfx_text_center(W / 2, 27, "do not", 1, 200, 120, 60);
        gfx_text_center(W / 2, 36, "power off", 1, 200, 120, 60);
        /* indeterminate sweep */
        uint32_t ph = (uint32_t)((esp_timer_get_time() / 30000) % (W - 8));
        whm_display_fill_rect(4, 48, W - 8, 3, BAR_TRACK);
        uint16_t bx = 4 + (uint16_t)ph;
        uint16_t bw = bx + 10 > (W - 4) ? (W - 4) - bx : 10;
        whm_display_fill_rect(bx, 48, bw, 3, 255, 190, 60);
        return;
    }
    if (!whm_storage_mounted()) {
        gfx_text_center(W / 2, 24, "NO CARD", 1, C_BAD);
        gfx_text_center(W / 2, 36, "insert to mount", 1, C_DIM);
        return;
    }
    /* f_getfree can walk the FAT - poll it gently, not at frame rate */
    static int64_t last_us = 0;
    static uint32_t c_total = 0, c_free = 0;
    static bool have = false;
    int64_t now = esp_timer_get_time();
    if (!have || now - last_us > 2000000LL) {
        uint32_t t, f;
        if (whm_storage_stats(&t, &f) == ESP_OK) {
            c_total = t; c_free = f; have = true;
        }
        last_us = now;
    }
    if (have) {
        gfx_text_center(W / 2, 13, "CARD OK", 1, C_GOOD);
        snprintf(buf, sizeof(buf), "%luM tot", (unsigned long)c_total);
        gfx_text(4, 26, buf, 1, 200, 200, 200);
        snprintf(buf, sizeof(buf), "%luM free", (unsigned long)c_free);
        gfx_text(4, 36, buf, 1, 200, 200, 200);
        bar(4, 48, 56, 4, c_total - c_free, c_total ? c_total : 1,
            255, 170, 70);
    } else {
        gfx_text_center(W / 2, 26, "reading...", 1, C_DIM);
    }
}

static void scr_vu(bool entering, bool leaving)
{
    static float pk_l = 0, pk_r = 0;
    if (entering) {
        if (whm_audio_mic_start() != ESP_OK) { /* drawn below as error */ }
        pk_l = pk_r = 0;
    }
    if (leaving) {
        whm_audio_mic_stop();
        return;
    }
    chrome("VU", SCR_VU);
    float l = 0, r = 0;
    if (!whm_audio_mic_active() || whm_audio_mic_read_rms(&l, &r) != ESP_OK) {
        gfx_text_center(W / 2, 26, "MIC ERR", 1, C_BAD);
        return;
    }
    /* raw rms -> dBFS -> 0..44 px (floor -60dB) */
    for (int ch = 0; ch < 2; ch++) {
        float rms = ch ? r : l;
        float db = 20.0f * log10f((rms + 1.0f) / 32768.0f);
        if (db < -60) db = -60;
        int hgt = (int)((db + 60.0f) * (44.0f / 60.0f));
        float *pk = ch ? &pk_r : &pk_l;
        if (hgt > *pk) *pk = (float)hgt; else *pk -= 0.6f;
        if (*pk < 0) *pk = 0;

        int x = ch ? 36 : 14;
        whm_display_fill_rect(x, 12, 14, 44, 18, 18, 30);
        for (int yy = 0; yy < hgt; yy++) {
            uint8_t rr = yy > 36 ? 255 : yy > 28 ? 255 : 40;
            uint8_t gg = yy > 36 ? 60 : 220;
            whm_display_fill_rect(x + 1, 12 + 44 - 1 - yy, 12, 1, rr, gg, 40);
        }
        int py = 12 + 44 - 1 - (int)*pk;
        whm_display_fill_rect(x + 1, py, 12, 1, 255, 255, 255);
    }
    gfx_text(10, H - 11, "L", 1, C_DIM);
    gfx_text(48, H - 11, "R", 1, C_DIM);
}

/* Conway's Life, age-colored */
static uint8_t *s_life_a = NULL, *s_life_b = NULL;
static int64_t s_life_seeded_at = 0;

static void life_seed(void)
{
    for (int i = 0; i < W * H; i++) {
        s_life_a[i] = (esp_random() % 100 < 22) ? 1 : 0;
    }
    s_life_seeded_at = esp_timer_get_time();
}

/* Fleet mode: both units must sow the SAME field for their window at the
 * same instant - deterministic xorshift seeded from (epoch, strip idx). */
static void life_seed_det(uint32_t epoch, uint8_t idx)
{
    uint32_t st = 0x9E3779B9u ^ (epoch * 2654435761u) ^
                  ((uint32_t)idx * 40503u);
    if (!st) st = 1;
    for (int i = 0; i < W * H; i++) {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        s_life_a[i] = ((st & 0xFF) < 56) ? 1 : 0;   /* ~22%% */
    }
    s_life_seeded_at = esp_timer_get_time();
}

/* One generation with optional ghost columns (fleet seams). ghostL/R are
 * neighbor edge ages or NULL for local torus wrap. */
static int life_step(const uint8_t *ghostL, const uint8_t *ghostR)
{
    int pop = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy) continue;
                    int xx = x + dx, yy = (y + dy + H) % H;
                    uint8_t v;
                    if (xx < 0) {
                        v = ghostL ? ghostL[yy]
                                   : s_life_a[yy * W + (W - 1)];
                    } else if (xx >= W) {
                        v = ghostR ? ghostR[yy] : s_life_a[yy * W];
                    } else {
                        v = s_life_a[yy * W + xx];
                    }
                    if (v) n++;
                }
            }
            uint8_t age = s_life_a[y * W + x];
            uint8_t nx = 0;
            if (age && (n == 2 || n == 3)) nx = age < 250 ? age + 1 : age;
            else if (!age && n == 3) nx = 1;
            s_life_b[y * W + x] = nx;
            if (nx) pop++;
        }
    }
    uint8_t *t = s_life_a;
    s_life_a = s_life_b;
    s_life_b = t;
    return pop;
}

static void scr_life(bool entering, int64_t tf)
{
    {
        uint8_t wi, wn; uint16_t ws;
        whm_ui_wander_get(&wi, &wn, &ws);
        s_life_wx = (uint32_t)wi * 64u;
    }
    if (!s_life_a) {
        s_life_a = heap_caps_calloc(W * H, 1, MALLOC_CAP_SPIRAM);
        s_life_b = heap_caps_calloc(W * H, 1, MALLOC_CAP_SPIRAM);
        if (!s_life_a) s_life_a = heap_caps_calloc(W * H, 1,
                                      MALLOC_CAP_INTERNAL);
        if (!s_life_b) s_life_b = heap_caps_calloc(W * H, 1,
                                      MALLOC_CAP_INTERNAL);
        if (!s_life_a || !s_life_b) return;
        uint8_t p8 = 0;
        if (whm_settings_get_u8("life_pal", &p8) == ESP_OK &&
            p8 < LIFE_PAL_N) {
            s_life_pal = p8;
        }
        life_seed();
    }
    if (entering) life_seed();

    uint8_t sidx, sn;
    uint16_t sspd;
    whm_ui_wander_get(&sidx, &sn, &sspd);

    if (sn > 1) {
        /* FLEET: one CA across the strip. Generations gate on the show
           clock (100ms/gen), edges exchange over whmcast, reseeds follow
           a deterministic 60s epoch schedule (local random/pop reseeds
           would diverge the flock). Stale ghosts approximate one seam
           column for one generation and self-heal. */
        int64_t t = tf;
        uint32_t gen_now = (uint32_t)(t / 100000LL);
        uint32_t epoch = gen_now / 600u;
        static uint32_t last_gen = 0, last_epoch = 0xFFFFFFFFu;
        if (epoch != last_epoch) {
            life_seed_det(epoch, sidx);
            last_epoch = epoch;
            last_gen = gen_now;
        }
        uint8_t gl[64], gr[64];
        uint8_t lp = (uint8_t)((sidx + sn - 1) % sn);
        uint8_t rp = (uint8_t)((sidx + 1) % sn);
        int steps = (int)(gen_now - last_gen);
        if (steps > 3) steps = 3;           /* hiccup: cap catch-up */
        while (steps-- > 0) {
            const uint8_t *pl =
                whm_sync_life_edge(lp, 1, gl, NULL) ? gl : NULL;
            const uint8_t *pr =
                whm_sync_life_edge(rp, 0, gr, NULL) ? gr : NULL;
            life_step(pl, pr);
            last_gen++;
            uint8_t colL[64], colR[64];
            for (int y = 0; y < H; y++) {
                colL[y] = s_life_a[y * W];
                colR[y] = s_life_a[y * W + (W - 1)];
            }
            whm_sync_life_send(sidx, 0, last_gen, colL);
            whm_sync_life_send(sidx, 1, last_gen, colR);
        }
    } else {
        static int div = 0;
        if ((++div % 3) == 0) {            /* ~10 gen/s, solo */
            int pop = life_step(NULL, NULL);
            if (pop < 12 ||
                esp_timer_get_time() - s_life_seeded_at >
                    45LL * 1000000LL) {
                life_seed();
            }
        }
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t age = s_life_a[y * W + x];
            if (!age) continue;
            uint8_t r, g, b;
            life_color(age, (uint8_t)x, (uint8_t)y, &r, &g, &b);
            whm_display_set_pixel(x, y, r, g, b);
        }
    }
    if (esp_timer_get_time() < s_life_toast_until) {
        whm_display_fill_rect(0, 51, W, 13, 0, 0, 0);
        gfx_text_center(W / 2, 54, k_life_pal_names[s_life_pal], 1,
                        230, 230, 240);
    }
}

/* --------------------------------------------------------------- patterns */

static void pat_corners(void)
{
    whm_display_fill_rect(0, 0, W, 1, 255, 255, 255);
    whm_display_fill_rect(0, H - 1, W, 1, 255, 255, 255);
    whm_display_fill_rect(0, 0, 1, H, 255, 255, 255);
    whm_display_fill_rect(W - 1, 0, 1, H, 255, 255, 255);
    whm_display_fill_rect(2, 2, 8, 8, 255, 0, 0);
    whm_display_fill_rect(W - 10, 2, 8, 8, 0, 255, 0);
    whm_display_fill_rect(2, H - 10, 8, 8, 0, 0, 255);
    whm_display_fill_rect(W - 10, H - 10, 8, 8, 255, 255, 255);
}

static void pat_gradients(void)
{
    static uint8_t *row = NULL;
    if (!row) row = malloc((size_t)64 * 3);
    if (!row) return;
    for (uint16_t y = 0; y < H; y++) {
        for (uint16_t x = 0; x < W; x++) {
            uint8_t r = (y < H / 2) ? (uint8_t)((x * 255) / (W - 1)) : 0;
            uint8_t g = (y >= H / 2) ? (uint8_t)((y * 255) / (H - 1)) : 0;
            uint8_t b = (uint8_t)(((x + y) * 255) / (W + H - 2));
            row[x * 3] = r; row[x * 3 + 1] = g; row[x * 3 + 2] = b / 2;
        }
        whm_display_blit_rgb888(0, y, W, 1, row);
    }
}

static void pat_grayscale(void)
{
    const int steps = 8;
    for (int i = 0; i < steps; i++) {
        uint8_t v = (uint8_t)((i * 255) / (steps - 1));
        whm_display_fill_rect((uint16_t)(i * W / steps), 0,
                              (uint16_t)(W / steps), H, v, v, v);
    }
}

static void pat_checker(void)
{
    for (uint16_t y = 0; y < H; y++)
        for (uint16_t x = 0; x < W; x++) {
            uint8_t v = ((x ^ y) & 1) ? 255 : 0;
            whm_display_set_pixel(x, y, v, v, v);
        }
}

/* stepped sweep with pacing stats; call at 60fps */
static void pat_sweep_step(bool entering)
{
    static uint16_t x;
    static int64_t next, worst, sum;
    static uint32_t frames;
    if (entering) {
        x = 0; worst = sum = 0; frames = 0;
        next = esp_timer_get_time();
    }
    whm_display_fill(0, 0, 12);
    whm_display_fill_rect(x, 0, 2, H, 255, 255, 255);
    x = (uint16_t)((x + 1) % (W - 1));
    frames++;

    int64_t now = esp_timer_get_time();
    int64_t late = now - next;
    if (late > worst) worst = late;
    if (late > 0) sum += late;
    next += 1000000 / 60;
    if (frames % 600 == 0) {
        ESP_LOGI(TAG, "sweep: %lu frames, worst miss %lld us, avg %lld us",
                 (unsigned long)frames, (long long)worst,
                 (long long)(sum / frames));
    }
}

/* ----------------------------------------------------------------- button */

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << WHM_BUTTON_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
}

#define DBL_MS   500
#define HOLD_MS  600

typedef enum { BE_NONE, BE_SINGLE, BE_DOUBLE, BE_LONG } btn_ev_t;

/* debounced edges + single/double/long classification. Screens without a
 * double handler get SINGLE on release instantly; only app screens pay the
 * DBL_MS disambiguation wait. */
static btn_ev_t button_service(bool screen_has_tap2)
{
    static bool db_level = false;
    static int64_t db_t = 0;
    static bool down = false, long_fired = false, pend1 = false;
    static int64_t t_down = 0, t_pend = 0, t_lastup = 0;

    int64_t now = esp_timer_get_time();
    bool raw = gpio_get_level(WHM_BUTTON_BOOT) == 0;
    if (raw != db_level && now - db_t > 20000) {
        db_level = raw;
        db_t = now;
        if (raw) {                         /* DOWN */
            down = true;
            t_down = now;
            long_fired = false;
        } else if (down) {                 /* UP */
            down = false;
            if (!long_fired) {
                if (pend1 && now - t_lastup <= DBL_MS * 1000) {
                    pend1 = false;
                    t_lastup = now;
                    return BE_DOUBLE;
                }
                t_lastup = now;
                if (screen_has_tap2) {
                    pend1 = true;
                    t_pend = now;
                } else {
                    return BE_SINGLE;
                }
            }
        }
    }
    if (down && !long_fired && now - t_down >= HOLD_MS * 1000) {
        long_fired = true;
        pend1 = false;
        return BE_LONG;
    }
    if (pend1 && now - t_pend > DBL_MS * 1000) {
        pend1 = false;
        return BE_SINGLE;
    }
    return BE_NONE;
}

/* ------------------------------------------------------------------- task */

void whm_ui_task(void *arg)
{
    (void)arg;
    W = whm_display_width();
    H = whm_display_height();
    if (!W || !H) {
        ESP_LOGE(TAG, "display not running; ui task exiting");
        vTaskDelete(NULL);
        return;
    }
    button_init();
    timer_load_default();

    uint8_t boot_auto = 0;
    whm_settings_get_u8("boot_auto", &boot_auto);
    if (boot_auto) s_mode = M_SCR_AUTO;

    ui_mode_t drawn_mode = M_PAT_CYCLE;     /* != boot mode -> first-frame entering */
    screen_t drawn_screen = SCR_COUNT;
    pattern_t drawn_pattern = P_COUNT;
    int64_t screen_since = esp_timer_get_time();
    int64_t cycle_stage_until = 0;
    int cycle_stage = 0;

    while (1) {
        timer_service();
        bool in_scr = (s_mode == M_SCR_AUTO || s_mode == M_SCR_HOLD);
        btn_ev_t be = button_service(in_scr && k_tap2[s_screen] != NULL);
        if (s_sas.active && s_sas.done < 0 && be != BE_NONE) {
            if (be == BE_SINGLE) whm_whlink_sas_result(true);
            else if (be == BE_LONG) whm_whlink_sas_result(false);
            be = BE_NONE;              /* the pairing owns the tap */
        }
        if (s_mode == M_BOOT) {
            if (be != BE_NONE) {
                s_boot0 = esp_timer_get_time() - 2100000;
            }
            be = BE_NONE;
        }
        if (be == BE_SINGLE) {
            if (in_scr) whm_ui_screen_set("next");
            else ui_resume_screens();
        } else if (be == BE_DOUBLE) {
            if (in_scr && k_tap2[s_screen]) k_tap2[s_screen]();
            else if (!in_scr) ui_resume_screens();
        } else if (be == BE_LONG) {
            if (in_scr && k_hold[s_screen]) {
                k_hold[s_screen]();
            } else if (in_scr) {
                if (s_mode == M_SCR_AUTO) {
                    s_mode = M_SCR_HOLD;
                    s_gen++;
                    ESP_LOGI(TAG, "rotation paused (holding %s)",
                             k_scr_names[s_screen]);
                } else {
                    whm_ui_screen_set("auto");
                }
            } else {
                whm_ui_screen_set("auto");
            }
        }

        ui_mode_t mode = s_mode;
        if (s_sas.active) {            /* SAS pairing takes the
                                          stage (and the button) */
            int64_t tf = ui_frame_wait_div(2);
            if (sas_screen(tf)) {
                ui_present(tf);
                continue;
            }
        }
        if (s_otui.active) {           /* OTA takeover at the TRUE
                                          loop head - two prior
                                          placements sat below
                                          branches that continue
                                          (M_TEXT, then the screens
                                          line); the photo of a home
                                          screen mid-pull convicted
                                          the second */
            int64_t tf = ui_frame_wait_div(2);
            if (ota_screen(tf)) {
                ui_present(tf);
                continue;
            }
        }
        bool in_screens = (mode == M_SCR_AUTO || mode == M_SCR_HOLD);
        bool was_screens = (drawn_mode == M_SCR_AUTO || drawn_mode == M_SCR_HOLD);
        if (in_screens) {
            s_frame_t = ui_frame_wait_div(2);   /* 30fps screens line */
        }

        if (in_screens) {
            if (mode == M_SCR_AUTO &&
                esp_timer_get_time() - screen_since >
                    (int64_t)AUTO_DWELL_MS * 1000) {
                s_screen = (screen_t)((s_screen + 1) % SCR_COUNT);
            }
            screen_t scr = s_screen;
            bool entering = !was_screens || (scr != drawn_screen) ||
                            (mode != drawn_mode);
            if (entering) {
                if (drawn_screen == SCR_VU && scr != SCR_VU) {
                    scr_vu(false, true);        /* close mic */
                }
                screen_since = esp_timer_get_time();
            }
            whm_display_clear();
            switch (scr) {
            case SCR_HOME:  scr_home(entering); break;
            case SCR_CLOCK: scr_clock(); break;
            case SCR_TIMER: scr_timer(); break;
            case SCR_WATCH: scr_watch(); break;
            case SCR_MUSIC: scr_music(); break;
            case SCR_LEVEL: scr_level(); break;
            case SCR_ENV:   scr_env(); break;
            case SCR_NET:   scr_net(); break;
            case SCR_SYS:   scr_sys(); break;
            case SCR_SD:    scr_sd(); break;
            case SCR_VU:    scr_vu(entering && drawn_screen != SCR_VU, false); break;
            case SCR_LIFE:  scr_life(entering, s_frame_t); break;
            case SCR_ORACLE: scr_oracle(); break;
            default: break;
            }
            drawn_screen = scr;
            drawn_mode = mode;
            if (s_mode == M_SCR_AUTO || s_mode == M_SCR_HOLD) {
            status_edges_and_bar();
        }
        ui_present(0);
            continue;
        }

        /* transitioning out of screens: release screen resources */
        if (was_screens) {
            if (drawn_screen == SCR_VU) scr_vu(false, true);
            drawn_screen = SCR_COUNT;
        }

        if (mode == M_BOOT) {
            if (!s_boot0) {
                printf("ui: alive (Ignition)\n");
                s_boot0 = esp_timer_get_time();
                whm_audio_chime_async();     /* relocated: phase lock */
            }
            int ms = (int)((esp_timer_get_time() - s_boot0) / 1000);
            whm_display_clear();
            ignition_render(ms);
            whm_display_flip();
            if (ms > 2025) {                 /* ~1.5x: sound decays,
                                                light lingers */
                uint8_t bm = 0;
                whm_settings_get_u8("bootmode", &bm);
                s_screen = SCR_HOME;
                s_mode = bm ? M_SCR_AUTO : M_SCR_HOLD;
                s_gen++;
            }
            vTaskDelay(pdMS_TO_TICKS(16));
            continue;
        }

        if (mode == M_TEXT) {
            drawn_mode = M_TEXT;
            drawn_pattern = P_COUNT;
            int64_t tf = ui_frame_wait_div(1);
            whm_display_clear();
            scr_text(tf);
            whm_display_flip();
            continue;
        }

        if (mode == M_PAT_HOLD) {
            pattern_t p = s_pattern;
            bool entering = (drawn_mode != M_PAT_HOLD) || (p != drawn_pattern);
            drawn_mode = M_PAT_HOLD;
            drawn_pattern = p;

            if (p == P_SWEEP) {
                pat_sweep_step(entering);
                ui_present(0);
                vTaskDelay(pdMS_TO_TICKS(16));
                continue;
            }
            if (p == P_WANDER || p == P_WALKER) {
                int64_t tf = ui_frame_wait_div(1);   /* 60fps master */
                if (p == P_WALKER) pat_walker(tf);
                else pat_wander(tf);
                ui_present(tf);
                continue;
            }
            if (entering) {
                whm_display_clear();
                switch (p) {
                case P_CORNERS: pat_corners(); break;
                case P_RED:   whm_display_fill(255, 0, 0); break;
                case P_GREEN: whm_display_fill(0, 255, 0); break;
                case P_BLUE:  whm_display_fill(0, 0, 255); break;
                case P_WHITE:
                    ESP_LOGW(TAG, "solid WHITE held - watch supply current");
                    whm_display_fill(255, 255, 255);
                    break;
                case P_GRADIENT: pat_gradients(); break;
                case P_GRAY: pat_grayscale(); break;
                case P_CHECKER: pat_checker(); break;
                default: break;
                }
                ui_present(0);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* M_PAT_CYCLE: staged tour; stage 8 (sweep) runs live at 60fps */
        if (drawn_mode != M_PAT_CYCLE) {
            drawn_mode = M_PAT_CYCLE;
            drawn_pattern = P_CYCLE;
            cycle_stage = 0;
            cycle_stage_until = 0;
        }
        int64_t now = esp_timer_get_time();
        if (cycle_stage < 8) {
            if (now >= cycle_stage_until) {
                if (cycle_stage_until != 0) cycle_stage++;   /* 0 draws first */
                if (cycle_stage < 8) {
                    uint32_t dur_ms = 3500;
                    whm_display_clear();
                    switch (cycle_stage) {
                    case 0: ESP_LOGI(TAG, "cycle: tour start"); pat_corners(); break;
                    case 1: whm_display_fill(255, 0, 0); dur_ms = 1700; break;
                    case 2: whm_display_fill(0, 255, 0); dur_ms = 1700; break;
                    case 3: whm_display_fill(0, 0, 255); dur_ms = 1700; break;
                    case 4: ESP_LOGW(TAG, "cycle: WHITE 3s - watch supply current");
                            whm_display_fill(255, 255, 255); dur_ms = 3000; break;
                    case 5: pat_gradients(); break;
                    case 6: pat_grayscale(); break;
                    case 7: pat_checker(); break;
                    default: break;
                    }
                    whm_display_flip();
                    cycle_stage_until = now + (int64_t)dur_ms * 1000;
                } else {
                    cycle_stage_until = 0;      /* hand off to sweep init */
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (cycle_stage_until == 0) {
            pat_sweep_step(true);
            cycle_stage_until = now + 10LL * 1000000LL;
        } else {
            pat_sweep_step(false);
        }
        ui_present(0);
        if (now >= cycle_stage_until) {
            cycle_stage = 0;
            cycle_stage_until = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}
