/*
 * mp3_player.c - MP3 playback from /sdcard/media with ID3v2 tags and
 * album art on the panel.
 *
 * Decode: minimp3 (lieff, CC0; vendored as main/minimp3.h).
 * Album art: JPEG APIC frames decoded by the ESP32-S3 mask-ROM TJpgDec
 * (RGB888 output, zero flash cost), scaled to the 64x64 canvas. PNG art
 * gets a drawn placeholder (v1 limitation). Art > 512KB skipped.
 * Tags: ID3v2.3/2.4 TIT2/TPE1; encodings 0/3 copied, UTF-16 flattened to
 * ASCII (the 5x7 font is ASCII anyway).
 * Speaker: single-owner API in audio_test.c; beeps yield while playing.
 */
#include "mp3_player.h"
#include "http_svc.h"
#include "wifi_tsf.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include <strings.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "sync.h"
#include <math.h>
#include "esp32s3/rom/tjpgd.h"

#include "audio_test.h"
#include "storage_test.h"
#include "settings.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

static const char *TAG = "whm_mp3";

#define WHM_SD_MOUNT "/sdcard"
#define MEDIA_DIR   WHM_SD_MOUNT "/media"
#define MAX_TRACKS  128
#define ART_W       64
#define ART_MAX     (512 * 1024)
#define INBUF_SZ    (16 * 1024)

typedef struct {
    char fname[96];
    char title[48];
    char artist[48];
    uint32_t fsize;
    uint32_t audio_off;      /* first byte after ID3v2 */
    uint32_t apic_off;       /* 0 = none */
    uint32_t apic_len;
    bool apic_jpeg;
    bool at_root;            /* true: file lives at /sdcard, not /media */
} track_t;

typedef enum { OP_PLAY, OP_STOP, OP_PAUSE, OP_RESUME, OP_NEXT, OP_PREV,
               OP_VOL, OP_FLEET } op_t;
typedef struct { op_t op; int arg; } cmd_t;


static track_t *s_tracks = NULL;
static int s_count = 0;
static int s_cur = -1;
static volatile whm_mp3_state_t s_state = WHM_MP3_STOPPED;
static QueueHandle_t s_q = NULL;
static uint8_t s_vol = 70;

static uint8_t *s_art = NULL;            /* 64*64*3, PSRAM */
static bool s_art_valid = false;



static FILE *s_f = NULL;
EXT_RAM_BSS_ATTR static mp3dec_t s_dec;
static uint8_t *s_in;                 /* INBUF_SZ bytes, PSRAM */
static int s_in_len = 0;
static bool s_in_eof = false;
static uint32_t s_pos_bytes = 0, s_audio_bytes = 1;
static uint32_t s_kbps_sum = 0, s_kbps_n = 0;
static int s_hz = 44100, s_ch = 2;

/* ---- Mode A sample-slaving engine -----------------------------------
 * Decode outruns realtime, so spk_write blocks on DMA space and buffer
 * occupancy pins FULL on every unit - a written-samples counter is a
 * valid playback clock fleet-wide (constant offset to the DAC). We
 * discipline CONTENT position: content_due = (tsf-start)*hz. Ahead ->
 * hold (write pad samples, content pauses). Behind -> drop source
 * frames (content jumps, nothing written). Corrections <=2ms, threshold
 * 500us, so +/-20ppm crystals stay bounded forever. */
static volatile int64_t s_fs_start = 0;      /* start_tsf; 0 = local play */
static int64_t s_fs_content = 0;
static volatile int64_t s_fs_err_us = 0;
static bool s_fs_waited = false;
static bool s_fs_primed = false;             /* pipe filled + rebased */
static int64_t s_fs_prime_until = 0;
static char s_fs_name[64];

static int64_t (*s_due_fn)(int64_t now);   /* Mode B / A-beacon due */
static int64_t bf_due(int64_t now);        /* Mode B, defined below */
static bool s_fs_master;                   /* I issued this fleet play:
                                              free-run + beacon truth */
static char s_fs_master_name[17];          /* whose beacons to trust */
static volatile int64_t s_ma_ref_tsf, s_ma_ref_idx;
static int64_t s_ma_last_tx;
static uint32_t s_ma_bcn_n;              /* beacons heard this run */
static int64_t s_ma_bcn_us;              /* local rx time of last */
static volatile bool s_ma_diag = true;   /* 1 Hz [A] ledger */
void whm_mp3_diag(bool on) { s_ma_diag = on; }

static int64_t ma_beacon_due(int64_t now)
{
    return s_ma_ref_idx +
           (now - s_ma_ref_tsf) * (int64_t)s_hz / 1000000LL;
}

void whm_mp3_a_on_beacon(const char *from, int64_t tsf, int64_t idx)
{
    if (s_fs_master || s_fs_start <= 0) return;
    if (s_fs_master_name[0] &&
        strcasecmp(from, s_fs_master_name) != 0) return;
    s_ma_ref_tsf = tsf;
    s_ma_ref_idx = idx;
    s_ma_bcn_n++;
    s_ma_bcn_us = esp_timer_get_time();
    if (s_due_fn != ma_beacon_due && s_due_fn != bf_due) {
        s_due_fn = ma_beacon_due;   /* upgrade: chase the MASTER's
                                       real audio, not the formula */
        printf("[modeA] locked to %s's beacon (real-audio sync)\n",
               from);
    }
}
static float s_rs_pos = 0.0f;            /* fractional read position */
static int16_t s_rs_prevL, s_rs_prevR;   /* carry across calls */
static float s_rs_ppm = 0.0f;            /* servo output, for the UI */
static int64_t s_rs_srv_t = 0;
static int16_t *s_rs_out;                /* PSRAM, frames+slack */

float whm_mp3_sync_ppm(void) { return s_rs_ppm; }
bool whm_mp3_is_master(void) { return s_fs_master && s_fs_start > 0; }

/* ================= MODE B: DJ streaming (anchor tees PCM) =========
 * Anchor: 'mp3 dj [n]' plays locally AND tees decoded PCM into a
 * ring; a tiny TCP server (:7778, select-loop, <=3 clients, own
 * cursors) frames it as [WHMT marker: tsf + abs sample idx][4800
 * stereo samples] every 100ms. Followers auto-tune on the type-6
 * packet, buffer 150ms in PSRAM, and feed engine v3 UNCHANGED - the
 * servo locks to marker-time exactly as it locks to downbeat-time. */
static void eng_write(const int16_t *out, int frames);   /* below */

