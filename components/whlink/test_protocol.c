/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/* test_protocol.c — wh-link protocol tests (single TU: includes implementation)
 *
 * Covers: header byte-exactness + validation, message round-trips, replay
 * window, nonce construction, the full pairing handshake over a simulated
 * two-endpoint bus (SAS agreement, key mirroring, session encryption), replay
 * and tamper rejection on live traffic, MITM/tamper detection on the handshake,
 * and fragmentation/reassembly of a large message.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#define WH_LINK_IMPLEMENTATION
#include "wh_link.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { g_pass++; printf("  PASS  %s\n", name); } \
    else      { g_fail++; printf("  FAIL  %s  (%s:%d)\n", name, __FILE__, __LINE__); } \
} while (0)

/* ---------------- simulated bus + endpoints ---------------- */
#define QCAP 64
typedef struct { uint8_t buf[WH_MAX_FRAME]; size_t len; } frame_t;
typedef struct { frame_t q[QCAP]; int head, tail; } queue_t;

static void q_push(queue_t *q, const uint8_t *f, size_t len) {
    int n = (q->tail + 1) % QCAP;
    if (n == q->head) { printf("QUEUE OVERFLOW\n"); exit(2); }
    memcpy(q->q[q->tail].buf, f, len);
    q->q[q->tail].len = len;
    q->tail = n;
}
static int q_pop(queue_t *q, frame_t *out) {
    if (q->head == q->tail) return 0;
    *out = q->q[q->head];
    q->head = (q->head + 1) % QCAP;
    return 1;
}

typedef struct endpoint {
    const char *name;
    queue_t *tx;            /* where our sent frames go */
    uint64_t prng;
    char last_sas[WH_SAS_DIGITS + 1];
    int  sas_count;
    int  paired;            /* -1 none, 0 fail, 1 ok */
    /* last received app message */
    uint8_t  rx_type;
    uint16_t rx_corr;
    uint8_t  rx_buf[WH_REASM_MAX];
    size_t   rx_len;
    int      rx_count;
    /* capture of last sent frame (for replay/tamper tests) */
    uint8_t  last_sent[WH_MAX_FRAME];
    size_t   last_sent_len;
} endpoint;

static uint64_t xorshift(uint64_t *s) {
    uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x;
}
static void ep_rng(void *user, uint8_t *buf, size_t len) {
    endpoint *e = (endpoint *)user;
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(xorshift(&e->prng) & 0xff);
}
static uint32_t ep_now(void *user) { (void)user; static uint32_t t = 1000; return t += 5; }
static int ep_send(void *user, const uint8_t *frame, size_t len) {
    endpoint *e = (endpoint *)user;
    memcpy(e->last_sent, frame, len);
    e->last_sent_len = len;
    q_push(e->tx, frame, len);
    return 0;
}
static void ep_on_msg(void *user, const wh_msg *m) {
    endpoint *e = (endpoint *)user;
    e->rx_type = m->type;
    e->rx_corr = m->corr;
    e->rx_len = m->len;
    if (m->len <= sizeof(e->rx_buf)) memcpy(e->rx_buf, m->payload, m->len);
    e->rx_count++;
}
static void ep_on_sas(void *user, const char *digits) {
    endpoint *e = (endpoint *)user;
    strncpy(e->last_sas, digits, sizeof(e->last_sas) - 1);
    e->last_sas[sizeof(e->last_sas) - 1] = '\0';
    e->sas_count++;
}
static void ep_on_paired(void *user, int ok) {
    endpoint *e = (endpoint *)user;
    e->paired = ok ? 1 : 0;
}

/* Deliver all queued frames until both queues drain. a2b -> ctxB, b2a -> ctxA */
static void pump(queue_t *a2b, queue_t *b2a, wh_ctx *A, wh_ctx *B) {
    frame_t f;
    int spin = 0;
    for (;;) {
        int did = 0;
        while (q_pop(a2b, &f)) { wh_on_frame(B, f.buf, f.len); did = 1; }
        while (q_pop(b2a, &f)) { wh_on_frame(A, f.buf, f.len); did = 1; }
        if (!did) break;
        if (++spin > 100) { printf("PUMP STUCK\n"); exit(2); }
    }
}

/* ---------------- tests ---------------- */

static long unhex(const char *s, uint8_t *b, size_t cap) {
    size_t n = 0; int hi = -1;
    for (; *s; s++) {
        if (*s == ' ') continue;
        int v = (*s>='0'&&*s<='9')?*s-'0':(*s>='a'&&*s<='f')?*s-'a'+10:(*s>='A'&&*s<='F')?*s-'A'+10:-1;
        if (v < 0) return -1;
        if (hi < 0) hi = v; else { if (n>=cap) return -1; b[n++]=(uint8_t)((hi<<4)|v); hi=-1; }
    }
    return hi<0 ? (long)n : -1;
}

static void test_header(void) {
    printf("[header]\n");
    wh_hdr h;
    memset(&h, 0, sizeof(h));
    h.version = WH_PROTO_VERSION;
    h.type = WH_MSG_PING;
    h.flags = WH_FLAG_REQ;
    h.src_id = 0x1234;
    h.dst_id = 0xABCD;
    h.seq = 0x11223344;
    h.corr = 0x5566;
    h.msg_id = 0x0009;
    h.frag_index = 0;
    h.frag_total = 1;
    h.payload_len = 5;
    uint8_t out[WH_HDR_LEN];
    int n = wh_hdr_encode(&h, out, sizeof(out));
    CHECK(n == WH_HDR_LEN, "hdr encode length == 22");

    /* byte-exact expected (LE): 57 48 | 01 | 06 | 02 | 00 | 3412 | cdab | 44332211 | 6655 | 0900 | 00 | 01 | 0500 */
    uint8_t want[WH_HDR_LEN];
    unhex("5748 01 06 02 00 3412 cdab 44332211 6655 0900 00 01 0500", want, sizeof(want));
    CHECK(memcmp(out, want, WH_HDR_LEN) == 0, "hdr byte-exact vector");

    wh_hdr d;
    int r = wh_hdr_decode(out, sizeof(out), &d);
    CHECK(r == WH_OK, "hdr decode ok");
    CHECK(d.src_id==0x1234 && d.dst_id==0xABCD && d.seq==0x11223344 &&
          d.corr==0x5566 && d.type==WH_MSG_PING && d.payload_len==5, "hdr fields round-trip");

    /* validation */
    uint8_t bad[WH_HDR_LEN]; memcpy(bad, out, WH_HDR_LEN);
    bad[0] = 0x00;
    CHECK(wh_hdr_decode(bad, WH_HDR_LEN, &d) == WH_ERR_MAGIC, "hdr rejects bad magic");
    memcpy(bad, out, WH_HDR_LEN); bad[2] = 0x02;
    CHECK(wh_hdr_decode(bad, WH_HDR_LEN, &d) == WH_ERR_VERSION, "hdr rejects bad version");
    memcpy(bad, out, WH_HDR_LEN); bad[5] = 0x01;
    CHECK(wh_hdr_decode(bad, WH_HDR_LEN, &d) == WH_ERR_RESERVED, "hdr rejects nonzero reserved");
    CHECK(wh_hdr_decode(out, 10, &d) == WH_ERR_SHORT, "hdr rejects short buffer");
}

