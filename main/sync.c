/*
 * sync.c - WHM-LINK P2a: fleet roles, presence, and the fleet wall clock.
 *
 * See WHM-LINK.md for the full protocol design. This layer:
 *   - conductor: runs the SoftAP (existing wifi module) and announces at
 *     1Hz with an (epoch_us, tsf_us) anchor pair;
 *   - member: joins WHM-<conductor> as STA (TSF then hardware-locks to
 *     the conductor's beacons) and derives wall time from the anchor:
 *     epoch_now = anchor.epoch + (tsf_now - anchor.tsf) - microsecond-
 *     class, no NTP, because the transport IS the clock;
 *   - everyone: listens on UDP 7777 and keeps a peer table.
 *
 * Roles persist in NVS; the flock self-assembles on power-up (member
 * STA rejoin rides the wifi module's own saved credentials).
 */
#include "sync.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <errno.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "wifi_tsf.h"
#include "timesync.h"
#include "settings.h"
#include "sensors_test.h"
#include "mdns.h"
#include "esp_console.h"
#include "esp_random.h"
#include "mp3_player.h"
#ifndef WHM_VERSION_STR
#define WHM_VERSION_STR "dev"
#endif
#include "ui.h"

static void fleet_exec_task(void *arg);   /* defined with the queue */
static uint32_t s_ctr_cmd_rx, s_ctr_cmd_exec, s_ctr_drop_size,
                s_ctr_drop_dup, s_ctr_drop_self, s_ctr_drop_ver,
                s_ctr_rx_ann, s_ctr_rx_life, s_ctr_rx_play;
static uint32_t s_ctr_rx_cmd_alias(void) { return s_ctr_cmd_rx; }

static const char *TAG = "whm_sync";

#define WHM_SYNC_PORT 7777
#define PEER_MAX 8

typedef struct __attribute__((packed)) {
    char magic[4];          /* 'WHML' */
    uint8_t ver;            /* 1 */
    uint8_t type;           /* 1 = ANNOUNCE */
    uint8_t role;           /* whm_sync_role_t */
    uint8_t rsv;
    uint16_t seq;
    char name[16];
    char fw[12];
    int64_t epoch_us;       /* anchor pair, sampled together */
    int64_t tsf_us;
    float temp_c;
    float rh;
} whm_ann_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 1 */
    uint8_t type;           /* 2 = LIFE_EDGE */
    uint8_t idx;            /* sender's strip index */
    uint8_t side;           /* 0 = left edge, 1 = right edge */
    uint32_t gen;
    uint8_t cells[64];      /* ages */
} whm_life_edge_t;

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 1 */
    uint8_t type;           /* 3 = FLEET CMD */
    uint8_t rsv[2];
    uint32_t seq;
    char from[16];
    char target[16];        /* empty = every node */
    int64_t exec_at;        /* TSF deadline: everyone runs it together */
    char line[160];
} whm_cmd_t;
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 2 */
    uint8_t type;           /* 6 = MODE-B DJ announce */
    uint8_t playing;        /* 1 = tune in, 0 = stop */
    uint8_t ch;
    uint32_t seq;
    uint32_t rate;
    char from[16];
    char title[64];
} whm_djb_t;
_Static_assert(sizeof(whm_djb_t) == 96, "djb wire");

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 2 */
    uint8_t type;           /* 7 = NYE fleet takeover */
    uint16_t year;
    uint32_t seq;
    char from[16];
    char tz[8];
    int64_t start_tsf;      /* show T0 (initiator 23:55:00) */
} whm_nye_t;
_Static_assert(sizeof(whm_nye_t) == 44, "nye wire");

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 2 */
    uint8_t type;           /* 8 = Mode-A master beacon */
    uint16_t rsv;
    uint32_t seq;
    char from[16];
    int64_t tsf;            /* master's clock at... */
    int64_t idx;            /* ...this content position */
} whm_mab_t;
_Static_assert(sizeof(whm_mab_t) == 44, "mab wire");

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 2 */
    uint8_t type;           /* 9 = walker pose/ownership beacon */
    uint8_t owner;          /* strip idx that simulates */
    uint8_t st;
    int8_t dir;
    int8_t y;
    uint16_t timer;
    uint32_t seq;
    float x;                /* world-x */
    char from[16];
    int64_t tsf;
    float tgt;           /* complete-promise fields: a snap that */
    float vx;            /* omits these leaves the replica       */
                         /* chasing its own target               */
} whm_wkb_t;
_Static_assert(sizeof(whm_wkb_t) == 52, "wkb wire");

_Static_assert(sizeof(whm_cmd_t) == 212,
               "cmd v2 wire format: 212 bytes on every unit");
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;
    uint8_t type;           /* 4 = MODE A PLAY */
    uint8_t flags;          /* bit0 = stop */
    uint8_t rsv;
    int64_t start_tsf;
    char name[64];
    char sha[64];
} whm_play_t;
static int64_t s_play_seen = 0;

static uint32_t s_cmd_seq;
static uint32_t s_cmd_seen_seq[4];
static char s_cmd_seen_from[4][16];

typedef struct {
    uint32_t gen;
    int64_t rx_us;
    uint8_t cells[64];
    bool have;
} edge_store_t;
static edge_store_t s_edges[8][2];

typedef struct {
    char name[16];
    char fw[12];
    uint8_t role;
    uint8_t prio;
    int64_t last_us;        /* local esp_timer at receipt */
    float temp_c, rh;
} peer_t;

typedef enum { SM_OFF = 0, SM_SOFTAP = 1, SM_AUTO = 2 } sync_mode_t;
static sync_mode_t s_smode = SM_OFF;
static uint8_t s_prio = 100;
static char s_anchor_name[16] = "";      /* current winner we follow */
static int64_t s_promote_at = 0;         /* jittered election deadline */
static whm_sync_role_t s_role = WHM_SYNC_OFF;
static char s_peer_name[16] = "";

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 2 */
    uint8_t type;           /* 11 = walker input */
    uint8_t act;            /* 0 auto,1 left,2 right,3 stop,4 jump */
    uint8_t rsv;
    uint32_t exec_step;
    uint32_t seq;
    char from[16];
    float arg;
} whm_wki_t;
_Static_assert(sizeof(whm_wki_t) == 36, "wki wire");

static uint32_t s_wki_seq;
static uint32_t s_wki_seen_seq[4];
static char s_wki_seen_from[4][16];

/* L5 tunnel surface: a TAP sees every fleet-relevant datagram
 * (types 2/7/9) - inbound after the door gate, outbound at every
 * tx site - and INJECT feeds one in through the FRONT DOOR via UDP
 * loopback (full validation, dedupe, and handlers for free; the
 * (from,seq) cmd dedupe is the loop guard). */
static void (*s_tap)(const uint8_t *buf, int len);
static void sync_tap(const void *buf, int len)
{
    const uint8_t *b = (const uint8_t *)buf;
    if (!s_tap || len < 8) return;
    if (b[5] != 2 && b[5] != 7 && b[5] != 9 && b[5] != 10 &&
        b[5] != 11) return;
    s_tap(b, len);
}

void whm_sync_set_tap(void (*cb)(const uint8_t *, int))
{
    s_tap = cb;
}

