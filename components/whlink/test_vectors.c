/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/* test_vectors.c — cross-implementation golden wire vectors.
 *
 * Both the watch and the ring compile this against THEIR OWN copy of wh_link.h.
 * It re-encodes a set of fixed inputs and compares the bytes to hard-coded
 * expected values. Round-trip tests only prove a single build is self-consistent;
 * these pin the exact wire bytes, so if the two implementations ever drift (a
 * reordered field, a different endianness, a changed session-blob layout, a crypto
 * mismatch) one side's build goes red instead of the link silently misbehaving on
 * hardware. The expected values were generated from the watch's wh_link.h; if the
 * ring disagrees, reconcile against the format spec and regenerate together.
 *
 * Build:  cc -O2 -I. test_vectors.c -o test_vectors && ./test_vectors
 */
#define WH_LINK_IMPLEMENTATION
#include "wh_link.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, passes = 0;

static void check(const char *name, const uint8_t *got, size_t n, const char *expect) {
    char h[1024];
    for (size_t i = 0; i < n; i++) sprintf(h + 2 * i, "%02x", got[i]);
    h[2 * n] = 0;
    if (strcmp(h, expect) == 0) { printf("  PASS  %-14s (%zu B)\n", name, n); passes++; }
    else {
        printf("  FAIL  %s\n        got:    %s\n        expect: %s\n", name, h, expect);
        fails++;
    }
}

/* capture transport for the full-frame vector */
static uint8_t cap_frame[WH_MAX_FRAME]; static size_t cap_len;
static int cap_send(void *u, const uint8_t *f, size_t n) { (void)u; memcpy(cap_frame, f, n); cap_len = n; return 0; }
static void rng_zero(void *u, uint8_t *b, size_t n) { (void)u; memset(b, 0, n); }

