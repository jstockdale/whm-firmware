#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_check.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WHM_TS_DISABLED,
    WHM_TS_WAITING,   /* enabled, no sync yet this boot */
    WHM_TS_SYNCED,    /* synced within 2x interval */
    WHM_TS_STALE,     /* last sync older than 2x interval */
} whm_timesync_state_t;

/* boot order: apply_tz() before anything uses localtime;
 * seed_from_rtc() after sensors init; net hooks from wifi events */
void      whm_timesync_apply_tz(void);
void      whm_timesync_seed_from_rtc(void);
void      whm_timesync_net_up(void);
void      whm_timesync_net_down(void);

esp_err_t whm_timesync_enable(bool on);
esp_err_t whm_timesync_set_server(const char *host);  /* NULL/"" -> default */
esp_err_t whm_timesync_set_interval(uint16_t minutes);
esp_err_t whm_timesync_force(void);
esp_err_t whm_timesync_set_tz(const char *tz);
esp_err_t whm_timesync_set_tz_named(const char *label, const char *posix);
const char *whm_timesync_tz_label(void);
esp_err_t whm_timesync_rtc_write_now(void);        /* NULL/"" -> UTC */
const char *whm_timesync_tz(void);

whm_timesync_state_t whm_timesync_state(void);
void      whm_timesync_status_print(void);

#ifdef __cplusplus
}
#endif
