#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WHM_MP3_STOPPED, WHM_MP3_PLAYING, WHM_MP3_PAUSED } whm_mp3_state_t;

esp_err_t whm_mp3_init(void);
esp_err_t whm_mp3_fleet_play_m(const char *name, const char *sha,
                               int64_t start_tsf, bool stop,
                               bool is_master, const char *master);
bool      whm_mp3_is_master(void);
void      whm_mp3_a_on_beacon(const char *from, int64_t tsf,
                              int64_t idx);
esp_err_t whm_mp3_fleet_play(const char *name, const char *sha,
                             int64_t start_tsf, bool stop);
esp_err_t whm_mp3_track_fname(int idx, char *out, size_t len);
int64_t   whm_mp3_sync_err_us(void);    /* INT64_MIN when not syncing */          /* start the player task */
esp_err_t whm_mp3_dj_start(int n);      /* Mode B: play + stream */
void      whm_mp3_dj_stop(void);
void      whm_mp3_now_title(char *out, size_t n);
void      whm_mp3_diag(bool on);
bool      whm_mp3_dj_active(void);
void      whm_mp3_b_on_pkt(const char *from, uint32_t rate, uint8_t ch,
                           bool playing, const char *title);
float     whm_mp3_sync_ppm(void);      /* servo rate, +-ppm */
esp_err_t whm_mp3_scan(void);          /* (re)scan /sdcard/media */
int       whm_mp3_count(void);
void      whm_mp3_invalidate(void);
esp_err_t whm_mp3_track_info(int idx, char *title, size_t tl,
                             char *artist, size_t al);
void      whm_mp3_play(int idx);       /* -1 = current/first */
void      whm_mp3_stop(void);
void      whm_mp3_toggle(void);        /* play/pause */
void      whm_mp3_next(void);
void      whm_mp3_prev(void);
void      whm_mp3_set_vol(uint8_t pct);
uint8_t   whm_mp3_vol(void);
whm_mp3_state_t whm_mp3_state(int *idx);
const uint8_t *whm_mp3_art(void);      /* 64*64*3 RGB888 or NULL */
void      whm_mp3_progress(uint32_t *pos_s, uint32_t *dur_s);

#ifdef __cplusplus
}
#endif