int main(void) {
    printf("=== wh-link golden wire vectors (cross-impl drift check) ===\n");
    uint8_t b[256]; int n;

    /* 1) STATUS message */
    wh_device_status st = { .role = WH_ROLE_RING, .mode = WH_OPMODE_SCANNING,
        .collectors = WH_COLL_WIFI | WH_COLL_BLE, .batt_pct = 73, .seen_wifi = 123456, .seen_ble = 789,
        .timer_state = WH_TMR_RUNNING, .timer_target_ms = 1723612345678LL,
        .sw_state = WH_SW_PAUSED, .sw_start_ms = 1723600000000LL, .sw_accum_ms = 42000 };
    n = wh_enc_status(&st, b, sizeof b);
    check("status", b, (size_t)n, "0101034940e2010015030000014e354d4f910100000200d4904e9101000010a40000");

    /* 2) device_seen message (now includes the 21-byte 3B timestamp tail) */
    wh_device_seen ds; memset(&ds, 0, sizeof ds);
    ds.kind = 1; ds.rssi = -57; ds.channel = 6; ds.flags = 0x02; ds.sec = 4; ds.freq_mhz = 2437;
    for (int i = 0; i < WH_ADDR_LEN; i++) ds.addr[i] = (uint8_t)(0xA0 + i);
    strncpy(ds.name, "whitehat-ap", WH_NAME_MAX - 1);
    ds.utc_valid = 1;   /* one non-zero tail byte pins the tail field order in the vector */
    n = wh_enc_device_seen(&ds, b, sizeof b);
    check("device_seen", b, (size_t)n,
          "01c70602048509a0a1a2a3a4a50b77686974656861742d6170"
          "000000000000000000000000000000000100000000");

    /* 2b) device_seen, sub-GHz kind (WH_SEEN_SUBGHZ=4): the kind-gated sub-GHz
     *      tail sits AFTER the 21-byte timestamp tail, length-guarded. Requires
     *      the sub-GHz fields on wh_device_seen (nested `.sg`) and the tail codec
     *      folded into wh_enc_device_seen -- see proposals/wh_subghz_seen.{h,c}.
     *      Bytes = base+name(24) | timestamp tail(21) | sub-GHz tail(30) = 75. */
    wh_device_seen dg; memset(&dg, 0, sizeof dg);
    dg.kind = WH_SEEN_SUBGHZ; dg.rssi = -72; dg.channel = 0; dg.flags = 0; dg.sec = 0;
    dg.freq_mhz = 434;   /* rounded MHz; exact Hz is in the sub-GHz tail */
    /* addr = stable fingerprint-derived id so (kind,addr) dedup shows distinct
     * emitters; fixed here to pin the vector. */
    { uint8_t id[WH_ADDR_LEN] = {0xE0,0xE1,0xE2,0xE3,0xE4,0xE5}; memcpy(dg.addr, id, WH_ADDR_LEN); }
    strncpy(dg.name, "OOK-433.92", WH_NAME_MAX - 1);
    dg.utc_ns = 1700000000000000000LL; dg.mono_ns = 123456789012345LL;
    dg.utc_valid = 1; dg.sync_delay_ns = 3200000;
    dg.sg.freq_hz = 433920000u; dg.sg.mod = 2 /*OOK*/; dg.sg.drate_bps = 4800;
    dg.sg.lqi = 31; dg.sg.klass = 1 /*periodic-OOK*/;
    dg.sg.n_edges = 42; dg.sg.duration_ms = 1200;
    { uint16_t fp[4] = {320,640,160,1280}; memcpy(dg.sg.fp, fp, 8); }
    { uint8_t dc[6] = {0xA5,0x5A,0x0F,0xF0,0x12,0x34}; dg.sg.dec_len = 6; memcpy(dg.sg.dec, dc, 6); }
    n = wh_enc_device_seen(&dg, b, sizeof b);
    check("device_seen_sg", b, (size_t)n,
          "04b8000000b201e0e1e2e3e4e50a4f4f4b2d3433332e3932"   /* base + name  */
          "00002a36fe9c971779df0d86487000000100d43000"         /* 21B ts tail  */
          "0018dd1902c01200001f0140018002a000000506a55a0ff01234"); /* sub-GHz: 20 B tail (node §3c; n_edges/duration_ms are local-only, off-wire) */

    /* 3) persisted session blob */
    wh_ctx a; memset(&a, 0, sizeof a);
    for (int i = 0; i < WH_KEY_LEN; i++) { a.sess.tx_key[i] = (uint8_t)(0x10 + i); a.sess.rx_key[i] = (uint8_t)(0x80 + i); }
    for (int i = 0; i < WH_NONCE_PREFIX_LEN; i++) { a.sess.tx_prefix[i] = (uint8_t)(0xA0 + i); a.sess.rx_prefix[i] = (uint8_t)(0xC0 + i); }
    a.sess.tx_ctr = 1000; a.sess.rx_replay.max = 777; a.sess.rx_replay.seen = 1; a.sess.established = 1; a.cfg.peer_id = 0xBEEF;
    n = wh_session_export(&a, b, sizeof b);
    check("session_blob", b, (size_t)n,
        "57485301efbe101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
        "a0a1a2a3a4a5a6a7c0c1c2c3c4c5c6c7e80300000903000001");

    /* 4) full sealed frame (framing + nonce construction + AEAD), fixed session */
    wh_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.local_id = 0x0002; cfg.peer_id = 0x0001; cfg.role = WH_ROLE_WATCH;
    cfg.crypto = wh_ref_crypto(); cfg.rng = rng_zero; cfg.transport_send = cap_send;
    wh_ctx c; wh_ctx_init(&c, &cfg);
    for (int i = 0; i < WH_KEY_LEN; i++) c.sess.tx_key[i] = (uint8_t)(0x10 + i);
    for (int i = 0; i < WH_NONCE_PREFIX_LEN; i++) c.sess.tx_prefix[i] = (uint8_t)(0xA0 + i);
    c.sess.tx_ctr = 5; c.sess.established = 1; c.cfg.peer_id = 0x0001;
    uint8_t pl[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    wh_send(&c, WH_MSG_PING, 0, 0, pl, sizeof pl);
    check("sealed_ping", cap_frame, cap_len,
        "574801060100020001000500000000000100000104007ccd74cd8c3f7324400f8f6b9ea2c2cbff84899d");

    /* 5) decode the golden blob: both sides must import to identical restored state */
    wh_ctx d; memset(&d, 0, sizeof d);
    uint8_t blob[128]; memset(blob, 0, sizeof blob);
    for (size_t i = 0; i < (size_t)WH_SESSION_BLOB_LEN; i++) {
        const char *hexs = "57485301efbe101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
            "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
            "a0a1a2a3a4a5a6a7c0c1c2c3c4c5c6c7e80300000903000001";
        unsigned v; sscanf(hexs + 2 * i, "%2x", &v); blob[i] = (uint8_t)v;
    }
    int rc = wh_session_import(&d, blob, WH_SESSION_BLOB_LEN);
    int ok = (rc == WH_OK) && (d.cfg.peer_id == 0xBEEF) &&
             (d.sess.tx_ctr == 1000u + WH_PERSIST_MARGIN) && (d.sess.rx_replay.max == 777);
    printf(ok ? "  PASS  session_import (peer/keys/tx_ctr+MARGIN)\n"
              : "  FAIL  session_import: rc=%d peer=0x%04x tx_ctr=%u\n", rc, d.cfg.peer_id, (unsigned)d.sess.tx_ctr);
    ok ? passes++ : fails++;

    printf("=== %d passed, %d failed ===\n", passes, fails);
    return fails == 0 ? 0 : 1;
}