static peer_t s_peers[PEER_MAX];
static uint16_t s_seq = 0;
static int s_sock = -1;

void whm_sync_inject(const uint8_t *buf, int len, bool rebroadcast)
{
    if (len < 8 || len > 256 || !s_sock) return;
    struct sockaddr_in lo = { 0 };
    lo.sin_family = AF_INET;
    lo.sin_port = htons(7777);
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(s_sock, buf, len, 0, (struct sockaddr *)&lo,
           sizeof(lo));                    /* front door, locally */
    if (rebroadcast) {
        struct sockaddr_in dst = { 0 };
        dst.sin_family = AF_INET;
        dst.sin_port = htons(7777);
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(s_sock, buf, len, 0, (struct sockaddr *)&dst,
               sizeof(dst));               /* the bridge role */
    }
}
static volatile bool s_wall_applied = false;
static int64_t s_last_cond_us = 0;

static void my_name(char *out, size_t n)
{
    out[0] = 0;
    whm_settings_get_str("node", out, n);
    if (!out[0]) strlcpy(out, "whm", n);
}

static void mdns_role_update(void);
static void election_tick(void);

static const char *leader_word(void)
{
    return s_smode == SM_AUTO ? "anchor" : "conductor";
}

void whm_sync_status_brief(char *mode16, char *role16,
                           uint8_t *prio, char *anchor16)
{
    strlcpy(mode16, s_smode == SM_AUTO ? "auto" :
                    s_smode == SM_SOFTAP ? "softap" : "off", 16);
    strlcpy(role16, s_role == WHM_SYNC_CONDUCTOR ? leader_word() :
                    s_role == WHM_SYNC_MEMBER ? "member" : "off",
            16);
    *prio = s_prio;
    strlcpy(anchor16, s_anchor_name[0] ? s_anchor_name : "-", 16);
}

int whm_sync_peer_iter(int i, char n16[16], char f8[8],
                       uint32_t *age_ms, uint8_t *role)
{
    int64_t now2 = esp_timer_get_time();
    int seen = 0;
    for (int k = 0; k < PEER_MAX; k++) {
        if (!s_peers[k].name[0]) continue;
        if (now2 - s_peers[k].last_us > 30000000LL) continue;
        if (seen++ == i) {
            strlcpy(n16, s_peers[k].name, 16);
            strlcpy(f8, s_peers[k].fw, 8);
            *age_ms = (uint32_t)((now2 - s_peers[k].last_us)
                                 / 1000);
            *role = s_peers[k].role;
            return 1;
        }
    }
    return 0;
}

const char *whm_sync_peer_fw(const char *name)
{
    int64_t now2 = esp_timer_get_time();
    for (int i = 0; i < PEER_MAX; i++)
        if (s_peers[i].name[0] &&
            strcasecmp(s_peers[i].name, name) == 0 &&
            now2 - s_peers[i].last_us < 30000000LL)
            return s_peers[i].fw;
    return NULL;
}

static const char *role_name(uint8_t r)
{
    if (r & 0x80) return "anchor";     /* elections-mode leader:
                                          the peer's own dialect
                                          travels in bit7 */
    return r == WHM_SYNC_CONDUCTOR ? leader_word()
         : r == WHM_SYNC_MEMBER ? "member" : "off";
}

/* --------------------------------------------------------------- announce */

static void fill_announce(whm_ann_t *p)
{
    memset(p, 0, sizeof(*p));
    memcpy(p->magic, "WHML", 4);
    p->ver = 1;
    p->type = 1;
    p->role = (uint8_t)s_role |
              ((s_role == WHM_SYNC_CONDUCTOR &&
                s_smode == SM_AUTO) ? 0x80 : 0);
    /* bit7 = elections-mode leader: peers print "anchor" not
       "conductor". Low bits stay wire-compat. */
    p->rsv = s_prio;                     /* election weight (wire-compat) */
    p->seq = s_seq++;
    my_name(p->name, sizeof(p->name));
    strlcpy(p->fw, WHM_VERSION_STR, sizeof(p->fw));

    struct timeval tv;
    gettimeofday(&tv, NULL);
    p->epoch_us = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
    p->tsf_us = whm_wifi_tsf_now();
    float t = 0, rh = 0;
    if (whm_sensors_read_th(&t, &rh) == ESP_OK) {
        p->temp_c = t;
        p->rh = rh;
    }
}

