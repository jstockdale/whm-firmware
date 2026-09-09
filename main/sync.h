/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WHM_SYNC_OFF = 0,
    WHM_SYNC_CONDUCTOR = 1,
    WHM_SYNC_MEMBER = 2,
} whm_sync_role_t;

esp_err_t whm_sync_init(void);                 /* resume role from NVS */
esp_err_t whm_sync_conduct(void);
esp_err_t whm_sync_join(const char *conductor_name);
esp_err_t whm_sync_auto(void);                /* join elections */
esp_err_t whm_sync_anchor(void);              /* claim now, prio>=200 */
esp_err_t whm_sync_set_prio(uint8_t prio);    /* 0 = follower-only */
const char *whm_sync_anchor_name(void);
uint8_t   whm_sync_prio(void);
int       whm_sync_mode(void);
esp_err_t whm_sync_follow(const char *name);  /* member, wifi untouched */
esp_err_t whm_sync_off(void);
bool      whm_sync_wall_from_flock(void);
const char *whm_sync_peer(void);
void whm_sync_name_refresh(void);
int64_t   whm_sync_conductor_age_us(void);    /* -1 when n/a */
whm_sync_role_t whm_sync_role(void);
void whm_sync_status_print(void);
void whm_sync_discover_print(void);
esp_err_t whm_sync_fleet_send(const char *line);
esp_err_t whm_sync_play_send(const char *name, const char *sha,
                             int64_t start_tsf, bool stop);
esp_err_t whm_sync_walk_input_send(uint8_t act, uint32_t exec_step,
                                   float arg);
esp_err_t whm_sync_wkparams_send(int64_t anchor, float cam_speed,
                                 uint8_t strips);
bool whm_sync_is_conductor(void);
esp_err_t whm_sync_oracle_send(uint8_t a8, uint32_t step,
                               uint64_t h, uint16_t hs);
esp_err_t whm_sync_wkb_send(uint8_t owner, float x, int8_t y,
                            uint8_t st, int8_t dir, uint16_t timer,
                            uint32_t step, float tgt, float vx,
                            float vy, float spd, int8_t sdir,
                            uint8_t phase, uint8_t turn_cd,
                            uint8_t fresh);
esp_err_t whm_sync_mab_send(int64_t tsf, int64_t idx);
esp_err_t whm_sync_nye_send(const char *tz, uint16_t year,
                            int64_t start_tsf);
esp_err_t whm_sync_djb_send(bool playing, uint32_t rate, uint8_t ch,
                            const char *title);
const char *whm_sync_node_name(void);
const char *whm_sync_peer_fw(const char *name);
void whm_sync_status_brief(char *mode16, char *role16,
                           uint8_t *prio, char *anchor16);
esp_err_t whm_sync_frame_send(uint16_t seq, uint16_t w, uint16_t h,
                              uint32_t pts, const uint8_t *rle,
                              uint32_t len);
bool whm_sync_secure_status(bool *strict);
void whm_sync_secure_strict(bool on);
int whm_sync_peer_iter(int i, char n16[16], char f8[8],
                       uint32_t *age_ms, uint8_t *role);
void whm_sync_brief(int *role, int *fresh, int *stale,
                    uint32_t *anchor_age_ms);
void whm_sync_set_tap(void (*cb)(const uint8_t *, int));
void whm_sync_inject(const uint8_t *buf, int len, bool rebroadcast);
esp_err_t whm_sync_fleet_send_to(const char *target, const char *line);
uint32_t  whm_sync_lead_ms(void);
void      whm_sync_set_lead_ms(uint32_t ms);
void      whm_sync_defer_line(const char *line, int64_t at);
bool      whm_sync_pop_due(char *out, size_t olen);
void whm_sync_life_send(uint8_t idx, uint8_t side, uint32_t gen,
                        const uint8_t *col64);
bool whm_sync_life_edge(uint8_t peer_idx, uint8_t side, uint8_t *out64,
                        uint32_t *gen);

#ifdef __cplusplus
}
#endif

void whm_lts(void);   /* UTC wall-clock stamp [HH:MM:SS.mmm] */
