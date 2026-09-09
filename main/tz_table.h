/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *alias;   /* friendly lowercase name for `tz <alias>` */
    const char *place;   /* IANA-style place, searchable */
    const char *posix;   /* the TZ string newlib consumes */
} whm_tz_entry_t;

extern const whm_tz_entry_t k_tz_table[];
extern const int k_tz_count;

#ifdef __cplusplus
}
#endif