static void announce_task(void *arg)
{
    (void)arg;
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    uint32_t tick = 0;
    for (;;) {
        election_tick();
        bool due = (s_role == WHM_SYNC_CONDUCTOR) || (tick % 5 == 0);
        if (s_role != WHM_SYNC_OFF && s_sock >= 0 && due) {
            whm_ann_t p;
            fill_announce(&p);
            sendto(s_sock, &p, sizeof(p), 0,
                   (struct sockaddr *)&dst, sizeof(dst));
        sync_tap((const uint8_t *)&p, (int)sizeof(p));
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ----------------------------------------------------------------- recv */

static void note_peer(const whm_ann_t *p)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < PEER_MAX; i++) {
        if (s_peers[i].name[0] &&
            strncmp(s_peers[i].name, p->name, sizeof(p->name)) == 0) {
            slot = i;
            break;
        }
        if (!s_peers[i].name[0]) { if (slot < 0) slot = i; }
        else if (s_peers[i].last_us < s_peers[oldest].last_us) oldest = i;
    }
    if (slot < 0) slot = oldest;
    strlcpy(s_peers[slot].name, p->name, sizeof(s_peers[slot].name));
    strlcpy(s_peers[slot].fw, p->fw, sizeof(s_peers[slot].fw));
    s_peers[slot].role = p->role;
    s_peers[slot].prio = p->rsv;
    s_peers[slot].last_us = esp_timer_get_time();
    s_peers[slot].temp_c = p->temp_c;
    s_peers[slot].rh = p->rh;
}

static void maybe_take_wall_clock(const whm_ann_t *p)
{
    if (s_role != WHM_SYNC_MEMBER || p->role != WHM_SYNC_CONDUCTOR) return;
    if (s_smode == SM_AUTO && s_anchor_name[0] &&
        strncmp(p->name, s_anchor_name, sizeof(s_anchor_name)) != 0) {
        return;                            /* only the elected anchor */
    }
    s_last_cond_us = esp_timer_get_time();
    if (whm_timesync_state() == WHM_TS_SYNCED) return;  /* SNTP outranks */
    if (p->epoch_us < 1735689600LL * 1000000LL) return; /* pre-2025: junk */

    int64_t our_tsf = whm_wifi_tsf_now();
    if (our_tsf <= 0 || p->tsf_us <= 0) return;
    int64_t epoch_now = p->epoch_us + (our_tsf - p->tsf_us);

    struct timeval cur;
    gettimeofday(&cur, NULL);
    int64_t cur_us = (int64_t)cur.tv_sec * 1000000LL + cur.tv_usec;
    if (llabs(cur_us - epoch_now) < 100000) return;     /* within 100ms */

    struct timeval tv = {
        .tv_sec = (time_t)(epoch_now / 1000000LL),
        .tv_usec = (suseconds_t)(epoch_now % 1000000LL),
    };
    settimeofday(&tv, NULL);
    whm_timesync_rtc_write_now();
    if (!s_wall_applied) {
        s_wall_applied = true;
        ESP_LOGI(TAG, "wall clock taken from %s '%.16s' (via shared TSF)",
                 leader_word(), p->name);
    }
}

static void recv_task(void *arg)
{
    (void)arg;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    int one = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in me = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(s_sock, (struct sockaddr *)&me, sizeof(me));

    /* THE 76-BYTE BUG: this buffer was sized for the two original
       packet types and never grew when cmd (204) and play (144) joined
       the family - recvfrom truncated every fleet command and Mode A
       PLAY to a 76-byte stump. Sized for the whole family now, with
       slack for the next member. */
    uint8_t rbuf[256];
    _Static_assert(sizeof(whm_cmd_t) <= 256, "grow rbuf");
    _Static_assert(sizeof(whm_play_t) <= 256, "grow rbuf");
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, rbuf, sizeof(rbuf), 0,
                         (struct sockaddr *)&from, &fl);
        if (n < 8 || memcmp(rbuf, "WHML", 4) != 0) continue;
        sync_tap(rbuf, n);
        if (rbuf[4] < 1 || rbuf[4] > 2) {
            /* THE DOOR GATE that silently ate every v2 cmd for three
               releases - now it counts and confesses instead */
            s_ctr_drop_ver++;
            printf("fleet: DROP ver %u pkt type %u (%dB) - mixed "
                   "firmware?\n", rbuf[4], rbuf[5], n);
            continue;
        }
        /* version-skew becomes a log line, not a mystery */
        if (rbuf[5] == 3 && n != (int)sizeof(whm_cmd_t)) {
            s_ctr_drop_size++;
            printf("fleet: DROP bad cmd size %d (want %d) - "
                   "mixed firmware? flash all units\n",
                   n, (int)sizeof(whm_cmd_t));
            continue;
        }
        if ((rbuf[5] == 3 && n != (int)sizeof(whm_cmd_t)) ||
            (rbuf[5] == 4 && n != (int)sizeof(whm_play_t)) ||
            (rbuf[5] == 2 && n != (int)sizeof(whm_life_edge_t))) {
            s_ctr_rx_play++;
            s_ctr_rx_life++;
            static int64_t last_warn = 0;
            int64_t nw = esp_timer_get_time();
            if (nw - last_warn > 10000000LL) {
                ESP_LOGW(TAG, "type-%d size %d != expected (cmd=%d "
                              "play=%d life=%d) - version skew? "
                              "hdr: %02x%02x%02x%02x %02x%02x%02x%02x "
                              "%02x%02x%02x%02x",
                         rbuf[5], n, (int)sizeof(whm_cmd_t),
                         (int)sizeof(whm_play_t),
                         (int)sizeof(whm_life_edge_t),
                         rbuf[0], rbuf[1], rbuf[2], rbuf[3], rbuf[4],
                         rbuf[5], rbuf[6], rbuf[7], rbuf[8], rbuf[9],
                         rbuf[10], rbuf[11]);
                last_warn = nw;
            }
            continue;
        }
        if (rbuf[5] == 2 && n == (int)sizeof(whm_life_edge_t)) {
            const whm_life_edge_t *e = (const whm_life_edge_t *)rbuf;
            if (e->idx < 8 && e->side < 2) {
                edge_store_t *st = &s_edges[e->idx][e->side];
                st->gen = e->gen;
                st->rx_us = esp_timer_get_time();
                memcpy(st->cells, e->cells, 64);
                st->have = true;
            }
            continue;
        }
        if (rbuf[5] == 4 && n == (int)sizeof(whm_play_t)) {
            whm_play_t pl;
            memcpy(&pl, rbuf, sizeof(pl));
            pl.name[63] = 0;
            char sha[65];
            memcpy(sha, pl.sha, 64);
            sha[64] = 0;
            if (pl.start_tsf == s_play_seen) continue;
            s_play_seen = pl.start_tsf;
            whm_mp3_fleet_play(pl.name, sha, pl.start_tsf,
                               (pl.flags & 1) != 0);
            continue;
        }
        if (rbuf[5] == 3 && n == (int)sizeof(whm_cmd_t)) {
            whm_cmd_t c;
            memcpy(&c, rbuf, sizeof(c));
            c.from[15] = 0;
            c.line[159] = 0;
            c.target[15] = 0;
            char me[16];
            my_name(me, sizeof(me));
            if (strncmp(c.from, me, sizeof(me)) == 0) {
                ESP_LOGD(TAG, "fleet: own echo, skip");
                continue;
            }
            char tgt2[16];
            strlcpy(tgt2, c.target, sizeof(tgt2));
            char *dot2 = strstr(tgt2, ".local");
            if (dot2) *dot2 = 0;         /* forgive @Two.local */
            if (tgt2[0] && strncasecmp(tgt2, me, sizeof(me)) != 0) {
                ESP_LOGD(TAG, "fleet: for '%s', not me ('%s')",
                         c.target, me);
                continue;
            }
            bool dup = false;
            for (int i = 0; i < 4; i++) {
                if (s_cmd_seen_seq[i] == c.seq &&
                    strncmp(s_cmd_seen_from[i], c.from, 16) == 0) {
                    dup = true;
                }
            }
            if (dup) {
                s_ctr_drop_dup++;
                ESP_LOGD(TAG, "fleet: dup seq %u from %s", (unsigned)c.seq,
                         c.from);
                continue;
            }
            s_ctr_cmd_rx++;
            static int wr = 0;
            s_cmd_seen_seq[wr] = c.seq;
            strlcpy(s_cmd_seen_from[wr], c.from, 16);
            wr = (wr + 1) % 4;
            char me2[17] = "";
            my_name(me2, sizeof(me2));
            if (strcmp(c.from, me2) == 0) {          /* own broadcast:
                                                        already deferred */
                s_ctr_drop_self++;
                continue;
            }
            printf("\n[fleet:%s] %s\n", c.from, c.line);
            /* deadline execution: run it when everyone runs it */
            whm_sync_defer_line(c.line, c.exec_at);
            continue;
        }
        if (rbuf[5] == 9 && n == (int)sizeof(whm_wkb_t)) {
            whm_wkb_t w;
            memcpy(&w, rbuf, sizeof(w));
            char me6[17] = "";
            my_name(me6, sizeof(me6));
            if (strcmp(w.from, me6) != 0) {
        if (rbuf[5] == 10 && n >= 24) {
            float c10;
            memcpy(&c10, rbuf + 16, 4);    /* cam POSITION (owner
                                              authoritative snap) */
            whm_ui_cam_set(c10);
        }
        if (rbuf[5] == 11 && n >= (int)sizeof(whm_wki_t)) {
            whm_wki_t ki;
            memcpy(&ki, rbuf, sizeof(ki));
            char me2[16];
            my_name(me2, sizeof(me2));
            if (strncmp(ki.from, me2, sizeof(me2)) != 0) {
                bool dup = false;
                for (int i2 = 0; i2 < 4; i2++)
                    if (s_wki_seen_seq[i2] == ki.seq &&
                        strncmp(s_wki_seen_from[i2], ki.from,
                                16) == 0) { dup = true; break; }
                if (!dup) {
                    static uint8_t wr2;
                    s_wki_seen_seq[wr2] = ki.seq;
                    memcpy(s_wki_seen_from[wr2], ki.from, 16);
                    wr2 = (uint8_t)((wr2 + 1) & 3);
                    whm_ui_walk_input_rx(ki.act, ki.exec_step,
                                         ki.arg);
                }
            }
            continue;
        }
                whm_ui_wkb_rx(w.owner, w.x, w.y, w.st, w.dir,
                              w.timer, w.seq, w.tsf, w.tgt, w.vx);
            }
            continue;
        }
        if (rbuf[5] == 8 && n == (int)sizeof(whm_mab_t)) {
            whm_mab_t mb;
            memcpy(&mb, rbuf, sizeof(mb));
            char me5[17] = "";
            my_name(me5, sizeof(me5));
            if (strcmp(mb.from, me5) != 0) {
                whm_mp3_a_on_beacon(mb.from, mb.tsf, mb.idx);
            }
            continue;
        }
        if (rbuf[5] == 7 && n == (int)sizeof(whm_nye_t)) {
            whm_nye_t y;
            memcpy(&y, rbuf, sizeof(y));
            char me4[17] = "";
            my_name(me4, sizeof(me4));
            if (strcmp(y.from, me4) != 0) {
                whm_ui_nye_join(y.from, y.tz, (int)y.year,
                                y.start_tsf);
            }
            continue;
        }
        if (rbuf[5] == 6 && n == (int)sizeof(whm_djb_t)) {
            whm_djb_t d;
            memcpy(&d, rbuf, sizeof(d));
            char me3[17] = "";
            my_name(me3, sizeof(me3));
            if (strcmp(d.from, me3) != 0) {
                whm_mp3_b_on_pkt(d.from, d.rate, d.ch,
                                 d.playing != 0, d.title);
            }
            continue;
        }
        if (rbuf[5] != 1 || n != (int)sizeof(whm_ann_t)) continue;
        s_ctr_rx_ann++;
        whm_ann_t p;
        memcpy(&p, rbuf, sizeof(p));
        char me_name[16];
        my_name(me_name, sizeof(me_name));
        if (strncmp(p.name, me_name, sizeof(p.name)) == 0) {
            continue;                       /* our own broadcast */
        }
        note_peer(&p);
        maybe_take_wall_clock(&p);
    }
}