#define DJ_PORT      7778
#define DJ_SPAN      4800                 /* samples per marker block */
#define DJ_RING_S    (48000 * 2 / 3)      /* ~660ms tee ring */
#define BF_RING_S    (48000 / 2)          /* follower: 500ms */

static struct {
    volatile bool on;
    int16_t *ring;                        /* PSRAM, stereo interleaved */
    volatile int64_t widx;                /* absolute sample index */
    volatile int64_t wtsf;                /* tsf at widx */
    TaskHandle_t srv;
} s_dj;

static struct {
    volatile bool on;
    int16_t *ring;
    volatile int64_t widx, ridx;          /* absolute sample indices */
    volatile int64_t ref_tsf, ref_idx;    /* latest marker pair */
    uint32_t rate;
    char host[36], title[64];
    TaskHandle_t cli, pump;
} s_bf;

static void dj_tee(const int16_t *pcm, int frames)
{
    if (!s_dj.on || !s_dj.ring) return;
    int64_t w = s_dj.widx;
    for (int i = 0; i < frames; i++) {
        int64_t p = (w + i) % DJ_RING_S;
        s_dj.ring[2 * p] = pcm[2 * i];
        s_dj.ring[2 * p + 1] = pcm[2 * i + 1];
    }
    s_dj.widx = w + frames;
    s_dj.wtsf = whm_wifi_tsf_now();
}

static void dj_server_task(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_port = htons(DJ_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind(ls, (struct sockaddr *)&a, sizeof(a));
    listen(ls, 3);
    struct { int fd; int64_t cur; } cl[3] = { { -1, 0 }, { -1, 0 },
                                              { -1, 0 } };
    int16_t *blk = heap_caps_malloc(24 + (size_t)DJ_SPAN * 4,
                                    MALLOC_CAP_SPIRAM);
    printf("dj: stream server on :%d\n", DJ_PORT);
    while (s_dj.on) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        struct timeval tv = { 0, 20000 };
        if (select(ls + 1, &rf, NULL, NULL, &tv) > 0) {
            int fd = accept(ls, NULL, NULL);
            for (int i = 0; i < 3 && fd >= 0; i++) {
                if (cl[i].fd < 0) {
                    cl[i].fd = fd;
                    cl[i].cur = s_dj.widx - DJ_SPAN;
                    if (cl[i].cur < 0) cl[i].cur = 0;
                    printf("dj: client %d tuned in\n", i);
                    fd = -1;
                }
            }
            if (fd >= 0) close(fd);
        }
        static int reann = 0;
        if (++reann >= 100) {             /* ~2s: late joiners tune in */
            reann = 0;
            char t2[64];
            whm_mp3_now_title(t2, sizeof(t2));
            whm_sync_djb_send(true, (uint32_t)s_hz, 2, t2);
        }
        for (int i = 0; i < 3; i++) {
            if (cl[i].fd < 0) continue;
            if (s_dj.widx - cl[i].cur < DJ_SPAN) continue;
            if (s_dj.widx - cl[i].cur > DJ_RING_S - DJ_SPAN) {
                cl[i].cur = s_dj.widx - 2 * DJ_SPAN;   /* lagged: skip */
            }
            uint8_t *hb = (uint8_t *)blk;
            memcpy(hb, "WHMT", 4);
            int64_t mtsf = s_dj.wtsf -
                (int64_t)(s_dj.widx - cl[i].cur) * 1000000LL /
                    (int64_t)s_hz;
            memcpy(hb + 4, &mtsf, 8);
            memcpy(hb + 12, &cl[i].cur, 8);
            memset(hb + 20, 0, 4);
            for (int k = 0; k < DJ_SPAN; k++) {
                int64_t p = (cl[i].cur + k) % DJ_RING_S;
                blk[12 + 2 * k] = s_dj.ring[2 * p];
                blk[12 + 2 * k + 1] = s_dj.ring[2 * p + 1];
            }
            int want = 24 + DJ_SPAN * 4;
            int off = 0;
            while (off < want) {
                int n = send(cl[i].fd, (uint8_t *)blk + off,
                             (size_t)(want - off), 0);
                if (n <= 0) { close(cl[i].fd); cl[i].fd = -1; break; }
                off += n;
            }
            if (cl[i].fd >= 0) cl[i].cur += DJ_SPAN;
        }
    }
    for (int i = 0; i < 3; i++) if (cl[i].fd >= 0) close(cl[i].fd);
    close(ls);
    free(blk);
    vTaskDelete(NULL);
}

static int64_t bf_due(int64_t now)
{
    return s_bf.ref_idx +
           (now - s_bf.ref_tsf) * (int64_t)s_bf.rate / 1000000LL;
}

static bool rd_full(int fd, uint8_t *b, int n)
{
    int off = 0;
    while (off < n) {
        int r = recv(fd, b + off, (size_t)(n - off), 0);
        if (r <= 0) return false;
        off += r;
    }
    return true;
}

static void bf_client_task(void *arg)
{
    (void)arg;
    struct addrinfo hints = { .ai_family = AF_INET,
                              .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%d", DJ_PORT);
    int fd = -1;
    if (getaddrinfo(s_bf.host, port, &hints, &res) == 0 && res) {
        fd = socket(res->ai_family, res->ai_socktype, 0);
        if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen)) {
            close(fd);
            fd = -1;
        }
        freeaddrinfo(res);
    }
    if (fd < 0) {
        printf("mode B: connect to %s:%d failed\n", s_bf.host, DJ_PORT);
        s_bf.on = false;
        vTaskDelete(NULL);
        return;
    }
    printf("mode B: tuned to %s ('%s')\n", s_bf.host, s_bf.title);
    uint8_t hdr[24];
    int16_t *pl = heap_caps_malloc((size_t)DJ_SPAN * 4,
                                   MALLOC_CAP_SPIRAM);
    while (s_bf.on && pl) {
        if (!rd_full(fd, hdr, 24)) break;
        if (memcmp(hdr, "WHMT", 4) != 0) break;
        int64_t mtsf, midx;
        memcpy(&mtsf, hdr + 4, 8);
        memcpy(&midx, hdr + 12, 8);
        if (!rd_full(fd, (uint8_t *)pl, DJ_SPAN * 4)) break;
        s_bf.ref_tsf = mtsf;
        s_bf.ref_idx = midx;
        for (int k = 0; k < DJ_SPAN; k++) {
            int64_t p = (midx + k) % BF_RING_S;
            s_bf.ring[2 * p] = pl[2 * k];
            s_bf.ring[2 * p + 1] = pl[2 * k + 1];
        }
        s_bf.widx = midx + DJ_SPAN;
    }
    close(fd);
    free(pl);
    printf("mode B: stream ended\n");
    s_bf.on = false;
    vTaskDelete(NULL);
}