static void test_messages(void) {
    printf("[messages]\n");
    uint8_t buf[128];

    wh_announce a; memset(&a, 0, sizeof(a));
    a.proto_version = 1; a.role = WH_ROLE_RING; a.caps = WH_CAPS_RING;
    a.battery_pct = 77; a.flags = 0x01; a.short_id = 0xBEEF;
    strcpy(a.nick, "ring-01");
    int n = wh_enc_announce(&a, buf, sizeof(buf));
    wh_announce a2; int r = wh_dec_announce(buf, (size_t)n, &a2);
    CHECK(r == WH_OK && a2.caps == a.caps && a2.short_id == 0xBEEF &&
          a2.battery_pct == 77 && strcmp(a2.nick, "ring-01") == 0, "announce round-trip");

    wh_gps_fix g; memset(&g, 0, sizeof(g));
    g.lat_1e7 = 377749000; g.lon_1e7 = -1224194000; g.alt_mm = 52000;
    g.hacc_mm = 3500; g.unix_seconds = 1754870400LL; g.millis = 250;
    g.speed_mms = 1200; g.heading_cdeg = 18000; g.fix_type = 3; g.sats = 11;
    n = wh_enc_gps_fix(&g, buf, sizeof(buf));
    wh_gps_fix g2; r = wh_dec_gps_fix(buf, (size_t)n, &g2);
    CHECK(r == WH_OK && g2.lat_1e7 == 377749000 && g2.lon_1e7 == -1224194000 &&
          g2.alt_mm == 52000 && g2.unix_seconds == 1754870400LL && g2.sats == 11 &&
          g2.fix_type == 3, "gps_fix round-trip (incl. negative lon)");

    wh_time_sync t; memset(&t, 0, sizeof(t));
    t.unix_seconds = 1754870400LL; t.millis = 512; t.source = WH_TIME_GPS; t.valid = 1;
    n = wh_enc_time_sync(&t, buf, sizeof(buf));
    wh_time_sync t2; r = wh_dec_time_sync(buf, (size_t)n, &t2);
    CHECK(r == WH_OK && t2.unix_seconds == t.unix_seconds && t2.source == WH_TIME_GPS &&
          t2.valid == 1, "time_sync round-trip");

    /* ---- timesync REQ/RESP: byte-exact layout the ring built to (LE, payload only) ---- */
    wh_timesync_req q; q.seq = 0x5A; q.t1_ring_ns = 0x1122334455667788LL;
    n = wh_enc_timesync_req(&q, buf, sizeof(buf));
    CHECK(n == 9, "timesync REQ payload == 9 bytes");
    CHECK(buf[0] == 0x5A, "REQ seq at off 0");
    CHECK(buf[1]==0x88 && buf[2]==0x77 && buf[8]==0x11, "REQ t1 little-endian at off 1");
    wh_timesync_req q2; r = wh_dec_timesync_req(buf, (size_t)n, &q2);
    CHECK(r == WH_OK && q2.seq == 0x5A && q2.t1_ring_ns == q.t1_ring_ns, "REQ round-trip");
    CHECK(wh_dec_timesync_req(buf, 8, &q2) == WH_ERR_SHORT, "REQ rejects short buffer");

    wh_timesync_resp rs;
    rs.seq = 0x5A; rs.t1_ring_ns = 0x1122334455667788LL;
    rs.t2_watch_ns = 0x0A0B0C0D0E0F1011LL; rs.t3_watch_ns = 0x2021222324252627LL;
    rs.watch_unc_ns = 0xDEADBEEFu; rs.watch_locked = 1;
    n = wh_enc_timesync_resp(&rs, buf, sizeof(buf));
    CHECK(n == 30, "timesync RESP payload == 30 bytes");
    /* offsets: seq@0, t1@1, t2@9, t3@17, unc@25, locked@29 */
    CHECK(buf[0]==0x5A, "RESP seq @0");
    CHECK(buf[1]==0x88 && buf[8]==0x11, "RESP t1 echo LE @1");
    CHECK(buf[9]==0x11 && buf[16]==0x0A, "RESP t2 LE @9");
    CHECK(buf[17]==0x27 && buf[24]==0x20, "RESP t3 LE @17");
    CHECK(buf[25]==0xEF && buf[26]==0xBE && buf[27]==0xAD && buf[28]==0xDE, "RESP unc u32 LE @25");
    CHECK(buf[29]==0x01, "RESP locked @29");
    wh_timesync_resp rs2; r = wh_dec_timesync_resp(buf, (size_t)n, &rs2);
    CHECK(r == WH_OK && rs2.seq==0x5A && rs2.t1_ring_ns==rs.t1_ring_ns &&
          rs2.t2_watch_ns==rs.t2_watch_ns && rs2.t3_watch_ns==rs.t3_watch_ns &&
          rs2.watch_unc_ns==0xDEADBEEFu && rs2.watch_locked==1, "RESP round-trip");
    CHECK(wh_dec_timesync_resp(buf, 29, &rs2) == WH_ERR_SHORT, "RESP rejects short buffer");

    wh_device_seen d; memset(&d, 0, sizeof(d));
    d.kind = WH_SEEN_WIFI_AP; d.rssi = -63; d.channel = 6; d.flags = 0x02;
    d.sec = 4 /* wpa2 */; d.freq_mhz = 2437;
    uint8_t mac[6] = {0xDE,0xAD,0xBE,0xEF,0x00,0x11}; memcpy(d.addr, mac, 6);
    strcpy(d.name, "CoffeeShop_5G");
    d.utc_ns = 0x0102030405060708LL; d.mono_ns = 0x1112131415161718LL;
    d.utc_valid = 1; d.sync_delay_ns = 0x00ABCDEFu;
    n = wh_enc_device_seen(&d, buf, sizeof(buf));
    wh_device_seen d2; r = wh_dec_device_seen(buf, (size_t)n, &d2);
    CHECK(r == WH_OK && d2.rssi == -63 && d2.channel == 6 && d2.flags == 0x02 &&
          d2.sec == 4 && d2.freq_mhz == 2437 &&
          memcmp(d2.addr, mac, 6) == 0 && strcmp(d2.name, "CoffeeShop_5G") == 0,
          "device_seen round-trip (incl. sec/freq + negative rssi)");
    CHECK(d2.utc_ns == d.utc_ns && d2.mono_ns == d.mono_ns &&
          d2.utc_valid == 1 && d2.sync_delay_ns == 0x00ABCDEFu,
          "device_seen 3B tail round-trip");

    /* 3-char name -> full frame == 38 bytes (base 13 + name 4 + tail 21), matches ring */
    wh_device_seen d3; memset(&d3, 0, sizeof(d3));
    d3.kind = WH_SEEN_WIFI_AP; strcpy(d3.name, "abc"); d3.utc_valid = 1;
    int n3 = wh_enc_device_seen(&d3, buf, sizeof(buf));
    CHECK(n3 == 38, "device_seen with 3-char name == 38 bytes");
    /* legacy/short frame: cut before the 21-byte tail -> base parses, utc_valid == 0 */
    wh_device_seen d4; r = wh_dec_device_seen(buf, (size_t)(n3 - 21), &d4);
    CHECK(r == WH_OK && d4.utc_valid == 0 && strcmp(d4.name, "abc") == 0,
          "device_seen legacy tail-absent decode -> utc_valid=0");

    /* ---- THREAT_EVENT (0x61): sizes + LE offsets + round-trip ---- */
    wh_threat_event te; memset(&te, 0, sizeof(te));
    te.klass = 0 /*FLOCK*/; te.confidence = 90; te.rssi = -55;
    te.id_kind = 1; te.id_len = 6;
    uint8_t fid[6] = {0xB4,0x1E,0x52,0x11,0x22,0x33}; memcpy(te.id, fid, 6);
    te.utc_ns = 0x2233445566778899LL;
    n = wh_enc_threat_event(&te, buf, sizeof(buf));
    CHECK(n == 20, "threat MAC id(6) payload == 20 bytes");
    CHECK(buf[0]==0 && buf[1]==90, "threat klass@0 confidence@1");
    CHECK(buf[2]==0xC9 && buf[3]==0xFF, "threat rssi -55 i16 LE @2");     /* -55 = 0xFFC9 */
    CHECK(buf[4]==1 && buf[5]==6, "threat id_kind@4 id_len@5");
    CHECK(buf[6]==0xB4 && buf[11]==0x33, "threat id MSB-first @6");
    CHECK(buf[12]==0x99 && buf[19]==0x22, "threat utc_ns LE @6+id_len");
    wh_threat_event te2; r = wh_dec_threat_event(buf, (size_t)n, &te2);
    CHECK(r == WH_OK && te2.klass==0 && te2.confidence==90 && te2.rssi==-55 &&
          te2.id_len==6 && memcmp(te2.id, fid, 6)==0 && te2.utc_ns==te.utc_ns,
          "threat MAC round-trip");

    wh_threat_event tp; memset(&tp, 0, sizeof(tp));
    tp.klass=3; tp.confidence=70; tp.rssi=0x7FFF; tp.id_kind=2; tp.id_len=2; tp.id[0]=0xAB; tp.id[1]=0xCD;
    CHECK(wh_enc_threat_event(&tp, buf, sizeof(buf)) == 16, "threat PAN id(2) == 16 bytes");
    wh_threat_event tn; memset(&tn, 0, sizeof(tn)); tn.klass=2; tn.id_len=0;
    CHECK(wh_enc_threat_event(&tn, buf, sizeof(buf)) == 14, "threat no-id == 14 bytes");
    wh_threat_event tbad; memset(&tbad,0,sizeof(tbad)); tbad.id_len=9;
    CHECK(wh_enc_threat_event(&tbad, buf, sizeof(buf)) == WH_ERR_ARG, "threat rejects id_len>8");

    /* ---- LAN_NET (0x64): byte-exact + NUL-terminated ssid + round-trip ---- */
    wh_lan_net ln; memset(&ln, 0, sizeof ln);
    uint8_t lgw[6]  = {0x02,0x11,0x22,0x33,0x44,0x55};
    uint8_t lsub[4] = {0xC0,0xA8,0x01,0x00};
    uint8_t lbss[6] = {0xE0,0xD3,0x62,0xD3,0xAC,0x94};
    memcpy(ln.gw_mac, lgw, 6); memcpy(ln.subnet_base, lsub, 4); ln.prefix = 24;
    memcpy(ln.bssid, lbss, 6); strcpy(ln.ssid, "SCN");
    n = wh_enc_lan_net(&ln, buf, sizeof(buf));
    CHECK(n == 21, "lan_net payload == 21 bytes (17 head + SCN + NUL)");
    {
        uint8_t wantn[32];
        unhex("02 11 22 33 44 55 C0 A8 01 00 18 E0 D3 62 D3 AC 94 53 43 4E 00", wantn, sizeof wantn);
        CHECK(memcmp(buf, wantn, 21) == 0, "lan_net byte-exact vector");
    }
    wh_lan_net ln2; r = wh_dec_lan_net(buf, (size_t)n, &ln2);
    CHECK(r == WH_OK && memcmp(ln2.gw_mac,lgw,6)==0 && memcmp(ln2.subnet_base,lsub,4)==0 &&
          ln2.prefix==24 && memcmp(ln2.bssid,lbss,6)==0 && strcmp(ln2.ssid,"SCN")==0,
          "lan_net round-trip");
    ln.ssid[0] = '\0';
    CHECK(wh_enc_lan_net(&ln, buf, sizeof(buf)) == 18, "lan_net empty ssid == 18 bytes");
    CHECK(wh_dec_lan_net(buf, 18, &ln2) == WH_OK && ln2.ssid[0]=='\0', "lan_net empty ssid round-trip");

    /* ---- LAN_CLIENT (0x65): byte-exact + i16 rtt + length-guarded 17-byte tail ---- */
    wh_lan_client_msg lc; memset(&lc, 0, sizeof lc);
    uint8_t lip[4]  = {0xC0,0xA8,0x01,0x32};
    uint8_t lmac[6] = {0x06,0xAA,0xBB,0xCC,0xDD,0xEE};
    memcpy(lc.gw_mac, lgw, 6); memcpy(lc.ip, lip, 4); memcpy(lc.mac, lmac, 6);
    lc.src = 0x03 /*ARP|PING*/; lc.rtt_ms = 5;
    lc.mono_ns = 0x0102030405060708LL; lc.utc_ns = 0x1122334455667788LL; lc.utc_valid = 1;
    n = wh_enc_lan_client(&lc, buf, sizeof(buf));
    CHECK(n == 36, "lan_client payload == 36 bytes (19 head + 17 tail)");
    {
        uint8_t wantc[48];
        unhex("02 11 22 33 44 55 C0 A8 01 32 06 AA BB CC DD EE 03 05 00 "
              "08 07 06 05 04 03 02 01 88 77 66 55 44 33 22 11 01", wantc, sizeof wantc);
        CHECK(memcmp(buf, wantc, 36) == 0, "lan_client byte-exact vector (mono@19, utc@27, valid@35)");
    }
    wh_lan_client_msg lc2; r = wh_dec_lan_client(buf, (size_t)n, &lc2);
    CHECK(r == WH_OK && memcmp(lc2.ip,lip,4)==0 && memcmp(lc2.mac,lmac,6)==0 && lc2.src==0x03 &&
          lc2.rtt_ms==5 && lc2.mono_ns==0x0102030405060708LL && lc2.utc_ns==0x1122334455667788LL &&
          lc2.utc_valid==1, "lan_client round-trip");
    lc.rtt_ms = -2 /*WH_LAN_RTT_TIMEOUT*/;
    wh_enc_lan_client(&lc, buf, sizeof(buf));
    CHECK(wh_dec_lan_client(buf, 36, &lc2) == WH_OK && lc2.rtt_ms == -2, "lan_client negative rtt (timeout) round-trip");
    CHECK(wh_dec_lan_client(buf, 19, &lc2) == WH_OK && lc2.utc_valid == 0, "lan_client head-only (19B) -> utc_valid 0");
    CHECK(wh_dec_lan_client(buf, 10, &lc2) == WH_ERR_SHORT, "lan_client rejects truncated head");

    /* ---- HID proxy codec (byte-exact per kind, signedness, guards) ---- */
    wh_hid_proxy pa; memset(&pa, 0, sizeof pa);
    pa.kind = WH_HID_ACTION; pa.u.action_id = 0 /*play/pause*/;
    n = wh_enc_hid_proxy(&pa, buf, sizeof buf);
    CHECK(n == 3, "hid action payload == 3 bytes");
    { uint8_t want[3] = {0x05,0x00,0x00}; CHECK(memcmp(buf,want,3)==0, "hid action byte-exact"); }
    wh_hid_proxy pa2; CHECK(wh_dec_hid_proxy(buf,(size_t)n,&pa2)==WH_OK &&
        pa2.kind==WH_HID_ACTION && pa2.u.action_id==0, "hid action round-trip");

    wh_hid_proxy pc; memset(&pc, 0, sizeof pc);
    pc.kind = WH_HID_CONSUMER; pc.target = 2; pc.u.consumer = 0x00CD;
    n = wh_enc_hid_proxy(&pc, buf, sizeof buf);
    CHECK(n == 4, "hid consumer payload == 4 bytes");
    { uint8_t want[4] = {0x03,0x02,0xCD,0x00}; CHECK(memcmp(buf,want,4)==0, "hid consumer usage LE byte-exact"); }
    wh_hid_proxy pc2; CHECK(wh_dec_hid_proxy(buf,(size_t)n,&pc2)==WH_OK &&
        pc2.u.consumer==0x00CD && pc2.target==2, "hid consumer round-trip");

    wh_hid_proxy pm; memset(&pm, 0, sizeof pm);
    pm.kind = WH_HID_MOUSE; pm.u.mouse.buttons = 0x01;
    pm.u.mouse.dx = -5; pm.u.mouse.dy = 7; pm.u.mouse.wheel = -1;
    n = wh_enc_hid_proxy(&pm, buf, sizeof buf);
    CHECK(n == 6, "hid mouse payload == 6 bytes");
    { uint8_t want[6] = {0x02,0x00,0x01,0xFB,0x07,0xFF}; CHECK(memcmp(buf,want,6)==0, "hid mouse signed byte-exact"); }
    wh_hid_proxy pm2; CHECK(wh_dec_hid_proxy(buf,(size_t)n,&pm2)==WH_OK &&
        pm2.u.mouse.dx==-5 && pm2.u.mouse.dy==7 && pm2.u.mouse.wheel==-1, "hid mouse signed round-trip");

    wh_hid_proxy pk; memset(&pk, 0, sizeof pk);
    pk.kind = WH_HID_KBD; pk.target = 1;
    uint8_t rep[8] = {0x02,0x00,0x04,0,0,0,0,0}; memcpy(pk.u.kbd, rep, 8);
    n = wh_enc_hid_proxy(&pk, buf, sizeof buf);
    CHECK(n == 10, "hid kbd payload == 10 bytes");
    wh_hid_proxy pk2; CHECK(wh_dec_hid_proxy(buf,(size_t)n,&pk2)==WH_OK &&
        memcmp(pk2.u.kbd, rep, 8)==0, "hid kbd report round-trip");

    wh_hid_proxy pt; memset(&pt, 0, sizeof pt);
    pt.kind = WH_HID_TYPE; pt.u.type.len = 2; memcpy(pt.u.type.text, "hi", 2);
    n = wh_enc_hid_proxy(&pt, buf, sizeof buf);
    CHECK(n == 5, "hid type 'hi' payload == 5 bytes");
    { uint8_t want[5] = {0x04,0x00,0x02,'h','i'}; CHECK(memcmp(buf,want,5)==0, "hid type byte-exact"); }
    wh_hid_proxy pt2; CHECK(wh_dec_hid_proxy(buf,(size_t)n,&pt2)==WH_OK &&
        pt2.u.type.len==2 && memcmp(pt2.u.type.text,"hi",2)==0, "hid type round-trip");

    CHECK(wh_enc_hid_proxy(&pk, buf, 4) == WH_ERR_SHORT, "hid enc rejects short buffer");
    { uint8_t badk[3] = {0x09,0x00,0x00}; wh_hid_proxy pb;
      CHECK(wh_dec_hid_proxy(badk, 3, &pb) == WH_ERR_ARG, "hid dec rejects unknown kind"); }
    { uint8_t ovl[4] = {0x04,0x00,0xFF,0x00}; wh_hid_proxy po;   /* len 255 > max, guarded before read */
      CHECK(wh_dec_hid_proxy(ovl, 4, &po) == WH_ERR_ARG, "hid dec rejects over-long text len"); }

    wh_hid_proxy_ack ack; ack.status = 1; ack.target = 3;
    n = wh_enc_hid_proxy_ack(&ack, buf, sizeof buf);
    CHECK(n == 2, "hid ack == 2 bytes");
    wh_hid_proxy_ack ack2; CHECK(wh_dec_hid_proxy_ack(buf, 2, &ack2)==WH_OK &&
        ack2.status==1 && ack2.target==3, "hid ack round-trip");
}