/* ------------------------------------------------------------- election
 * Bully-lite over announces, infra (SM_AUTO) mode only:
 *  - claim = a fresh announce with role==CONDUCTOR; rank = (prio, name),
 *    name lexical as the deterministic tiebreak; a `sync follow <name>`
 *    pin outranks everything while that name is alive.
 *  - no fresh claim for a jittered 3-6s (hashed from our name so units
 *    don't stampede) and prio>0 -> self-promote.
 *  - claiming and a strictly better claim stays fresh -> step down.
 *  - preemption: a better unit returning takes over via the same rule;
 *    its claim must survive the freshness window, which is the hold-down.
 * SoftAP mode (SM_SOFTAP) never elects: the conductor IS the radio. */

#define CLAIM_FRESH_US   8000000LL
static bool claim_rank_gt(uint8_t ap, const char *an,
                          uint8_t bp, const char *bn)
{
    if (s_peer_name[0]) {                 /* pin outranks while alive */
        bool a_pin = strncmp(an, s_peer_name, 16) == 0;
        bool b_pin = strncmp(bn, s_peer_name, 16) == 0;
        if (a_pin != b_pin) return a_pin;
    }
    if (ap != bp) return ap > bp;
    return strncmp(an, bn, 16) > 0;
}

static bool best_live_claim(uint8_t *prio, char *name, size_t nlen)
{
    int64_t now = esp_timer_get_time();
    bool have = false;
    uint8_t bp = 0;
    char bn[16] = "";
    for (int i = 0; i < PEER_MAX; i++) {
        if (!s_peers[i].name[0]) continue;
        if ((s_peers[i].role & 0x7F) != WHM_SYNC_CONDUCTOR)
            continue;      /* bit7 = anchor label; claims
                              rank on the low bits */
        if (now - s_peers[i].last_us > CLAIM_FRESH_US) continue;
        if (!have || claim_rank_gt(s_peers[i].prio, s_peers[i].name,
                                   bp, bn)) {
            bp = s_peers[i].prio;
            strlcpy(bn, s_peers[i].name, sizeof(bn));
            have = true;
        }
    }
    if (have) {
        *prio = bp;
        strlcpy(name, bn, nlen);
    }
    return have;
}

static void election_tick(void)
{
    if (s_smode != SM_AUTO) return;
    int64_t now = esp_timer_get_time();
    char me[16];
    my_name(me, sizeof(me));
    uint8_t bp;
    char bn[16];
    bool claim = best_live_claim(&bp, bn, sizeof(bn));

    if (s_role == WHM_SYNC_CONDUCTOR) {
        if (claim && claim_rank_gt(bp, bn, s_prio, me)) {
            s_role = WHM_SYNC_MEMBER;
            s_wall_applied = false;
            strlcpy(s_anchor_name, bn, sizeof(s_anchor_name));
            strlcpy(s_peer_name, bn, sizeof(s_peer_name));
            s_last_cond_us = now;    /* election stepdown now fills
                                        the member-view fields the
                                        follow path always set -
                                        no more "member of ?" */
            mdns_role_update();
            ESP_LOGI(TAG, "stepping down: '%s' (prio %u) outranks me",
                     bn, (unsigned)bp);
        }
        return;
    }
    if (claim) {
        if (strncmp(s_anchor_name, bn, sizeof(s_anchor_name)) != 0) {
            strlcpy(s_anchor_name, bn, sizeof(s_anchor_name));
            strlcpy(s_peer_name, bn, sizeof(s_peer_name));
            s_wall_applied = false;
            ESP_LOGI(TAG, "anchor: '%s' (prio %u)", bn, (unsigned)bp);
        }
        s_last_cond_us = now;        /* any live claim = leader
                                        heard; keeps "heard Xs
                                        ago" honest for members */
        s_promote_at = 0;
        return;
    }
    /* no live anchor */
    if (s_prio == 0) return;               /* follower-only */
    if (!s_promote_at) {
        uint32_t h = 5381;
        for (const char *p = me; *p; p++) h = h * 33 + (uint8_t)*p;
        s_promote_at = now + 3000000LL + (int64_t)(h % 3000) * 1000LL;
        ESP_LOGI(TAG, "no anchor heard - election in %lldms",
                 (long long)((s_promote_at - now) / 1000));
        return;
    }
    if (now >= s_promote_at) {
        s_role = WHM_SYNC_CONDUCTOR;
        s_promote_at = 0;
        strlcpy(s_anchor_name, me, sizeof(s_anchor_name));
        mdns_role_update();
        ESP_LOGI(TAG, "promoted to anchor (prio %u)", (unsigned)s_prio);
    }
}

const char *whm_sync_anchor_name(void)
{
    if (s_role == WHM_SYNC_CONDUCTOR) {
        static char me[16];
        my_name(me, sizeof(me));
        return me;
    }
    return s_anchor_name[0] ? s_anchor_name : "?";
}

uint8_t whm_sync_prio(void) { return s_prio; }
int whm_sync_mode(void) { return (int)s_smode; }

/* --------------------------------------------------------------- public */

