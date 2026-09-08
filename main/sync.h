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
esp_err_t whm_sync_wkb_send(uint8_t owner, float x, int8_t y,
                            uint8_t st, int8_t dir, uint16_t timer,
                            uint32_t step, float tgt, float vx);
esp_err_t whm_sync_mab_send(int64_t tsf, int64_t idx);
esp_err_t whm_sync_nye_send(const char *tz, uint16_t year,
                            int64_t start_tsf);
esp_err_t whm_sync_djb_send(bool playing, uint32_t rate, uint8_t ch,
                            const char *title);
const char *whm_sync_node_name(void);
void whm_sync_brief(int *role, int *fresh, int *stale,
                    uint32_t *anchor_age_ms);
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