static void bf_pump_task(void *arg)
{
    (void)arg;
    while (s_bf.on && s_bf.widx - s_bf.ridx <
                          (int64_t)s_bf.rate * 150 / 1000) {
        vTaskDelay(pdMS_TO_TICKS(10));   /* prime ~150ms */
    }
    if (s_bf.on) {
        whm_audio_spk_open((int)s_bf.rate, 2, whm_audio_sys_vol());
        s_hz = (int)s_bf.rate;
        s_bf.ridx = s_bf.widx - (int64_t)s_bf.rate * 150 / 1000;
        s_fs_content = bf_due(whm_wifi_tsf_now());
        s_rs_pos = 0;
        s_rs_ppm = 0;
        s_due_fn = bf_due;
        s_fs_waited = true;
        s_fs_primed = true;
        s_fs_start = 1;                   /* engine gate on */
        printf("mode B: locked, playing\n");
    }
    int16_t buf[576 * 2];
    while (s_bf.on) {
        int64_t avail = s_bf.widx - s_bf.ridx;
        int want = avail > 576 ? 576 : (int)avail;
        if (want <= 0) {
            memset(buf, 0, sizeof(int16_t) * 2 * 192);
            eng_write(buf, 192);          /* underrun: silence */
            continue;
        }
        for (int k = 0; k < want; k++) {
            int64_t p = (s_bf.ridx + k) % BF_RING_S;
            buf[2 * k] = s_bf.ring[2 * p];
            buf[2 * k + 1] = s_bf.ring[2 * p + 1];
        }
        s_bf.ridx += want;
        eng_write(buf, want);
    }
    s_fs_start = 0;
    s_due_fn = NULL;
    whm_audio_spk_close();
    vTaskDelete(NULL);
}

void whm_mp3_b_on_pkt(const char *from, uint32_t rate, uint8_t ch,
                      bool playing, const char *title)
{
    (void)ch;
    if (!playing) {
        if (s_bf.on) printf("mode B: dj stopped\n");
        s_bf.on = false;
        return;
    }
    if (s_bf.on || s_dj.on) return;       /* already tuned / I'm the dj */
    if (!s_bf.ring) {
        s_bf.ring = heap_caps_malloc((size_t)BF_RING_S * 4,
                                     MALLOC_CAP_SPIRAM);
        if (!s_bf.ring) return;
    }
    snprintf(s_bf.host, sizeof(s_bf.host), "%s.local", from);
    strlcpy(s_bf.title, title, sizeof(s_bf.title));
    s_bf.rate = rate ? rate : 48000;
    s_bf.widx = s_bf.ridx = 0;
    s_bf.on = true;
    xTaskCreate(bf_client_task, "bf_cli", 4096, NULL, 6, &s_bf.cli);
    xTaskCreate(bf_pump_task, "bf_pump", 4096, NULL, 7, &s_bf.pump);
}

void whm_mp3_now_title(char *out, size_t n)
{
    char a[48];
    if (s_cur >= 0 &&
        whm_mp3_track_info(s_cur, out, n, a, sizeof(a)) == ESP_OK &&
        out[0]) {
        return;
    }
    strlcpy(out, "(untitled)", n);
}

bool whm_mp3_dj_active(void) { return s_dj.on; }

esp_err_t whm_mp3_dj_start(int n)
{
    if (s_dj.on) return ESP_OK;
    if (!s_dj.ring) {
        s_dj.ring = heap_caps_malloc((size_t)DJ_RING_S * 4,
                                     MALLOC_CAP_SPIRAM);
        if (!s_dj.ring) return ESP_ERR_NO_MEM;
    }
    s_dj.widx = 0;
    s_dj.on = true;
    xTaskCreate(dj_server_task, "dj_srv", 4096, NULL, 6, &s_dj.srv);
    whm_mp3_play(n);
    return ESP_OK;
}

void whm_mp3_dj_stop(void)
{
    s_dj.on = false;
    whm_mp3_stop();
}

/* engine v3: RATE-SERVO. v2 corrected with discrete splices (2ms
 * last-sample holds / drops) - each one an audible tick whenever TSF
 * and the DAC crystal disagreed. v3 resamples continuously: step =
 * 1 +- delta, delta a slew-limited PI-ish servo on the same error,
 * clamped +-150ppm. Discontinuities are impossible; |e| converges to
 * ~250us and stays. Holds/drops survive only as a >50ms emergency. */