esp_err_t whm_sync_init(void)
{
    uint8_t r = 0, m = 0, pr = 100;
    whm_settings_get_u8("sync_role", &r);
    whm_settings_get_u8("sync_mode", &m);
    whm_settings_get_u8("sync_prio", &pr);
    s_prio = pr;
    if (m <= SM_AUTO) s_smode = (sync_mode_t)m;
    if (!m && r) s_smode = SM_SOFTAP;      /* legacy role-only NVS */
    if (r <= WHM_SYNC_MEMBER) s_role = (whm_sync_role_t)r;
    if (s_smode == SM_AUTO) s_role = WHM_SYNC_MEMBER;  /* re-elect fresh */
    whm_settings_get_str("sync_peer", s_peer_name, sizeof(s_peer_name));

    if (mdns_init() == ESP_OK) {
        char nm[16];
        my_name(nm, sizeof(nm));
        mdns_hostname_set(nm);
        mdns_instance_name_set(nm);
        mdns_txt_item_t txt[2] = {
            { "role", (char *)role_name((uint8_t)s_role) },
            { "fw", WHM_VERSION_STR },
        };
        mdns_service_add("WHM-LINK", "_whm-link", "_udp", WHM_SYNC_PORT,
                         txt, 2);
        ESP_LOGI(TAG, "mDNS: %s.local, _whm-link._udp/%d", nm,
                 WHM_SYNC_PORT);
    }

    /* rx back on PSRAM: its only flash touch (name lookups) now rides
       the NVS broker, so the doctrine below is satisfied - and the
       6K stays with internal heap where it belongs. */
    {
        static StaticTask_t s_rx_tcb;             /* TCB: internal */
        static StackType_t *s_rx_stack;
        s_rx_stack = heap_caps_malloc(6144 * sizeof(StackType_t),
                                      MALLOC_CAP_SPIRAM);
        TaskHandle_t rh = s_rx_stack
            ? xTaskCreateStatic(recv_task, "whm_sync_rx", 6144, NULL, 8,
                                s_rx_stack, &s_rx_tcb)
            : NULL;
        if (!rh) {
            printf("sync: rx task FAILED to start (internal=%u free)\n",
                   (unsigned)heap_caps_get_free_size(
                       MALLOC_CAP_INTERNAL));
        }
    }
    /* DOCTRINE (learned via cache_utils assert): PSRAM stacks ONLY for
       tasks that never touch spi_flash/NVS - flash ops disable cache,
       making PSRAM unreachable mid-call. mp3 qualifies (SD+I2S only).
       rx and exec run console commands and name/NVS lookups: INTERNAL. */
    if (xTaskCreate(fleet_exec_task, "fleet_exec", 4608, NULL, 4, NULL)
        != pdPASS) {
        printf("fleet: exec task FAILED to start (internal=%u free) - "
               "commands would queue forever\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        printf("whmcast: cmd v2 (%d bytes), exec internal / rx PSRAM "
               "via NVS broker, deadline exec up "
               "(internal stack, flash-safe)\n",
               (int)sizeof(whm_cmd_t));
    }
    xTaskCreate(announce_task, "whm_sync_tx", 4096, NULL, 5, NULL);

    if (s_smode == SM_SOFTAP && s_role == WHM_SYNC_CONDUCTOR) {
        whm_wifi_softap(NULL, NULL);       /* resume conducting the AP */
        ESP_LOGI(TAG, "resuming as SoftAP conductor");
    } else if (s_smode == SM_AUTO) {
        ESP_LOGI(TAG, "auto mode: prio %u, electing (STA rejoin via "
                      "saved wifi creds)", (unsigned)s_prio);
    } else if (s_role == WHM_SYNC_MEMBER) {
        ESP_LOGI(TAG, "member of '%s' (STA rejoin via saved wifi creds)",
                 s_peer_name[0] ? s_peer_name : "?");
    }
    return ESP_OK;
}

esp_err_t whm_sync_set_prio(uint8_t prio)
{
    s_prio = prio;
    whm_settings_set_u8("sync_prio", prio);
    if (prio == 0 && s_role == WHM_SYNC_CONDUCTOR && s_smode == SM_AUTO) {
        s_role = WHM_SYNC_MEMBER;          /* abdicate immediately */
        s_wall_applied = false;
        mdns_role_update();
    }
    return ESP_OK;
}

esp_err_t whm_sync_auto(void)
{
    s_smode = SM_AUTO;
    s_role = WHM_SYNC_MEMBER;
    s_promote_at = 0;
    s_anchor_name[0] = 0;
    whm_settings_set_u8("sync_mode", SM_AUTO);
    whm_settings_set_u8("sync_role", (uint8_t)s_role);
    mdns_role_update();
    ESP_LOGI(TAG, "auto mode: electing (prio %u)", (unsigned)s_prio);
    return ESP_OK;
}

esp_err_t whm_sync_anchor(void)
{
    /* conductor role WITHOUT touching wifi: for flocks living on shared
       infrastructure - every STA on the same house AP inherits that AP's
       TSF, so the anchor math holds with zero SoftAP */
    s_smode = SM_AUTO;
    s_role = WHM_SYNC_CONDUCTOR;
    if (s_prio < 200) whm_sync_set_prio(200);
    whm_settings_set_u8("sync_mode", SM_AUTO);
    whm_settings_set_u8("sync_role", (uint8_t)WHM_SYNC_MEMBER);
    whm_settings_erase("sync_peer");
    s_peer_name[0] = 0;
    strlcpy(s_anchor_name, "", sizeof(s_anchor_name));
    mdns_role_update();
    ESP_LOGI(TAG, "anchoring (prio %u) on current network (wifi "
                  "untouched; elections may preempt)", (unsigned)s_prio);
    return ESP_OK;
}

esp_err_t whm_sync_follow(const char *conductor_name)
{
    /* member with optional PIN: pinned name outranks all claims while
       alive; normal election resumes on its death */
    s_smode = SM_AUTO;
    s_role = WHM_SYNC_MEMBER;
    whm_settings_set_u8("sync_mode", SM_AUTO);
    whm_settings_set_u8("sync_role", (uint8_t)s_role);
    if (conductor_name && conductor_name[0]) {
        whm_settings_set_str("sync_peer", conductor_name);
        strlcpy(s_peer_name, conductor_name, sizeof(s_peer_name));
    }
    s_wall_applied = false;
    mdns_role_update();
    ESP_LOGI(TAG, "following '%s' on current network",
             s_peer_name[0] ? s_peer_name : "any conductor");
    return ESP_OK;
}

bool whm_sync_wall_from_flock(void) { return s_wall_applied; }

void whm_sync_name_refresh(void)
{
    char nm[16];
    my_name(nm, sizeof(nm));
    mdns_hostname_set(nm);
    mdns_instance_name_set(nm);
}

const char *whm_sync_peer(void)
{
    return s_peer_name[0] ? s_peer_name : "?";
}

int64_t whm_sync_conductor_age_us(void)
{
    if (s_role != WHM_SYNC_MEMBER || !s_last_cond_us) return -1;
    return esp_timer_get_time() - s_last_cond_us;
}

