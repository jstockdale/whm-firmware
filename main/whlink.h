/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/* wh-link on WHM - public surface (L1: link-up + pairing) */
#pragma once
#include <stdbool.h>
#include <stdint.h>

void whm_whlink_init(void);
void whm_whlink_pair_window(uint32_t secs);   /* open discoverable window */
void whm_whlink_off(void);                    /* close window, drop link  */
void whm_whlink_status_print(void);
void whm_whlink_status_line(char *out, size_t cap);
void whm_whlink_sas_result(bool match);
void whm_whlink_forget(void);                 /* erase the bond    */
void whm_whlink_feed(bool on);                /* whmcast tunnel out */       /* from the UI BOOT button  */