static void eng_write(const int16_t *out, int frames)
{
    if (s_fs_start > 0 && s_hz > 0) {
        if (!s_fs_waited) {
            int64_t t;
            while ((t = whm_wifi_tsf_now()) > 0 && t < s_fs_start) {
                int64_t left = s_fs_start - t;
                vTaskDelay(pdMS_TO_TICKS(left > 4000 ? 2 : 1));
            }
            s_fs_waited = true;
            s_fs_primed = false;
            s_fs_content = 0;
            s_rs_pos = 0.0f;
            s_rs_prevL = s_rs_prevR = 0;
            s_rs_ppm = 0.0f;
            s_fs_prime_until = s_fs_start + 400000LL;
        }
        int64_t now = whm_wifi_tsf_now();
        if (s_ma_diag) {             /* the owner's 1 Hz ledger */
            static int64_t dgl;
            if (now - dgl > 1000000) {
                dgl = now;
                if (s_fs_master) {
                    printf("[A] M cont=%lld tx@500ms hz=%d\n",
                           (long long)s_fs_content, s_hz);
                } else {
                    int64_t age = s_ma_bcn_us
                        ? (esp_timer_get_time() - s_ma_bcn_us)
                              / 1000 : -1;
                    printf("[A] F ppm=%+.1f err=%+lldus "
                           "cont=%lld bcn=%u age=%lldms %s\n",
                           (double)s_rs_ppm,
                           (long long)s_fs_err_us,
                           (long long)s_fs_content,
                           (unsigned)s_ma_bcn_n,
                           (long long)age,
                           s_due_fn ? "LOCKED" : "formula");
                }
            }
        }
        int64_t due = s_due_fn ? s_due_fn(now)
                    : ((now - s_fs_start) * s_hz) / 1000000LL;
        if (!s_fs_primed) {
            if (now >= s_fs_prime_until) {
                s_fs_content = due;      /* rebase once; servo holds */
                s_fs_primed = true;
                s_rs_srv_t = now;
                ESP_LOGI(TAG, "mode A: primed + rebased, servo on");
            }
        } else if (s_fs_master) {
            /* MASTER FREE-RUN: my DAC crystal IS the truth. No servo,
               no resampler - pristine passthrough - and a 500ms
               type-8 beacon publishes (tsf, content) so followers
               lock to my REAL audio. */
            s_fs_err_us = 0;
            if (now - s_ma_last_tx > 500000) {
                s_ma_last_tx = now;
                whm_sync_mab_send(now, s_fs_content);
            }
            whm_audio_spk_write(out, (size_t)frames * 2 *
                                         sizeof(int16_t));
            s_fs_content += frames;
            return;
        } else {
            int64_t e_us = (s_fs_content - due) * 1000000LL / s_hz;
            s_fs_err_us = e_us;
            if (e_us > 50000 || e_us < -50000) {
                s_fs_content = due;      /* wifi stall etc. */
                s_rs_ppm = 0.0f;
                ESP_LOGW(TAG, "mode A: hard resync (%lldus) - TSF "
                         "wobble or stall; follower re-locks",
                         (long long)e_us);
            } else if (now - s_rs_srv_t >= 100000) {
                s_rs_srv_t = now;
                float tgt = -(float)e_us * 0.35f;
                if (tgt > 150.0f) tgt = 150.0f;
                if (tgt < -150.0f) tgt = -150.0f;
                s_rs_ppm += 0.30f * (tgt - s_rs_ppm);   /* slewed */
            }
        }
        if (!s_rs_out) {
            s_rs_out = heap_caps_malloc(
                (MINIMP3_MAX_SAMPLES_PER_FRAME + 16) * 2 *
                    sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!s_rs_out) { whm_audio_spk_write(out,
                (size_t)frames * 4); s_fs_content += frames; return; }
        }
        float step = 1.0f + s_rs_ppm * 1e-6f;
        float p = s_rs_pos;
        int oi = 0;
        while (p + 1.0f < (float)frames &&
               oi < MINIMP3_MAX_SAMPLES_PER_FRAME + 12) {
            int ii = (int)p;
            float f = p - (float)ii;
            int16_t L0 = ii < 0 ? s_rs_prevL : out[2 * ii];
            int16_t R0 = ii < 0 ? s_rs_prevR : out[2 * ii + 1];
            int16_t L1 = out[2 * (ii + 1) < 2 * frames ? 2 * (ii + 1)
                                                        : 2 * ii];
            int16_t R1 = out[2 * (ii + 1) < 2 * frames
                                 ? 2 * (ii + 1) + 1 : 2 * ii + 1];
            s_rs_out[2 * oi] = (int16_t)(L0 + f * (float)(L1 - L0));
            s_rs_out[2 * oi + 1] =
                (int16_t)(R0 + f * (float)(R1 - R0));
            oi++;
            p += step;
        }
        s_rs_pos = p - (float)frames;
        if (frames > 0) {
            s_rs_prevL = out[2 * (frames - 1)];
            s_rs_prevR = out[2 * (frames - 1) + 1];
        }
        if (oi > 0) {
            whm_audio_spk_write(s_rs_out, (size_t)oi * 2 *
                                              sizeof(int16_t));
            s_fs_content += oi;
        }
        return;
    }
    whm_audio_spk_write(out, (size_t)frames * 2 * sizeof(int16_t));
}

static int16_t *s_pcm;                 /* MINIMP3_MAX_SAMPLES_PER_FRAME bytes, PSRAM */
static int16_t *s_pcm2;   /* frame*2 int16, PSRAM */

/* ------------------------------------------------------------- ID3 scan */

static uint32_t syncsafe(const uint8_t *b)
{
    return ((uint32_t)(b[0] & 0x7f) << 21) | ((uint32_t)(b[1] & 0x7f) << 14) |
           ((uint32_t)(b[2] & 0x7f) << 7) | (b[3] & 0x7f);
}

static void copy_text(char *dst, size_t dstlen, const uint8_t *src,
                      size_t n, uint8_t enc)
{
    size_t o = 0;
    if (enc == 1 || enc == 2) {          /* UTF-16 (w/wo BOM) -> ASCII */
        size_t i = 0;
        int lo_first = 1;
        if (n >= 2 && src[0] == 0xff && src[1] == 0xfe) { i = 2; lo_first = 1; }
        else if (n >= 2 && src[0] == 0xfe && src[1] == 0xff) { i = 2; lo_first = 0; }
        else if (enc == 2) lo_first = 0;
        for (; i + 1 < n && o + 1 < dstlen; i += 2) {
            uint16_t u = lo_first ? (src[i] | (src[i + 1] << 8))
                                  : ((src[i] << 8) | src[i + 1]);
            if (u == 0) break;
            dst[o++] = (u >= 0x20 && u < 0x7f) ? (char)u : '?';
        }
    } else {                             /* ISO-8859-1 / UTF-8: keep ASCII */
        for (size_t i = 0; i < n && o + 1 < dstlen; i++) {
            uint8_t c = src[i];
            if (c == 0) break;
            dst[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        }
    }
    dst[o] = 0;
}

static void parse_id3(FILE *f, track_t *t)
{
    uint8_t h[10];
    t->audio_off = 0;
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) return;
    uint8_t ver = h[3];
    uint32_t tag_sz = syncsafe(h + 6);
    uint32_t end = 10 + tag_sz;
    t->audio_off = end;

    uint32_t pos = 10;
    while (pos + 10 < end) {
        uint8_t fh[10];
        if (fseek(f, pos, SEEK_SET) != 0 || fread(fh, 1, 10, f) != 10) break;
        if (fh[0] == 0) break;
        uint32_t fsz = (ver == 4) ? syncsafe(fh + 4)
                                  : ((uint32_t)fh[4] << 24) | (fh[5] << 16) |
                                        (fh[6] << 8) | fh[7];
        if (fsz == 0 || pos + 10 + fsz > end) break;

        if (memcmp(fh, "TIT2", 4) == 0 || memcmp(fh, "TPE1", 4) == 0) {
            uint8_t buf[128];
            size_t n = fsz < sizeof(buf) ? fsz : sizeof(buf);
            if (fread(buf, 1, n, f) == n && n > 1) {
                char *dst = (fh[1] == 'I') ? t->title : t->artist;
                copy_text(dst, 48, buf + 1, n - 1, buf[0]);
            }
        } else if (memcmp(fh, "APIC", 4) == 0) {
            uint8_t buf[300];
            size_t n = fsz < sizeof(buf) ? fsz : sizeof(buf);
            if (fread(buf, 1, n, f) == n && n > 3) {
                uint8_t enc = buf[0];
                size_t i = 1;
                char mime[32] = "";
                size_t mi = 0;
                while (i < n && buf[i] && mi + 1 < sizeof(mime)) {
                    mime[mi++] = (char)buf[i++];
                }
                mime[mi] = 0;
                i++;                       /* mime NUL */
                i++;                       /* picture type */
                if (enc == 1 || enc == 2) {
                    while (i + 1 < n && !(buf[i] == 0 && buf[i + 1] == 0)) i += 2;
                    i += 2;
                } else {
                    while (i < n && buf[i]) i++;
                    i++;
                }
                if (i < fsz) {
                    t->apic_off = pos + 10 + (uint32_t)i;
                    t->apic_len = fsz - (uint32_t)i;
                    t->apic_jpeg = (strcasestr(mime, "jpeg") != NULL) ||
                                   (strcasestr(mime, "jpg") != NULL);
                }
            }
        }
        pos += 10 + fsz;
    }
}