esp_err_t whm_sync_conduct(void)
{
    ESP_RETURN_ON_ERROR(whm_wifi_softap(NULL, NULL), TAG, "softap");
    s_smode = SM_SOFTAP;
    s_role = WHM_SYNC_CONDUCTOR;
    whm_settings_set_u8("sync_mode", SM_SOFTAP);
    whm_settings_set_u8("sync_role", (uint8_t)s_role);
    whm_settings_erase("sync_peer");
    s_peer_name[0] = 0;
    mdns_role_update();
    ESP_LOGI(TAG, "conducting: SSID %s, announcing on udp/%d",
             whm_wifi_ssid(), WHM_SYNC_PORT);
    return ESP_OK;
}

esp_err_t whm_sync_join(const char *conductor_name)
{
    if (!conductor_name || !conductor_name[0]) return ESP_ERR_INVALID_ARG;
    /* the conductor's SoftAP SSID is its bare node name */
    ESP_RETURN_ON_ERROR(whm_wifi_join(conductor_name,
                                      whm_wifi_ap_default_psk()),
                        TAG, "join");
    s_role = WHM_SYNC_MEMBER;
    whm_settings_set_u8("sync_role", (uint8_t)s_role);
    whm_settings_set_str("sync_peer", conductor_name);
    strlcpy(s_peer_name, conductor_name, sizeof(s_peer_name));
    s_wall_applied = false;
    mdns_role_update();
    ESP_LOGI(TAG, "joining flock of '%s'", conductor_name);
    return ESP_OK;
}

esp_err_t whm_sync_off(void)
{
    s_role = WHM_SYNC_OFF;
    s_smode = SM_OFF;
    whm_settings_set_u8("sync_mode", 0);
    whm_settings_set_u8("sync_role", 0);
    whm_settings_erase("sync_peer");
    mdns_role_update();
    ESP_LOGI(TAG, "sync role cleared (wifi left as-is)");
    return ESP_OK;
}

whm_sync_role_t whm_sync_role(void) { return s_role; }

static void mdns_role_update(void)
{
    mdns_service_txt_item_set("_whm-link", "_udp", "role",
                              role_name((uint8_t)s_role));
}

void whm_sync_discover_print(void)
{
    printf("browsing _whm-link._udp (1.5s)...\n");
    mdns_result_t *res = NULL;
    esp_err_t err = mdns_query_ptr("_whm-link", "_udp", 1500, 10, &res);
    if (err != ESP_OK || !res) {
        printf("  no nodes found\n");
        return;
    }
    for (mdns_result_t *r = res; r; r = r->next) {
        char ip[16] = "?";
        if (r->addr) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&r->addr->addr.u_addr.ip4));
        }
        const char *role = "?";
        for (size_t i = 0; i < r->txt_count; i++) {
            if (strcmp(r->txt[i].key, "role") == 0) role = r->txt[i].value;
        }
        printf("  %-15s %s.local  %s:%u  role=%s\n",
               r->instance_name ? r->instance_name : "?",
               r->hostname ? r->hostname : "?", ip, r->port, role);
    }
    mdns_query_results_free(res);
}

esp_err_t whm_sync_play_send(const char *name, const char *sha,
                             int64_t start_tsf, bool stop)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_play_t p = { 0 };
    memcpy(p.magic, "WHML", 4);
    p.ver = 1;
    p.type = 4;
    p.flags = stop ? 1 : 0;
    p.start_tsf = start_tsf;
    strlcpy(p.name, name ? name : "", sizeof(p.name));
    if (sha) memcpy(p.sha, sha, 64);
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    for (int i = 0; i < 3; i++) {           /* UDP: say it thrice */
        sendto(s_sock, &p, sizeof(p), 0, (struct sockaddr *)&dst,
               sizeof(dst));
        sync_tap((const uint8_t *)&p, (int)sizeof(p));
        if (i < 2) vTaskDelay(pdMS_TO_TICKS(60));
    }
    return ESP_OK;
}

esp_err_t whm_sync_fleet_send(const char *line)
{
    return whm_sync_fleet_send_to(NULL, line);
}

#define FQ_N 4
static struct { char line[160]; int64_t at; } s_fq[FQ_N];
static int64_t s_fq_last_at;
static volatile uint8_t s_fq_n = 0;
static portMUX_TYPE s_fq_mux = portMUX_INITIALIZER_UNLOCKED;

void whm_sync_defer_line(const char *line, int64_t at)
{
    int64_t now = whm_wifi_tsf_now();
    if (now <= 0) now = esp_timer_get_time();
    portENTER_CRITICAL(&s_fq_mux);
    if (s_fq_n < FQ_N) {
        strlcpy(s_fq[s_fq_n].line, line, sizeof(s_fq[0].line));
        s_fq[s_fq_n].at = at;
        s_fq_n++;
    }
    portEXIT_CRITICAL(&s_fq_mux);
    {
        int rem = (int)((at - now) / 1000);
        if (rem >= 0) {
            printf("fleet: queued '%s' (T-%dms)\n", line, rem);
        } else {
            printf("fleet: queued '%s' (late %dms at rx - running "
                   "now)\n", line, -rem);
        }
    }
}

/* Deadline lines run in THEIR OWN task - console handlers ran from
   the sync task for months (proven stack); 0.21.0 briefly moved them
   onto the UI task, whose stack was never sized for them. */