static void test_replay(void) {
    printf("[replay window]\n");
    wh_replay w; memset(&w, 0, sizeof(w));
    CHECK(wh__replay_check(&w, 0) == WH_OK, "first seq accepted"); wh__replay_mark(&w, 0);
    CHECK(wh__replay_check(&w, 1) == WH_OK, "next seq accepted");  wh__replay_mark(&w, 1);
    CHECK(wh__replay_check(&w, 1) == WH_ERR_REPLAY, "duplicate rejected");
    CHECK(wh__replay_check(&w, 0) == WH_ERR_REPLAY, "older duplicate rejected");
    wh__replay_mark(&w, 5);
    CHECK(wh__replay_check(&w, 3) == WH_OK, "in-window gap accepted"); wh__replay_mark(&w, 3);
    CHECK(wh__replay_check(&w, 3) == WH_ERR_REPLAY, "reordered duplicate rejected");
    wh__replay_mark(&w, 200);
    CHECK(wh__replay_check(&w, 100) == WH_ERR_REPLAY, "far-old rejected after big jump");
    CHECK(wh__replay_check(&w, 201) == WH_OK, "new max accepted");
}

static void test_nonce(void) {
    printf("[nonce]\n");
    uint8_t prefix[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
    uint8_t nonce[12];
    wh__build_nonce(prefix, 0x0A0B0C0D, nonce);
    uint8_t want[12]; unhex("1122334455667788 0d0c0b0a", want, sizeof(want));
    CHECK(memcmp(nonce, want, 12) == 0, "nonce = prefix(8) || ctr_le(4)");
}

static void setup_pair(endpoint *ea, endpoint *eb, wh_ctx *A, wh_ctx *B,
                       queue_t *a2b, queue_t *b2a) {
    memset(ea, 0, sizeof(*ea)); memset(eb, 0, sizeof(*eb));
    memset(a2b, 0, sizeof(*a2b)); memset(b2a, 0, sizeof(*b2a));
    ea->name = "ring"; ea->tx = a2b; ea->prng = 0xA11CE5EEDULL; ea->paired = -1;
    eb->name = "watch"; eb->tx = b2a; eb->prng = 0xB0BCAFE00ULL; eb->paired = -1;

    wh_config ca; memset(&ca, 0, sizeof(ca));
    ca.local_id = 0x0001; ca.peer_id = 0x0002; ca.role = WH_ROLE_RING; ca.caps = WH_CAPS_RING;
    strcpy(ca.nick, "ring"); ca.rng = ep_rng; ca.now_ms = ep_now; ca.transport_send = ep_send;
    ca.on_message = ep_on_msg; ca.on_sas = ep_on_sas; ca.on_paired = ep_on_paired; ca.user = ea;

    wh_config cb; memset(&cb, 0, sizeof(cb));
    cb.local_id = 0x0002; cb.peer_id = 0x0001; cb.role = WH_ROLE_WATCH; cb.caps = WH_CAPS_WATCH;
    strcpy(cb.nick, "watch"); cb.rng = ep_rng; cb.now_ms = ep_now; cb.transport_send = ep_send;
    cb.on_message = ep_on_msg; cb.on_sas = ep_on_sas; cb.on_paired = ep_on_paired; cb.user = eb;

    wh_ctx_init(A, &ca);
    wh_ctx_init(B, &cb);
}

static void test_pairing_and_session(void) {
    printf("[pairing + session]\n");
    endpoint ea, eb; wh_ctx A, B; queue_t a2b, b2a;
    setup_pair(&ea, &eb, &A, &B, &a2b, &b2a);

    int r = wh_pair_start(&A);
    CHECK(r == WH_OK, "pair_start ok");
    pump(&a2b, &b2a, &A, &B);

    CHECK(ea.sas_count == 1 && eb.sas_count == 1, "both sides produced a SAS");
    CHECK(strlen(ea.last_sas) == 6 && strcmp(ea.last_sas, eb.last_sas) == 0,
          "SAS matches on both devices");
    CHECK(A.pair.state == WH_PS_AWAIT_SAS && B.pair.state == WH_PS_AWAIT_SAS,
          "both await user confirm");

    /* user confirms on both */
    wh_pair_confirm(&A, 1);
    wh_pair_confirm(&B, 1);
    CHECK(ea.paired == 1 && eb.paired == 1, "both report paired");
    CHECK(wh_is_established(&A) && wh_is_established(&B), "sessions established");

    /* keys mirrored: A.tx == B.rx and vice versa */
    CHECK(memcmp(A.sess.tx_key, B.sess.rx_key, WH_KEY_LEN) == 0 &&
          memcmp(A.sess.rx_key, B.sess.tx_key, WH_KEY_LEN) == 0 &&
          memcmp(A.sess.tx_prefix, B.sess.rx_prefix, WH_NONCE_PREFIX_LEN) == 0 &&
          memcmp(A.sess.rx_prefix, B.sess.tx_prefix, WH_NONCE_PREFIX_LEN) == 0,
          "directional keys/prefixes mirrored");

    /* encrypted app message A -> B */
    const char *ping = "hello-watch";
    r = wh_send(&A, WH_MSG_PING, WH_FLAG_REQ, 0x0042, (const uint8_t *)ping, strlen(ping));
    CHECK(r == WH_OK, "encrypted send ok");
    pump(&a2b, &b2a, &A, &B);
    CHECK(eb.rx_count == 1 && eb.rx_type == WH_MSG_PING && eb.rx_corr == 0x0042 &&
          eb.rx_len == strlen(ping) && memcmp(eb.rx_buf, ping, eb.rx_len) == 0,
          "watch received decrypted PING with corr");

    /* response B -> A */
    const char *pong = "hi-ring";
    wh_send(&B, WH_MSG_PONG, WH_FLAG_RESP, 0x0042, (const uint8_t *)pong, strlen(pong));
    pump(&a2b, &b2a, &A, &B);
    CHECK(ea.rx_count == 1 && ea.rx_type == WH_MSG_PONG &&
          ea.rx_len == strlen(pong) && memcmp(ea.rx_buf, pong, ea.rx_len) == 0,
          "ring received decrypted PONG");

    /* replay: re-inject the last frame A sent (the PING) -> must be rejected */
    int before = eb.rx_count;
    /* A's last sent was the PONG's... no: capture PING earlier. Re-send a fresh
     * encrypted frame and replay it. */
    wh_send(&A, WH_MSG_STATUS, WH_FLAG_EVENT, 0, (const uint8_t *)"x", 1);
    frame_t cap; memset(&cap, 0, sizeof(cap));
    memcpy(cap.buf, ea.last_sent, ea.last_sent_len); cap.len = ea.last_sent_len;
    pump(&a2b, &b2a, &A, &B);              /* delivers it once (accepted) */
    int after_first = eb.rx_count;
    int rr = wh_on_frame(&B, cap.buf, cap.len);  /* replay the same frame */
    CHECK(after_first == before + 1 && rr == WH_ERR_REPLAY && eb.rx_count == after_first,
          "replayed frame rejected, not re-delivered");

    /* tamper: corrupt a ciphertext byte on a FRESH-seq frame so it passes the
     * replay pre-check and reaches the AEAD verification (which must reject).
     * Deliver the tampered copy BEFORE the legit frame is pumped. */
    wh_send(&A, WH_MSG_STATUS, WH_FLAG_EVENT, 0, (const uint8_t *)"tamperme", 8);
    frame_t tf; memcpy(tf.buf, ea.last_sent, ea.last_sent_len); tf.len = ea.last_sent_len;
    int rc_before = eb.rx_count;
    tf.buf[WH_HDR_LEN + 2] ^= 0x40;   /* corrupt ciphertext (fresh seq) */
    int rt = wh_on_frame(&B, tf.buf, tf.len);
    CHECK(rt == WH_ERR_AUTH && eb.rx_count == rc_before, "tampered ciphertext rejected (AEAD)");
    /* the legit frame is still queued; it must still deliver (failed-auth frame
     * did NOT advance the replay window) */
    pump(&a2b, &b2a, &A, &B);
    CHECK(eb.rx_count == rc_before + 1, "failed-auth frame did not block the legit one");
}

static void test_fragmentation(void) {
    printf("[fragmentation]\n");
    endpoint ea, eb; wh_ctx A, B; queue_t a2b, b2a;
    setup_pair(&ea, &eb, &A, &B, &a2b, &b2a);
    wh_pair_start(&A); pump(&a2b, &b2a, &A, &B);
    wh_pair_confirm(&A, 1); wh_pair_confirm(&B, 1);

    /* build a payload that spans several encrypted fragments (>212 each) */
    size_t big = 1000;
    uint8_t *msg = (uint8_t *)malloc(big);
    for (size_t i = 0; i < big; i++) msg[i] = (uint8_t)(i * 31 + 7);
    int r = wh_send(&A, WH_MSG_FILE_CHUNK, WH_FLAG_EVENT, 0, msg, big);
    CHECK(r == WH_OK, "fragmented send ok");
    pump(&a2b, &b2a, &A, &B);
    CHECK(eb.rx_count == 1 && eb.rx_len == big && memcmp(eb.rx_buf, msg, big) == 0,
          "large message reassembled byte-exact across fragments");

    /* exact multiple of cap boundary */
    size_t exact = (size_t)WH_FRAG_CAP_ENC * 3;
    uint8_t *m2 = (uint8_t *)malloc(exact);
    for (size_t i = 0; i < exact; i++) m2[i] = (uint8_t)(i ^ 0xA5);
    eb.rx_count = 0;
    wh_send(&A, WH_MSG_FILE_CHUNK, WH_FLAG_EVENT, 0, m2, exact);
    pump(&a2b, &b2a, &A, &B);
    CHECK(eb.rx_count == 1 && eb.rx_len == exact && memcmp(eb.rx_buf, m2, exact) == 0,
          "exact-multiple-of-cap message reassembled");
    free(msg); free(m2);
}

static void test_mitm(void) {
    printf("[MITM / handshake tamper]\n");
    /* A active MITM would run two handshakes yielding different SAS on each
     * side; here we model tampering the wire (which either breaks the
     * commitment check or diverges the SAS). Flip a byte in PKr in transit. */
    endpoint ea, eb; wh_ctx A, B; queue_t a2b, b2a;
    setup_pair(&ea, &eb, &A, &B, &a2b, &b2a);

    wh_pair_start(&A);                          /* A -> PAIR_REQ (PKi) */
    /* deliver REQ to B, let B emit RESP */
    frame_t f;
    q_pop(&a2b, &f); wh_on_frame(&B, f.buf, f.len);
    /* B's RESP is now in b2a: corrupt PKr (first payload byte after header) */
    q_pop(&b2a, &f);
    f.buf[WH_HDR_LEN + 0] ^= 0x80;              /* tamper responder ephemeral pub */
    wh_on_frame(&A, f.buf, f.len);              /* A stores tampered PKr, sends NA */
    /* finish the exchange */
    pump(&a2b, &b2a, &A, &B);

    /* Initiator verifies the commitment over the (tampered) PKr it holds vs the
     * Nr it receives; the responder committed over the real PKr, so they differ
     * -> commitment check fails, OR the SAS diverges. Either way pairing must
     * NOT succeed silently with matching SAS. */
    int sas_mismatch = (ea.sas_count == 0) || (eb.sas_count == 0) ||
                       (strcmp(ea.last_sas, eb.last_sas) != 0);
    int failed = (A.pair.state == WH_PS_FAILED) || (B.pair.state == WH_PS_FAILED);
    CHECK(sas_mismatch || failed, "handshake tamper detected (commitment fail or SAS mismatch)");
    CHECK(!(wh_is_established(&A) && wh_is_established(&B)) || sas_mismatch,
          "tamper does not yield a silently-matching established session");
}


static void test_lora_bridge(void) {
    uint8_t buf[300];

    /* --- LORA_TX round-trip (broadcast text message) --- */
    wh_lora_tx tx; memset(&tx, 0, sizeof(tx));
    tx.dest = 0xFFFFFFFFu; tx.channel = 0; tx.want_ack = 1; tx.hop_limit = 3;
    tx.portnum = WH_PORT_TEXT;
    const char *msg = "ping from the ring";
    tx.payload_len = (uint16_t)strlen(msg);
    memcpy(tx.payload, msg, tx.payload_len);
    int n = wh_enc_lora_tx(&tx, buf, sizeof(buf));
    wh_lora_tx tx2;
    int r = wh_dec_lora_tx(buf, (size_t)n, &tx2);
    CHECK(n > 0 && r == WH_OK && tx2.dest == 0xFFFFFFFFu && tx2.want_ack == 1 &&
          tx2.hop_limit == 3 && tx2.portnum == WH_PORT_TEXT &&
          tx2.payload_len == tx.payload_len &&
          memcmp(tx2.payload, msg, tx2.payload_len) == 0, "LORA_TX round-trip");

    /* --- LORA_TX at max payload --- */
    wh_lora_tx txm; memset(&txm, 0, sizeof(txm));
    txm.dest = 42; txm.portnum = WH_PORT_PRIVATE; txm.payload_len = WH_LORA_MAX_PAYLOAD;
    for (int i = 0; i < WH_LORA_MAX_PAYLOAD; i++) txm.payload[i] = (uint8_t)(i & 0xFF);
    n = wh_enc_lora_tx(&txm, buf, sizeof(buf));
    wh_lora_tx txm2; r = wh_dec_lora_tx(buf, (size_t)n, &txm2);
    CHECK(n > 0 && r == WH_OK && txm2.payload_len == WH_LORA_MAX_PAYLOAD &&
          memcmp(txm2.payload, txm.payload, WH_LORA_MAX_PAYLOAD) == 0, "LORA_TX max payload round-trip");

    /* --- encoder rejects over-max payload --- */
    wh_lora_tx txbad; memset(&txbad, 0, sizeof(txbad)); txbad.payload_len = WH_LORA_MAX_PAYLOAD + 1;
    CHECK(wh_enc_lora_tx(&txbad, buf, sizeof(buf)) == WH_ERR_ARG, "LORA_TX encoder rejects over-max payload");

    /* --- decoder rejects a frame claiming over-max payload_len --- */
    n = wh_enc_lora_tx(&tx, buf, sizeof(buf));       /* valid frame */
    buf[9] = 234; buf[10] = 0;                        /* payload_len field -> 234 (> max) */
    CHECK(wh_dec_lora_tx(buf, (size_t)n, &tx2) == WH_ERR_SHORT, "LORA_TX decoder rejects over-max payload_len");

    /* --- LORA_RX round-trip (negative rssi/snr) --- */
    wh_lora_rx rx; memset(&rx, 0, sizeof(rx));
    rx.from = 0x11223344; rx.rx_time = 1754870400u; rx.channel = 2;
    rx.rssi = -119; rx.snr_q4 = -37; rx.portnum = WH_PORT_TEXT;
    const char *rmsg = "ack";
    rx.payload_len = (uint16_t)strlen(rmsg); memcpy(rx.payload, rmsg, rx.payload_len);
    n = wh_enc_lora_rx(&rx, buf, sizeof(buf));
    wh_lora_rx rx2; r = wh_dec_lora_rx(buf, (size_t)n, &rx2);
    CHECK(n > 0 && r == WH_OK && rx2.from == 0x11223344 && rx2.rssi == -119 &&
          rx2.snr_q4 == -37 && rx2.channel == 2 && rx2.payload_len == rx.payload_len &&
          memcmp(rx2.payload, rmsg, rx2.payload_len) == 0, "LORA_RX round-trip (negative rssi/snr)");

    /* --- LORA_RX_SUB round-trip --- */
    wh_lora_sub sub; memset(&sub, 0, sizeof(sub));
    sub.enable = 1; sub.channel_mask = 0x05; sub.n_ports = 2;
    sub.ports[0] = WH_PORT_TEXT; sub.ports[1] = WH_PORT_PRIVATE;
    n = wh_enc_lora_sub(&sub, buf, sizeof(buf));
    wh_lora_sub sub2; r = wh_dec_lora_sub(buf, (size_t)n, &sub2);
    CHECK(n > 0 && r == WH_OK && sub2.enable == 1 && sub2.channel_mask == 0x05 &&
          sub2.n_ports == 2 && sub2.ports[0] == WH_PORT_TEXT && sub2.ports[1] == WH_PORT_PRIVATE,
          "LORA_RX_SUB round-trip");
    /* n_ports > 8 rejected */
    wh_lora_sub subbad; memset(&subbad, 0, sizeof(subbad)); subbad.n_ports = 9;
    CHECK(wh_enc_lora_sub(&subbad, buf, sizeof(buf)) == WH_ERR_ARG, "LORA_RX_SUB rejects n_ports>8");

    /* --- LOG_WRITE (logrec) round-trip carrying an NDJSON line --- */
    wh_logrec_msg lr; memset(&lr, 0, sizeof(lr));
    lr.fmt = WH_LOGREC_FMT_NDJSON;
    const char *line = "{\"v\":1,\"t\":\"evt\",\"ts\":5,\"sev\":1,\"code\":2,\"msg\":\"hi\"}";
    lr.len = (uint16_t)strlen(line); memcpy(lr.data, line, lr.len);
    n = wh_enc_logrec(&lr, buf, sizeof(buf));
    wh_logrec_msg lr2; r = wh_dec_logrec(buf, (size_t)n, &lr2);
    CHECK(n > 0 && r == WH_OK && lr2.fmt == 0 && lr2.len == lr.len &&
          memcmp(lr2.data, line, lr2.len) == 0, "LOG_WRITE round-trip (NDJSON line)");
    wh_logrec_msg lrbad; memset(&lrbad, 0, sizeof(lrbad)); lrbad.len = WH_LOGREC_MAX + 1;
    CHECK(wh_enc_logrec(&lrbad, buf, sizeof(buf)) == WH_ERR_ARG, "LOG_WRITE rejects over-max record");

    /* --- default injection policy (trust boundary) --- */
    CHECK(wh_lora_port_allowed_default(WH_PORT_TEXT) == 1 &&
          wh_lora_port_allowed_default(WH_PORT_PRIVATE) == 1 &&
          wh_lora_port_allowed_default(WH_PORT_POSITION) == 0 &&
          wh_lora_port_allowed_default(WH_PORT_NODEINFO) == 0 &&
          wh_lora_port_allowed_default(WH_PORT_TELEMETRY) == 0,
          "default policy: Ring may inject only TEXT + PRIVATE ports");
}

static void test_pairing_broadcast_initiator(void) {
    printf("[pairing: broadcast initiator (real bootstrap)]\n");
    endpoint ea, eb; wh_ctx A, B; queue_t a2b, b2a;
    setup_pair(&ea, &eb, &A, &B, &a2b, &b2a);
    /* The real app hits this: the initiator sends first, so it CANNOT know the
     * peer id yet — peer_id is BROADCAST until the PAIR_RESP header teaches it.
     * The other pairing test pre-sets peer_id and so never caught the deadlock. */
    A.cfg.peer_id = WH_ID_BROADCAST;
    int r = wh_pair_start(&A);
    CHECK(r == WH_OK, "pair_start OK with broadcast peer (regression guard)");
    CHECK(a2b.head != a2b.tail, "PAIR_REQ actually emitted, not silently dropped");
    pump(&a2b, &b2a, &A, &B);
    CHECK(ea.sas_count == 1 && eb.sas_count == 1, "both sides produced a SAS");
    CHECK(strlen(eb.last_sas) == 6 && strcmp(ea.last_sas, eb.last_sas) == 0,
          "SAS matches after broadcast bootstrap");
    wh_pair_confirm(&A, 1);
    wh_pair_confirm(&B, 1);
    CHECK(ea.paired == 1 && eb.paired == 1 && wh_is_established(&A) && wh_is_established(&B),
          "session established from a broadcast-initiated pairing");
}

static void test_status_codec(void) {
    printf("[status codec]\n");
    wh_device_status s = {
        .role = WH_ROLE_RING, .mode = WH_OPMODE_SCANNING,
        .collectors = WH_COLL_WIFI | WH_COLL_BLE, .batt_pct = 73,
        .seen_wifi = 123456, .seen_ble = 789,
        .timer_state = WH_TMR_RUNNING, .timer_target_ms = 1723612345678LL,
        .sw_state = WH_SW_PAUSED, .sw_start_ms = 1723600000000LL, .sw_accum_ms = 42000,
    };
    uint8_t buf[64];
    int n = wh_enc_status(&s, buf, sizeof buf);
    CHECK(n > 0, "status encodes");
    wh_device_status o; memset(&o, 0, sizeof o);
    int d = wh_dec_status(buf, (size_t)n, &o);
    CHECK(d == n, "status decodes full length");
    CHECK(o.role==s.role && o.mode==s.mode && o.collectors==s.collectors &&
          o.batt_pct==s.batt_pct && o.seen_wifi==s.seen_wifi && o.seen_ble==s.seen_ble,
          "status fields round-trip (mode/collectors/counts)");
    CHECK(o.timer_state==s.timer_state && o.timer_target_ms==s.timer_target_ms &&
          o.sw_state==s.sw_state && o.sw_start_ms==s.sw_start_ms && o.sw_accum_ms==s.sw_accum_ms,
          "timer + stopwatch (absolute ms) round-trip exactly");
    CHECK(wh_dec_status(buf, (size_t)n - 1, &o) == WH_ERR_SHORT, "truncated status rejected");
}

static void test_session_persist(void) {
    printf("[session persistence]\n");
    wh_ctx a; memset(&a, 0, sizeof a);
    for (int i = 0; i < WH_KEY_LEN; i++) { a.sess.tx_key[i]=(uint8_t)(0x10+i); a.sess.rx_key[i]=(uint8_t)(0x80+i); }
    for (int i = 0; i < WH_NONCE_PREFIX_LEN; i++) { a.sess.tx_prefix[i]=(uint8_t)(0xA0+i); a.sess.rx_prefix[i]=(uint8_t)(0xC0+i); }
    a.sess.tx_ctr = 1000;
    a.sess.rx_replay.max = 777; a.sess.rx_replay.bits = 0xDEADBEEFu; a.sess.rx_replay.seen = 1;
    a.sess.established = 1;
    a.cfg.peer_id = 0xBEEF;

    uint8_t blob[128];
    int n = wh_session_export(&a, blob, sizeof blob);
    CHECK(n == WH_SESSION_BLOB_LEN, "export writes a 95-byte blob");

    wh_ctx b; memset(&b, 0, sizeof b);
    CHECK(wh_session_import(&b, blob, (size_t)n) == WH_OK, "import ok");
    CHECK(memcmp(a.sess.tx_key, b.sess.tx_key, WH_KEY_LEN)==0 &&
          memcmp(a.sess.rx_key, b.sess.rx_key, WH_KEY_LEN)==0 &&
          memcmp(a.sess.tx_prefix, b.sess.tx_prefix, WH_NONCE_PREFIX_LEN)==0 &&
          memcmp(a.sess.rx_prefix, b.sess.rx_prefix, WH_NONCE_PREFIX_LEN)==0,
          "keys + prefixes round-trip");
    CHECK(b.cfg.peer_id == 0xBEEF, "peer_id round-trips");
    CHECK(b.sess.tx_ctr == 1000 + WH_PERSIST_MARGIN, "tx_ctr jumped forward by MARGIN");
    CHECK(b.sess.rx_replay.max==777 && b.sess.rx_replay.seen==1, "replay max/seen round-trip");
    CHECK(b.sess.rx_replay.bits == 0, "replay bits reset to 0 on import (no stale window)");
    CHECK(b.sess.established && wh_is_established(&b), "restored session is established");

    wh_ctx c; memset(&c, 0, sizeof c);
    CHECK(wh_session_export(&c, blob, sizeof blob) < 0, "export refuses an un-established session");
    CHECK(wh_session_import(&b, blob, WH_SESSION_BLOB_LEN-1) == WH_ERR_SHORT, "truncated blob rejected");
    uint8_t bad[128]; memcpy(bad, blob, sizeof bad); bad[0]='X';
    CHECK(wh_session_import(&b, bad, WH_SESSION_BLOB_LEN) == WH_ERR_ARG, "bad magic rejected");

    a.sess.tx_ctr = 0xFFFFFFFFu - 100u;    /* within 2*MARGIN of the u32 max */
    n = wh_session_export(&a, blob, sizeof blob);
    CHECK(wh_session_import(&b, blob, (size_t)n) == WH_ERR_STATE, "near-wrap counter refused (forces re-pair)");
}

int main(void) {
    printf("=== wh-link protocol tests ===\n");
    test_header();
    test_messages();
    test_replay();
    test_nonce();
    test_pairing_and_session();
    test_pairing_broadcast_initiator();
    test_status_codec();
    test_session_persist();
    test_fragmentation();
    test_mitm();
    test_lora_bridge();
    printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
