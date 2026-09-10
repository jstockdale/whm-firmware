#pragma once
void mon_http_start(void);
void mon_snap_service(const uint16_t *fb);  /* ui task, post-compose */
int  mon_snap_take(void);                   /* console path: fill snap */
const uint16_t *mon_snap_buf(void);