static void fleet_exec_task(void *arg)
{
    (void)arg;
    char fl[160];
    for (;;) {
        if (whm_sync_pop_due(fl, sizeof(fl))) {
            int64_t nw = whm_wifi_tsf_now();
            if (nw <= 0) nw = esp_timer_get_time();
            s_ctr_cmd_exec++;
            printf("fleet: exec '%s' (%+dms vs deadline)\n", fl,
                   (int)((nw - s_fq_last_at) / 1000));
            int frc = 0;
            esp_console_run(fl, &frc);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

bool whm_sync_pop_due(char *out, size_t olen)
{
    int64_t now = whm_wifi_tsf_now();
    if (now <= 0) now = esp_timer_get_time();
    bool got = false;
    portENTER_CRITICAL(&s_fq_mux);
    for (uint8_t i = 0; i < s_fq_n; i++) {
        if (s_fq[i].at <= now) {
            s_fq_last_at = s_fq[i].at;
            strlcpy(out, s_fq[i].line, olen);
            for (uint8_t j = i + 1; j < s_fq_n; j++) s_fq[j - 1] = s_fq[j];
            s_fq_n--;
            got = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_fq_mux);
    return got;
}

static uint32_t s_lead_us;

static uint32_t fleet_lead_us(void)
{
    if (!s_lead_us) {
        uint32_t ms = 333;   /* covers DTIM-3 worst (307ms) + path */
        whm_settings_get_u32("fl_lead", &ms);
        if (ms < 100) ms = 100;
        if (ms > 1000) ms = 1000;
        s_lead_us = ms * 1000;
    }
    return s_lead_us;
}

uint32_t whm_sync_lead_ms(void) { return fleet_lead_us() / 1000; }

void whm_sync_set_lead_ms(uint32_t ms)
{
    if (ms < 100) ms = 100;
    if (ms > 1000) ms = 1000;
    whm_settings_set_u32("fl_lead", ms);
    s_lead_us = ms * 1000;
}

/* WK_INPUT - whml type 11 (P1: the input spine). Applies at its
 * STAMPED step on every replica; the ui keeps a step-stamped log
 * that both live stepping and replay consume - one mechanism, so
 * late or missed events self-heal through snap-storm -> replay.
 * 36 bytes packed LE; x3 burst; (from,seq) dedupe. */

esp_err_t whm_sync_walk_input_send(uint8_t act, uint32_t exec_step,
                                   float arg)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_wki_t k = { .magic = { 'W', 'H', 'M', 'L' }, .ver = 2,
                    .type = 11, .act = act, .exec_step = exec_step,
                    .seq = ++s_wki_seq, .arg = arg };
    my_name(k.from, sizeof(k.from));
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(7777);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    for (int i = 0; i < 3; i++)
        sendto(s_sock, &k, sizeof(k), 0, (struct sockaddr *)&dst,
               sizeof(dst));
    sync_tap((const uint8_t *)&k, (int)sizeof(k));
    whm_ui_walk_input_rx(act, exec_step, arg);   /* self-apply */
    return ESP_OK;
}

/* WK_PARAMS - whml type 10 (watch agent, TO-PANEL-AGENT §4): the
 * world seed a Tier-1 viewer cannot derive. anchor is the universe;
 * same step under a different anchor is different terrain. Low-rate
 * (every ~5 s from the owner + poked on a new subscriber), never
 * baked into keyframes. 24 bytes packed LE. */
typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t ver;            /* 2 */
    uint8_t type;           /* 10 = walker params */
    uint16_t rsv;
    int64_t anchor;         /* fleet epoch anchor (fleet time base) */
    float cam_speed;        /* the walk-speed dial */
    uint8_t wver;           /* walker/world version = 2 */
    uint8_t strips;         /* fleet strip count (parallax hint) */
    uint16_t rsv2;
} whm_wkp_t;
_Static_assert(sizeof(whm_wkp_t) == 24, "wkp wire");

esp_err_t whm_sync_wkparams_send(int64_t anchor, float cam_speed,
                                 uint8_t strips)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_wkp_t k = { .magic = { 'W', 'H', 'M', 'L' }, .ver = 2,
                    .type = 10, .anchor = anchor,
                    .cam_speed = cam_speed, .wver = 2,
                    .strips = strips };
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(7777);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(s_sock, &k, sizeof(k), 0, (struct sockaddr *)&dst,
           sizeof(dst));
    sync_tap((const uint8_t *)&k, (int)sizeof(k));
    return ESP_OK;
}

esp_err_t whm_sync_wkb_send(uint8_t owner, float x, int8_t y,
                            uint8_t st, int8_t dir, uint16_t timer,
                            uint32_t step, float tgt, float vx)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_wkb_t w = { 0 };
    memcpy(w.magic, "WHML", 4);
    w.ver = 2;
    w.type = 9;
    w.owner = owner;
    w.st = st;
    w.dir = dir;
    w.y = y;
    w.timer = timer;
    w.tgt = tgt;
    w.vx = vx;
    w.seq = step;                 /* seq REPURPOSED: step number */
    w.x = x;
    my_name(w.from, sizeof(w.from));
    w.tsf = whm_wifi_tsf_now();
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(s_sock, &w, sizeof(w), 0, (struct sockaddr *)&dst,
           sizeof(dst));                  /* 10Hz stream: no burst */
    return ESP_OK;
}

esp_err_t whm_sync_mab_send(int64_t tsf, int64_t idx)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_mab_t mb = { 0 };
    memcpy(mb.magic, "WHML", 4);
    mb.ver = 2;
    mb.type = 8;
    if (!s_cmd_seq) s_cmd_seq = esp_random() | 1;
    mb.seq = ++s_cmd_seq;
    my_name(mb.from, sizeof(mb.from));
    mb.tsf = tsf;
    mb.idx = idx;
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(s_sock, &mb, sizeof(mb), 0, (struct sockaddr *)&dst,
           sizeof(dst));                  /* 2Hz stream: no burst */
    return ESP_OK;
}

esp_err_t whm_sync_nye_send(const char *tz, uint16_t year,
                            int64_t start_tsf)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_nye_t y = { 0 };
    memcpy(y.magic, "WHML", 4);
    y.ver = 2;
    y.type = 7;
    y.year = year;
    if (!s_cmd_seq) s_cmd_seq = esp_random() | 1;
    y.seq = ++s_cmd_seq;
    my_name(y.from, sizeof(y.from));
    strlcpy(y.tz, tz ? tz : "", sizeof(y.tz));
    y.start_tsf = start_tsf;
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    for (int i = 0; i < 3; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(40));
        sendto(s_sock, &y, sizeof(y), 0, (struct sockaddr *)&dst,
               sizeof(dst));
        sync_tap((const uint8_t *)&y, (int)sizeof(y));
    }
    return ESP_OK;
}

esp_err_t whm_sync_djb_send(bool playing, uint32_t rate, uint8_t ch,
                            const char *title)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_djb_t d = { 0 };
    memcpy(d.magic, "WHML", 4);
    d.ver = 2;
    d.type = 6;
    d.playing = playing ? 1 : 0;
    d.ch = ch;
    d.rate = rate;
    if (!s_cmd_seq) s_cmd_seq = esp_random() | 1;
    d.seq = ++s_cmd_seq;
    my_name(d.from, sizeof(d.from));
    strlcpy(d.title, title ? title : "", sizeof(d.title));
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    for (int i = 0; i < 3; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(50));
        sendto(s_sock, &d, sizeof(d), 0, (struct sockaddr *)&dst,
               sizeof(dst));
        sync_tap((const uint8_t *)&d, (int)sizeof(d));
    }
    return ESP_OK;
}

void whm_sync_brief(int *role, int *fresh, int *stale,
                    uint32_t *anchor_age_ms)
{
    int64_t now = esp_timer_get_time();
    int fr = 0, st = 0;
    uint32_t aage = 0xFFFFFFFFu;
    /* the lead's name lives in s_anchor_name under ELECTIONS and in
       s_peer_name under SOFTAP conduct/join - match either (the
       photo: 'member of One, heard 0s ago' wearing the red hollow) */
    const char *lead = s_anchor_name[0] ? s_anchor_name
                                        : s_peer_name;
    for (int i = 0; i < PEER_MAX; i++) {
        if (!s_peers[i].name[0]) continue;
        int64_t age = now - s_peers[i].last_us;
        if (age < 8000000) fr++;            /* 8s/20s: 2-3 missed
        else if (age < 20000000) st++;         announces before
                                               judgment */
        if (lead[0] &&
            strncmp(s_peers[i].name, lead, 16) == 0) {
            aage = (uint32_t)(age / 1000);
        }
    }
    if (role) {
        /* GLYPH TRUTH (owner's question found the bug): an
           elections ANCHOR carries internal s_role=CONDUCTOR (the
           pre-elections enum), so checking role first made the
           anchor glyph UNREACHABLE - every leader wore the baton.
           The dialect decides: SM_AUTO leader = anchor glyph (1),
           conduct/join leader = baton (2). */
        bool lead2 = (s_role == WHM_SYNC_CONDUCTOR);
        *role = lead2 ? (s_smode == SM_AUTO ? 1 : 2) : 0;
    }
    if (fresh) *fresh = fr;
    if (stale) *stale = st;
    if (anchor_age_ms) *anchor_age_ms = aage;
}