static int scan_dir(const char *dir, bool at_root, int *ignored)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_TRACKS) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcasecmp(e->d_name + l - 4, ".mp3") != 0) {
            if (e->d_type != DT_DIR && ignored) (*ignored)++;
            continue;
        }
        if (l >= sizeof(s_tracks[0].fname)) {
            ESP_LOGW(TAG, "skipping (name too long): %s", e->d_name);
            continue;
        }
        track_t *t = &s_tracks[s_count];
        memset(t, 0, sizeof(*t));
        strlcpy(t->fname, e->d_name, sizeof(t->fname));
        t->at_root = at_root;

        char path[160];
        snprintf(path, sizeof(path), "%s/%s", dir, t->fname);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        struct stat st;
        if (stat(path, &st) == 0) t->fsize = (uint32_t)st.st_size;
        parse_id3(f, t);
        fclose(f);
        if (!t->title[0]) {
            strlcpy(t->title, t->fname, sizeof(t->title));
            char *dot = strrchr(t->title, '.');
            if (dot) *dot = 0;
        }
        s_count++;
        found++;
    }
    closedir(d);
    return found;
}

static int track_cmp(const void *a, const void *b)
{
    return strcasecmp(((const track_t *)a)->fname,
                      ((const track_t *)b)->fname);
}

esp_err_t whm_mp3_scan(void)
{
    if (!whm_storage_mounted() && whm_storage_mount() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_tracks) {
        s_tracks = heap_caps_calloc(MAX_TRACKS, sizeof(track_t),
                                    MALLOC_CAP_SPIRAM);
        if (!s_tracks) return ESP_ERR_NO_MEM;
    }
    s_count = 0;
    int ignored = 0;
    int in_media = scan_dir(MEDIA_DIR, false, &ignored);
    if (in_media < 0) {
        ESP_LOGW(TAG, "no %s directory yet", MEDIA_DIR);
    }
    if (s_count == 0) {
        /* fall back to the card root - files often land there */
        int at_root = scan_dir(WHM_SD_MOUNT, true, &ignored);
        if (at_root > 0) {
            ESP_LOGW(TAG, "found %d .mp3 at the card ROOT - indexed; "
                          "a /media folder keeps things tidy", at_root);
        }
    }
    ESP_LOGI(TAG, "media scan: %d track(s)%s%s", s_count,
             in_media > 0 ? " in " MEDIA_DIR : "",
             ignored ? "" : "");
    if (ignored) {
        ESP_LOGI(TAG, "(%d non-mp3 file(s) ignored during scan)", ignored);
    }
    /* SORTED: readdir order is filesystem whim - alphabetical fname
       makes 'mp3 list' numbering identical across matching cards */
    qsort(s_tracks, (size_t)s_count, sizeof(track_t), track_cmp);
    return ESP_OK;
}

/* ------------------------------------------------------------ album art */

typedef struct { const uint8_t *p; uint32_t len, pos; } jsrc_t;

static UINT jpg_in(JDEC *jd, BYTE *buf, UINT n)
{
    jsrc_t *s = (jsrc_t *)jd->device;
    if (n > s->len - s->pos) n = s->len - s->pos;
    if (buf) memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    return n;
}

typedef struct { uint8_t *canvas; uint16_t w, h; } jdst_t;
static jdst_t s_jdst;

static UINT jpg_out(JDEC *jd, void *bitmap, JRECT *r)
{
    const uint8_t *src = (const uint8_t *)bitmap;
    uint16_t rw = r->right - r->left + 1;
    for (uint16_t y = r->top; y <= r->bottom; y++) {
        memcpy(s_jdst.canvas + ((uint32_t)y * s_jdst.w + r->left) * 3,
               src, (size_t)rw * 3);
        src += (size_t)rw * 3;
    }
    return 1;
}

static void art_placeholder(void)
{
    for (int y = 0; y < ART_W; y++) {
        uint8_t b = (uint8_t)(18 + y);
        for (int x = 0; x < ART_W; x++) {
            uint8_t *px = s_art + (y * ART_W + x) * 3;
            px[0] = 8; px[1] = 10; px[2] = b;
        }
    }
    /* eighth note */
    for (int dy = -5; dy <= 5; dy++) {
        for (int dx = -5; dx <= 5; dx++) {
            if (dx * dx + dy * dy <= 25) {
                int x = 24 + dx, y = 44 + dy;
                uint8_t *px = s_art + (y * ART_W + x) * 3;
                px[0] = 235; px[1] = 235; px[2] = 245;
            }
        }
    }
    for (int y = 16; y <= 44; y++) {
        uint8_t *px = s_art + (y * ART_W + 29) * 3;
        px[0] = 235; px[1] = 235; px[2] = 245;
        px[3] = 235; px[4] = 235; px[5] = 245;
    }
    for (int i = 0; i < 9; i++) {
        int x = 31 + i, y = 17 + i / 2;
        uint8_t *px = s_art + (y * ART_W + x) * 3;
        px[0] = 235; px[1] = 235; px[2] = 245;
    }
    s_art_valid = true;
}

