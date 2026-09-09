/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
void        whm_id_init(void);
const uint8_t *whm_id_pk(void);                 /* ed 32B */
const uint8_t *whm_id_xpk(void);                /* x25519 32B */
void        whm_id_xshared(const uint8_t their_xpk[32],
                           uint8_t out[32]);
bool        whm_pinx_get(const char *name, uint8_t xpk[32]);
void        whm_pinx_put(const char *name, const uint8_t xpk[32]);                 /* 32B */
void        whm_id_sign(const uint8_t *m, size_t n, uint8_t sig[64]);
bool        whm_id_verify(const uint8_t pk[32], const uint8_t *m,
                          size_t n, const uint8_t sig[64]);
void        whm_id_fp(const uint8_t pk[32], char out[17]);
bool        whm_pin_get(const char *name, uint8_t pk[32]);
void        whm_pin_put(const char *name, const uint8_t pk[32]);
bool        whm_pin_del(const char *name);
int         whm_pin_count(void);
int         whm_pin_list(void (*cb)(const char *, const uint8_t *));