const char *whm_sync_node_name(void)
{
    static char n[16];
    my_name(n, sizeof(n));
    return n;
}

esp_err_t whm_sync_fleet_send_to(const char *target, const char *line)
{
    if (s_sock < 0) return ESP_ERR_INVALID_STATE;
    whm_cmd_t c = { 0 };
    memcpy(c.magic, "WHML", 4);
    c.ver = 2;
    c.type = 3;
    /* Lead budget is a per-rig DIAL ('sync lead <ms>', NVS): DTIM-3
       infra needs ~307ms worst-case; SoftAP flocks need almost none.
       Default 333ms: clears DTIM-3 worst-case; dial for other rigs. */
    c.exec_at = whm_wifi_tsf_now() + (int64_t)fleet_lead_us();
    /* ONE TIMESTAMP, TWO CONSUMERS: the sender self-defers HERE with
       the exact exec_at stamped into the packet. (The console used to
       defer separately with a forgotten hardcoded +150ms - so the
       anchor always fired early and RAISING the lead WIDENED the skew:
       lead-150 = 100/183/350ms at 250/333/500, the exact lags seen.) */
    {
        char me[17] = "";
        my_name(me, sizeof(me));
        if (!target || !target[0] || strcasecmp(target, me) == 0) {
            whm_sync_defer_line(line, c.exec_at);
        }
    }
    if (target && target[0]) strlcpy(c.target, target, sizeof(c.target));
    if (!s_cmd_seq) s_cmd_seq = esp_random() | 1;
    c.seq = ++s_cmd_seq;
    my_name(c.from, sizeof(c.from));
    strlcpy(c.line, line, sizeof(c.line));
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    /* Burst x3 (0/60/120ms): broadcasts are unACKed at the lowest
       rate; the dedupe ring makes repeats free, so redundancy buys
       real delivery. ENOMEM (lwIP pbufs, seen on bench) gets a
       backoff-retry per shot. */
    int sent = 0;
    for (int shot = 0; shot < 3; shot++) {
        if (shot) vTaskDelay(pdMS_TO_TICKS(60));
        int n = -1;
        for (int a = 0; a < 3; a++) {
            n = sendto(s_sock, &c, sizeof(c), 0,
                       (struct sockaddr *)&dst, sizeof(dst));
            if (n == (int)sizeof(c) || errno != ENOMEM) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (n == (int)sizeof(c)) sent++;
        else printf("fleet: tx shot %d FAILED (%d/%d, errno %d)\n",
                    shot + 1, n, (int)sizeof(c), errno);
    }
    if (sent) {
        printf("fleet: tx %dB cmd v%u x%d -> broadcast:7777\n",
               (int)sizeof(c), c.ver, sent);
        return ESP_OK;
    }
    return ESP_FAIL;
}

void whm_sync_life_send(uint8_t idx, uint8_t side, uint32_t gen,
                        const uint8_t *col64)
{
    if (s_sock < 0) return;
    whm_life_edge_t e;
    memcpy(e.magic, "WHML", 4);
    e.ver = 1;
    e.type = 2;
    e.idx = idx;
    e.side = side;
    e.gen = gen;
    memcpy(e.cells, col64, 64);
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(WHM_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(s_sock, &e, sizeof(e), 0, (struct sockaddr *)&dst, sizeof(dst));
}

bool whm_sync_life_edge(uint8_t peer_idx, uint8_t side, uint8_t *out64,
                        uint32_t *gen)
{
    if (peer_idx >= 8 || side >= 2) return false;
    edge_store_t *st = &s_edges[peer_idx][side];
    if (!st->have) return false;
    memcpy(out64, st->cells, 64);
    if (gen) *gen = st->gen;
    return true;
}

void whm_sync_status_print(void)
{
    printf("fw:        %s (me)\n", WHM_VERSION_STR);
    printf("mode:      %s\n", s_smode == SM_SOFTAP ? "softap"
                              : s_smode == SM_AUTO ? "auto (elections)"
                                                   : "off");
    if (s_smode == SM_AUTO) {
        printf("prio:      %u%s\n", (unsigned)s_prio,
               s_prio == 0 ? " (follower-only)" : "");
        printf("anchor:    %s%s\n", whm_sync_anchor_name(),
               s_role == WHM_SYNC_CONDUCTOR ? " (me)" : "");
    }
    printf("role:      %s", role_name((uint8_t)s_role));
    if (s_role == WHM_SYNC_MEMBER) {
        printf(" of '%s'", s_peer_name[0] ? s_peer_name : "?");
    }
    printf("\n");
    if (s_role == WHM_SYNC_CONDUCTOR) {
        int st = whm_wifi_ap_stations();
        printf("ssid:      %s (%d station%s)\n", whm_wifi_ssid(),
               st < 0 ? 0 : st, st == 1 ? "" : "s");
    }
    int64_t tsf = whm_wifi_tsf_now();
    if (tsf > 0) printf("tsf:       %lld us\n", (long long)tsf);
    if (s_role == WHM_SYNC_MEMBER) {
        if (s_last_cond_us) {
            printf("%s: heard %llds ago; wall clock %s\n", leader_word(),
                   (long long)((esp_timer_get_time() - s_last_cond_us)
                               / 1000000),
                   s_wall_applied ? "taken from flock" : "own (SNTP)");
        } else {
            printf("%s: not heard yet\n", leader_word());
        }
    }
    int np = 0;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < PEER_MAX; i++) {
        if (!s_peers[i].name[0]) continue;
        int64_t age = (now - s_peers[i].last_us) / 1000000;
        if (age > 30) continue;            /* stale */
        if (!np) printf("peers:\n");
        np++;
        printf("  %-15s %-9s fw %-8s %llds ago  %.1fC %.0f%%\n",
               s_peers[i].name, role_name(s_peers[i].role), s_peers[i].fw,
               (long long)age, s_peers[i].temp_c, s_peers[i].rh);
    }
    if (!np) printf("peers:     none heard in 30s\n");
    if (s_smode == SM_OFF && s_role == WHM_SYNC_OFF) {
        int vis = 0;
        int64_t now2 = esp_timer_get_time();
        for (int i = 0; i < PEER_MAX; i++)
            if (s_peers[i].name[0] &&
                now2 - s_peers[i].last_us < 30000000LL) vis++;
        if (vis)
            printf("        (mode off: you can SEE the fleet; it "
                   "cannot see you - 'sync auto' to join)\n");
    }
    printf("lead:   %u ms ('sync lead <ms>' 100-1000)\n",
           (unsigned)(fleet_lead_us() / 1000));
    printf("rx:     ann=%u life=%u cmd=%u play=%u\n",
           (unsigned)s_ctr_rx_ann, (unsigned)s_ctr_rx_life,
           (unsigned)s_ctr_rx_cmd_alias(), (unsigned)s_ctr_rx_play);
    printf("fleet:  exec=%u q=%u | drop: ver=%u size=%u dup=%u self=%u\n",
           (unsigned)s_ctr_cmd_exec, (unsigned)s_fq_n,
           (unsigned)s_ctr_drop_ver, (unsigned)s_ctr_drop_size,
           (unsigned)s_ctr_drop_dup, (unsigned)s_ctr_drop_self);
}