static void art_load(const track_t *t, const char *path)
{
    if (!s_art) {
        s_art = heap_caps_malloc(ART_W * ART_W * 3, MALLOC_CAP_SPIRAM);
        if (!s_art) return;
    }
    s_art_valid = false;
    if (!t->apic_off || !t->apic_jpeg || t->apic_len > ART_MAX) {
        if (t->apic_off && !t->apic_jpeg) {
            ESP_LOGW(TAG, "APIC is not JPEG (PNG art unsupported v1) - "
                          "placeholder used");
        }
        art_placeholder();
        return;
    }
    uint8_t *raw = heap_caps_malloc(t->apic_len, MALLOC_CAP_SPIRAM);
    if (!raw) { art_placeholder(); return; }
    FILE *f = fopen(path, "rb");
    bool ok = f && fseek(f, t->apic_off, SEEK_SET) == 0 &&
              fread(raw, 1, t->apic_len, f) == t->apic_len;
    if (f) fclose(f);
    if (!ok) { free(raw); art_placeholder(); return; }

    static uint8_t work[3800];
    JDEC jd;
    jsrc_t src = { .p = raw, .len = t->apic_len, .pos = 0 };
    JRESULT jr = jd_prepare(&jd, jpg_in, work, sizeof(work), &src);
    if (jr != JDR_OK) {
        ESP_LOGW(TAG, "jpeg prepare failed (%d) - placeholder", (int)jr);
        free(raw);
        art_placeholder();
        return;
    }
    /* Prefer a decode >=128px so the box filter averages a real
       neighborhood (2x2+ per output pixel); fall back to >=64. */
    uint8_t scale = 0;
    for (uint8_t s = 3;; s--) {
        if ((jd.width >> s) >= 2 * ART_W && (jd.height >> s) >= 2 * ART_W) {
            scale = s;
            break;
        }
        if (s == 0) break;
    }
    if ((jd.width >> scale) < 2 * ART_W) {
        for (uint8_t s = 3;; s--) {
            if ((jd.width >> s) >= ART_W && (jd.height >> s) >= ART_W) {
                scale = s;
                break;
            }
            if (s == 0) break;
        }
    }
    uint16_t w2 = jd.width >> scale, h2 = jd.height >> scale;
    uint8_t *canvas = heap_caps_malloc((size_t)w2 * h2 * 3, MALLOC_CAP_SPIRAM);
    if (!canvas) { free(raw); art_placeholder(); return; }
    s_jdst.canvas = canvas; s_jdst.w = w2; s_jdst.h = h2;
    jr = jd_decomp(&jd, jpg_out, scale);
    if (jr == JDR_OK) {
        /* Panel emits LINEAR light; JPEG pixels are sRGB. Without this
           LUT, sRGB mid-gray (21.5%% intended luminance) emits ~50%% -
           the classic washed-out pastel look on LED matrices. Gamma 2.2,
           plus a mild saturation lift for the tiny-LED white point, and
           box-average downsampling instead of nearest (which threw away
           3 of every 4 source pixels). */
        static uint8_t lut[256];
        static bool lut_ok = false;
        if (!lut_ok) {
            for (int i = 0; i < 256; i++) {
                lut[i] = (uint8_t)(powf((float)i / 255.0f, 2.2f) * 255.0f
                                   + 0.5f);
            }
            lut_ok = true;
        }
        for (int y = 0; y < ART_W; y++) {
            uint32_t sy0 = (uint32_t)y * h2 / ART_W;
            uint32_t sy1 = (uint32_t)(y + 1) * h2 / ART_W;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            for (int x = 0; x < ART_W; x++) {
                uint32_t sx0 = (uint32_t)x * w2 / ART_W;
                uint32_t sx1 = (uint32_t)(x + 1) * w2 / ART_W;
                if (sx1 <= sx0) sx1 = sx0 + 1;
                uint32_t ar = 0, ag = 0, ab = 0, n = 0;
                for (uint32_t yy = sy0; yy < sy1; yy++) {
                    const uint8_t *row = canvas + (yy * w2 + sx0) * 3;
                    for (uint32_t xx = sx0; xx < sx1; xx++) {
                        ar += row[0]; ag += row[1]; ab += row[2];
                        row += 3;
                        n++;
                    }
                }
                int r = (int)(ar / n), g = (int)(ag / n), b = (int)(ab / n);
                /* +15%% saturation in sRGB, then gamma to linear */
                int l = (77 * r + 150 * g + 29 * b) >> 8;
                r = l + (((r - l) * 147) >> 7);
                g = l + (((g - l) * 147) >> 7);
                b = l + (((b - l) * 147) >> 7);
                if (r < 0) r = 0;
                if (r > 255) r = 255;
                if (g < 0) g = 0;
                if (g > 255) g = 255;
                if (b < 0) b = 0;
                if (b > 255) b = 255;
                uint8_t *px = s_art + (y * ART_W + x) * 3;
                px[0] = lut[r]; px[1] = lut[g]; px[2] = lut[b];
            }
        }
        s_art_valid = true;
        ESP_LOGI(TAG, "album art: %ux%u jpeg -> %ux%u -> 64x64 "
                      "(box+gamma)", jd.width, jd.height, w2, h2);
    } else {
        ESP_LOGW(TAG, "jpeg decode failed (%d) - placeholder", (int)jr);
        art_placeholder();
    }
    free(canvas);
    free(raw);
}

/* -------------------------------------------------------------- playback */

static void track_close(void)
{
    if (s_f) { fclose(s_f); s_f = NULL; }
    whm_audio_spk_close();
}

static bool refill(void)
{
    if (s_in_eof || s_in_len >= INBUF_SZ) return s_in_len > 0;
    size_t n = fread(s_in + s_in_len, 1, INBUF_SZ - s_in_len, s_f);
    if (n == 0) s_in_eof = true;
    s_in_len += (int)n;
    return s_in_len > 0;
}

static bool track_open(int idx)
{
    track_close();
    if (idx < 0 || idx >= s_count) return false;
    track_t *t = &s_tracks[idx];
    char path[160];
    snprintf(path, sizeof(path), "%s/%s",
             t->at_root ? WHM_SD_MOUNT : MEDIA_DIR, t->fname);

    art_load(t, path);

    s_f = fopen(path, "rb");
    if (!s_f) return false;
    fseek(s_f, t->audio_off, SEEK_SET);
    mp3dec_init(&s_dec);
    s_in_len = 0; s_in_eof = false;
    s_pos_bytes = 0;
    s_audio_bytes = t->fsize > t->audio_off ? t->fsize - t->audio_off : 1;
    s_kbps_sum = 0; s_kbps_n = 0;

    /* prime: find the first real frame to learn the sample format */
    for (int guard = 0; guard < 64; guard++) {
        if (!refill()) return false;
        mp3dec_frame_info_t fi;
        int samples = mp3dec_decode_frame(&s_dec, s_in, s_in_len, s_pcm, &fi);
        if (fi.frame_bytes <= 0) {
            if (s_in_eof) return false;
            continue;
        }
        memmove(s_in, s_in + fi.frame_bytes, s_in_len - fi.frame_bytes);
        s_in_len -= fi.frame_bytes;
        s_pos_bytes += fi.frame_bytes;
        if (samples > 0) {
            s_hz = fi.hz; s_ch = fi.channels;
            s_kbps_sum += fi.bitrate_kbps; s_kbps_n++;
            if (whm_audio_spk_open((uint32_t)s_hz, 2, s_vol) != ESP_OK) {
                ESP_LOGE(TAG, "speaker busy/unavailable");
                return false;
            }
            const int16_t *out = s_pcm;
            int frames = samples;
            if (s_ch == 1) {
                for (int i = 0; i < samples; i++) {
                    s_pcm2[2 * i] = s_pcm2[2 * i + 1] = s_pcm[i];
                }
                out = s_pcm2;
            }
            dj_tee(out, frames);
            eng_write(out, frames);
            s_cur = idx;
            ESP_LOGI(TAG, "playing [%d] %s - %s (%dHz %dch)", idx,
                     t->title, t->artist[0] ? t->artist : "?", s_hz, s_ch);
            ESP_LOGD(TAG, "mp3 task stack headroom: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            return true;
        }
    }
    return false;
}

/* returns false at end of track */
static bool pump(void)
{
    if (!refill() && s_in_len == 0) return false;
    mp3dec_frame_info_t fi;
    int samples = mp3dec_decode_frame(&s_dec, s_in, s_in_len, s_pcm, &fi);
    if (fi.frame_bytes <= 0) return !s_in_eof;
    memmove(s_in, s_in + fi.frame_bytes, s_in_len - fi.frame_bytes);
    s_in_len -= fi.frame_bytes;
    s_pos_bytes += fi.frame_bytes;
    if (samples > 0) {
        s_kbps_sum += fi.bitrate_kbps; s_kbps_n++;
        const int16_t *out = s_pcm;
        if (fi.channels == 1) {
            for (int i = 0; i < samples; i++) {
                s_pcm2[2 * i] = s_pcm2[2 * i + 1] = s_pcm[i];
            }
            out = s_pcm2;
        }
        dj_tee(out, samples);
        eng_write(out, samples);
    }
    return true;
}

static void player_task(void *arg)
{
    (void)arg;
    cmd_t c;
    for (;;) {
        if (s_state == WHM_MP3_STOPPED) {
            xQueueReceive(s_q, &c, portMAX_DELAY);
        } else if (xQueueReceive(s_q, &c, 0) != pdTRUE) {
            if (s_state == WHM_MP3_PAUSED) {
                vTaskDelay(pdMS_TO_TICKS(40));
                continue;
            }
            if (!pump()) {                     /* auto-advance */
                int nxt = (s_cur + 1) % (s_count ? s_count : 1);
                if (s_count == 0 || !track_open(nxt)) {
                    track_close();
                    s_state = WHM_MP3_STOPPED;
                }
            }
            continue;
        }
        switch (c.op) {
        case OP_FLEET: {
            if (s_state != WHM_MP3_STOPPED) {
                whm_audio_spk_close();
                s_state = WHM_MP3_STOPPED;
            }
            s_fs_waited = false;
            s_fs_content = 0;
            /* s_fs_start already set by whm_mp3_fleet_play */
            if (track_open(c.arg)) {
                s_state = WHM_MP3_PLAYING;
                ESP_LOGI(TAG, "MODE A: locked to start_tsf");
            } else {
                s_fs_start = 0;
            }
            break;
        }
        case OP_PLAY: {
            s_fs_start = 0;                  /* local play: engine off */
            int idx = c.arg >= 0 ? c.arg : (s_cur >= 0 ? s_cur : 0);
            if (track_open(idx)) s_state = WHM_MP3_PLAYING;
            else { track_close(); s_state = WHM_MP3_STOPPED; }
            break;
        }
        case OP_STOP:
            s_fs_start = 0;
            track_close();
            s_state = WHM_MP3_STOPPED;
            break;
        case OP_PAUSE:
            if (s_state == WHM_MP3_PLAYING) s_state = WHM_MP3_PAUSED;
            break;
        case OP_RESUME:
            if (s_state == WHM_MP3_PAUSED) s_state = WHM_MP3_PLAYING;
            break;
        case OP_NEXT:
        case OP_PREV: {
            if (!s_count) break;
            int idx = (s_cur + (c.op == OP_NEXT ? 1 : s_count - 1)) % s_count;
            if (track_open(idx)) s_state = WHM_MP3_PLAYING;
            else s_state = WHM_MP3_STOPPED;
            break;
        }
        case OP_VOL:
            s_vol = (uint8_t)c.arg;
            whm_settings_set_u32("mp3_vol", s_vol);
            whm_audio_spk_setvol(s_vol);
            break;
        }
    }
}

/* ---------------------------------------------------------------- public */

static void post(op_t op, int arg)
{
    cmd_t c = { .op = op, .arg = arg };
    if (s_q) xQueueSend(s_q, &c, pdMS_TO_TICKS(100));
}

static int track_by_fname(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcasecmp(s_tracks[i].fname, name) == 0) return i;
    }
    return -1;
}

esp_err_t whm_mp3_track_fname(int idx, char *out, size_t len)
{
    if (s_count == 0) whm_mp3_scan();
    if (idx < 0) idx = s_cur >= 0 ? s_cur : 0;
    if (idx >= s_count) return ESP_ERR_NOT_FOUND;
    strlcpy(out, s_tracks[idx].fname, len);
    return ESP_OK;
}

esp_err_t whm_mp3_fleet_play_m(const char *name, const char *sha,
                               int64_t start_tsf, bool stop,
                               bool is_master, const char *master)
{
    s_fs_master = is_master;
    s_ma_ref_tsf = 0;
    s_ma_ref_idx = 0;
    s_ma_last_tx = 0;
    if (!is_master && master) {
        strlcpy(s_fs_master_name, master, sizeof(s_fs_master_name));
    } else {
        s_fs_master_name[0] = 0;
    }
    if (s_due_fn == ma_beacon_due) s_due_fn = NULL;
    return whm_mp3_fleet_play(name, sha, start_tsf, stop);
}

esp_err_t whm_mp3_fleet_play(const char *name, const char *sha,
                             int64_t start_tsf, bool stop)
{
    if (stop) {
        s_fs_start = 0;
        post(OP_STOP, 0);
        return ESP_OK;
    }
    if (s_count == 0) whm_mp3_scan();
    int idx = track_by_fname(name);
    if (idx < 0) {
        printf("[modeA] '%s' not on this card - run: sync media "
               "<leader>\n", name);
        return ESP_ERR_NOT_FOUND;
    }
    char lsha[65];
    long lsz;
    if (sha && sha[0]) {
        if (whm_http_media_lookup(name, lsha, &lsz) != ESP_OK) {
            printf("[modeA] no local hash for '%s' yet - playing by "
                   "name ('sync media' verifies)\n", name);
        } else if (strcasecmp(lsha, sha) != 0) {
            printf("[modeA] '%s' differs from leader's copy - run: "
                   "sync media <leader>\n", name);
            return ESP_ERR_INVALID_STATE;
        }
    }
    {
        int64_t tn = whm_wifi_tsf_now();
        printf("[modeA] '%s' downbeat in %dms\n", name,
               (int)((start_tsf - tn) / 1000));
    }
    s_ma_ref_tsf = 0;            /* ARM RESET: a stale ref from the
                                    previous run poisoned the due
                                    formula until the first new
                                    beacon - the 65s/120s hard-
                                    resync pairs in the field log,
                                    one per replay. Clean slate. */
    s_ma_ref_idx = 0;
    s_due_fn = NULL;
    s_ma_bcn_n = 0;
    s_fs_start = start_tsf;
    strlcpy(s_fs_name, name, sizeof(s_fs_name));
    post(OP_FLEET, idx);
    return ESP_OK;
}

int64_t whm_mp3_sync_err_us(void)
{
    return (s_fs_start > 0 && s_state == WHM_MP3_PLAYING) ? s_fs_err_us
                                                          : INT64_MIN;
}

esp_err_t whm_mp3_init(void)
{
    /* RAM audit 0.24: ~29.5KB of pure data buffers move to PSRAM
       (decoder STATE stays internal - hot per-sample access) */
    if (!s_in) {
        s_in = heap_caps_malloc(INBUF_SZ, MALLOC_CAP_SPIRAM);
        if (!s_in) s_in = malloc(INBUF_SZ);
        if (!s_in) return ESP_ERR_NO_MEM;
    }
    if (!s_pcm) {
        size_t sz = MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t);
        s_pcm = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!s_pcm) s_pcm = malloc(sz);
        if (!s_pcm) return ESP_ERR_NO_MEM;
    }
    if (!s_pcm2) {
        size_t sz = MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * sizeof(int16_t);
        s_pcm2 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (!s_pcm2) s_pcm2 = malloc(sz);
        if (!s_pcm2) return ESP_ERR_NO_MEM;
    }
    uint32_t v = 70;
    whm_settings_get_u32("mp3_vol", &v);
    s_vol = (uint8_t)(v > 100 ? 100 : v);
    s_q = xQueueCreate(8, sizeof(cmd_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    /* minimp3's decode locals + the codec-open call chain need real
       headroom: 12K overflowed on the first frame. 20K is comfortable -
       and it now lives in PSRAM: this stack was the single biggest
       internal-RAM consumer and helped starve the REPL at boot. TCB
       stays internal (required); SD/I2S/decode are all PSRAM-stack
       safe. */
    static StaticTask_t s_mp3_tcb;
    StackType_t *stk = heap_caps_malloc(20480,
                                        MALLOC_CAP_SPIRAM |
                                        MALLOC_CAP_8BIT);
    if (stk) {
        xTaskCreateStaticPinnedToCore(player_task, "whm_mp3",
                                      20480 / sizeof(StackType_t), NULL,
                                      6, stk, &s_mp3_tcb, 0);
    } else {
        ESP_LOGW(TAG, "PSRAM stack alloc failed; internal fallback");
        xTaskCreatePinnedToCore(player_task, "whm_mp3", 20480, NULL, 6,
                                NULL, 0);
    }
    return ESP_OK;
}

int whm_mp3_count(void) { return s_count; }

void whm_mp3_invalidate(void)      /* card reformatted/replaced */
{
    whm_http_media_dirty();
    s_count = 0;
    s_cur = -1;
}
whm_mp3_state_t whm_mp3_state(int *idx)
{
    if (idx) *idx = s_cur;
    return s_state;
}

esp_err_t whm_mp3_track_info(int idx, char *title, size_t tl,
                             char *artist, size_t al)
{
    if (idx < 0 || idx >= s_count) return ESP_ERR_INVALID_ARG;
    if (title) strlcpy(title, s_tracks[idx].title, tl);
    if (artist) strlcpy(artist, s_tracks[idx].artist, al);
    return ESP_OK;
}

void whm_mp3_play(int idx) { post(OP_PLAY, idx); }
void whm_mp3_stop(void) { post(OP_STOP, 0); }
void whm_mp3_next(void) { post(OP_NEXT, 0); }
void whm_mp3_prev(void) { post(OP_PREV, 0); }
void whm_mp3_toggle(void)
{
    if (s_state == WHM_MP3_PLAYING) post(OP_PAUSE, 0);
    else if (s_state == WHM_MP3_PAUSED) post(OP_RESUME, 0);
    else if (s_count > 0) post(OP_PLAY, -1);
}
void whm_mp3_set_vol(uint8_t pct) { post(OP_VOL, pct > 100 ? 100 : pct); }
uint8_t whm_mp3_vol(void) { return s_vol; }

const uint8_t *whm_mp3_art(void) { return s_art_valid ? s_art : NULL; }

void whm_mp3_progress(uint32_t *pos_s, uint32_t *dur_s)
{
    uint32_t kbps = s_kbps_n ? s_kbps_sum / s_kbps_n : 128;
    if (kbps == 0) kbps = 128;
    if (pos_s) *pos_s = s_pos_bytes / (kbps * 125);
    if (dur_s) *dur_s = s_audio_bytes / (kbps * 125);
}
