/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 John Stockdale (jstockdale@gmail.com)
 */
/*
 * wh_link.h — Whitehat device-to-device link protocol (wh-link) v1
 *
 * Framework-neutral, header-only C11. Shared verbatim by the Whitehat Ring
 * (ESP32-C6, ESP-IDF) and the Whitehat Watch (ESP32-S3, ESP-IDF). Compiles as
 * C or C++, and on the host for verification. No ESP-IDF, Arduino, or platform
 * headers are referenced by the core; the platform provides transport, RNG, a
 * monotonic clock, and (optionally) a crypto backend via callbacks.
 *
 * Layers:
 *   - Wire framing        : fixed 22-byte header + payload (+16B AEAD tag)
 *   - Message taxonomy     : session/link, command/RPC, telemetry
 *   - Crypto envelope      : ChaCha20-Poly1305 AEAD, per-direction keys,
 *                            deterministic 96-bit nonce = prefix(8) || ctr(4)
 *   - Pairing (AKE)        : X25519 + commitment-based numeric-comparison SAS
 *   - Replay protection    : 64-entry sliding window per direction
 *   - Fragmentation        : up to 255 fragments/message, offset-addressed
 *
 * Endianness: ALL multi-byte wire integers are little-endian, written and read
 * with explicit byte ops (never struct memcpy) so the wire format is identical
 * regardless of host endianness. This is a deliberate, tested choice (both
 * endpoints are little-endian ESP32); swapping to big-endian is a one-line
 * change in the rd_/wr_ helpers.
 *
 * stb-style single-header:
 *     #define WH_LINK_IMPLEMENTATION   // in exactly ONE .c file
 *     #include "wh_link.h"
 *
 * License: BSD-3-Clause (intended), matching the surrounding firmware.
 */
#ifndef WH_LINK_H
#define WH_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===================================================================== */
/* Protocol constants                                                    */
/* ===================================================================== */
#define WH_PROTO_VERSION    1
#define WH_MAGIC0           0x57  /* 'W' */
#define WH_MAGIC1           0x48  /* 'H' */

#define WH_HDR_LEN          22
#define WH_TAG_LEN          16
#define WH_MAX_FRAME        250   /* ESP-NOW ESP_NOW_MAX_DATA_LEN */
#define WH_KEY_LEN          32
#define WH_NONCE_LEN        12
#define WH_NONCE_PREFIX_LEN 8     /* 96-bit nonce = prefix(8) || counter(4) */
#define WH_X25519_LEN       32
#define WH_PAIR_NONCE_LEN   16
#define WH_COMMIT_LEN       16
#define WH_SAS_DIGITS       6

/* Max fragment PAYLOAD that still fits one frame, by encryption mode */
#define WH_FRAG_CAP_ENC     (WH_MAX_FRAME - WH_HDR_LEN - WH_TAG_LEN)  /* 212 */
#define WH_FRAG_CAP_PLAIN   (WH_MAX_FRAME - WH_HDR_LEN)               /* 228 */
#define WH_MAX_FRAGS        255

/* Reassembly buffer cap (overridable). Bulk transfers (files, screenshots)
 * are chunked at the application layer into messages that each fit this. */
#ifndef WH_REASM_MAX
#define WH_REASM_MAX        4096
#endif

#define WH_NICK_MAX         16
#define WH_ADDR_LEN         6
#define WH_NAME_MAX         32

#define WH_ID_BROADCAST     0xFFFFu

/* ===================================================================== */
/* Errors                                                                */
/* ===================================================================== */
typedef enum {
    WH_OK                =  0,
    WH_ERR_ARG           = -1,
    WH_ERR_SHORT         = -2,   /* buffer too short */
    WH_ERR_MAGIC         = -3,
    WH_ERR_VERSION       = -4,
    WH_ERR_RESERVED      = -5,
    WH_ERR_LEN           = -6,   /* payload_len inconsistent with frame */
    WH_ERR_NOT_READY     = -7,   /* no session / not established */
    WH_ERR_TOO_BIG       = -8,   /* message exceeds fragmentation/reasm limits */
    WH_ERR_AUTH          = -9,   /* AEAD tag verification failed */
    WH_ERR_REPLAY        = -10,  /* sequence rejected by replay window */
    WH_ERR_STATE         = -11,  /* pairing state machine misuse */
    WH_ERR_COMMIT        = -12,  /* pairing commitment mismatch (tamper/MITM) */
    WH_ERR_REASM         = -13,  /* reassembly failure */
    WH_ERR_NOMEM         = -14,
    WH_ERR_TRANSPORT     = -15
} wh_err;

/* ===================================================================== */
/* Roles / capabilities                                                  */
/* ===================================================================== */
typedef enum {
    WH_ROLE_GENERIC = 0,
    WH_ROLE_RING    = 1,
    WH_ROLE_WATCH   = 2,
    WH_ROLE_SUBGHZ  = 3
} wh_role;

/* Capability bitmap (advertised in ANNOUNCE / CAPS) */
#define WH_CAP_WIFI        (1u << 0)
#define WH_CAP_BLE         (1u << 1)
#define WH_CAP_IEEE802154  (1u << 2)
#define WH_CAP_LORA        (1u << 3)
#define WH_CAP_GPS         (1u << 4)
#define WH_CAP_SD          (1u << 5)
#define WH_CAP_RTC         (1u << 6)
#define WH_CAP_NFC         (1u << 7)
#define WH_CAP_AUDIO       (1u << 8)
#define WH_CAP_DISPLAY     (1u << 9)
#define WH_CAP_HAPTIC      (1u << 10)
#define WH_CAP_IMU         (1u << 11)
#define WH_CAP_SUBGHZ      (1u << 12)   /* CC1101/EvilCrow-class sub-GHz collector */
#define WH_CAP_SUBGHZ_TX   (1u << 13)   /* node built with TX enabled (agreed additive, redistribute) */
#define WH_CAP_NETSCAN     (1u << 14)   /* ring offers proxied network probing (WH-NETSCAN); bit 13 taken */

/* Convenience: the actual hardware profiles */
#define WH_CAPS_RING   (WH_CAP_WIFI | WH_CAP_BLE | WH_CAP_IEEE802154 | \
                        WH_CAP_DISPLAY | WH_CAP_IMU | WH_CAP_NETSCAN)
#define WH_CAPS_WATCH  (WH_CAP_WIFI | WH_CAP_BLE | WH_CAP_LORA | WH_CAP_GPS | \
                        WH_CAP_SD | WH_CAP_RTC | WH_CAP_NFC | WH_CAP_AUDIO | \
                        WH_CAP_DISPLAY | WH_CAP_HAPTIC | WH_CAP_IMU)
#define WH_CAPS_SUBGHZ (WH_CAP_BLE | WH_CAP_SUBGHZ)

/* ===================================================================== */
/* Frame flags                                                           */
/* ===================================================================== */
#define WH_FLAG_ENC      (1u << 0)  /* payload AEAD-sealed, tag present */
#define WH_FLAG_REQ      (1u << 1)  /* request (expects RESP with same corr) */
#define WH_FLAG_RESP     (1u << 2)  /* response to a REQ */
#define WH_FLAG_EVENT    (1u << 3)  /* fire-and-forget, may be lossy */
#define WH_FLAG_ACK_REQ  (1u << 4)  /* sender requests an ACK frame */
/* bits 5-7 reserved (MUST be 0) */

/* ===================================================================== */
/* Message taxonomy                                                      */
/* ===================================================================== */
typedef enum {
    /* --- session / link (0x00-0x1F) --- */
    WH_MSG_ANNOUNCE     = 0x01, /* broadcast beacon: role, caps, batt, nick */
    WH_MSG_PAIR_REQ     = 0x02, /* I->R: ephemeral PKi */
    WH_MSG_PAIR_RESP    = 0x03, /* R->I: ephemeral PKr + commitment Cr */
    WH_MSG_PAIR_NA      = 0x04, /* I->R: initiator nonce Ni */
    WH_MSG_PAIR_NB      = 0x05, /* R->I: responder nonce Nr (opens commitment) */
    WH_MSG_PING         = 0x06,
    WH_MSG_PONG         = 0x07,
    WH_MSG_CAPS_GET     = 0x08,
    WH_MSG_CAPS         = 0x09,
    WH_MSG_TIME_SYNC    = 0x0A, /* watch->ring: authoritative wall clock */
    WH_MSG_STEALTH_SET  = 0x0B, /* mirror go-dark across devices */
    WH_MSG_NAK          = 0x0C, /* negative ack / error (reason + corr) */
    WH_MSG_ACK          = 0x0D,

    /* --- command / RPC (0x20-0x5F) --- (bodies defined incrementally) */
    WH_MSG_SCAN_REQ     = 0x20,
    WH_MSG_SCAN_RESULT  = 0x21,
    WH_MSG_SNIFF_START  = 0x22, /* drive the Ring's 802.15.4 sniffer remotely */
    WH_MSG_SNIFF_STOP   = 0x23,
    WH_MSG_GPS_GET      = 0x24,
    WH_MSG_GPS_FIX      = 0x25, /* geotag source: lat/lon/alt/fix/sats/time */
    WH_MSG_LOG_WRITE    = 0x26, /* ring->watch: persist a record to SD (idem) */
    WH_MSG_LOG_ACK      = 0x27,
    WH_MSG_LORA_TX      = 0x28, /* ring->watch: transmit over LoRa */
    WH_MSG_LORA_RX_SUB  = 0x29,
    WH_MSG_NFC_REQ      = 0x2A,
    WH_MSG_NFC_RESP     = 0x2B,
    WH_MSG_FILE_LIST    = 0x2C,
    WH_MSG_FILE_GET     = 0x2D,
    WH_MSG_FILE_CHUNK   = 0x2E,
    WH_MSG_CONSOLE      = 0x2F, /* tunnel a serial-console command to the peer */
    WH_MSG_CONSOLE_OUT  = 0x30,

    /* --- clock sync (NTP-style; ring is the client, watch the authoritative server) --- */
    WH_MSG_TIMESYNC_REQ  = 0x31, /* ring->watch: t1_ring; echoed back in RESP */
    WH_MSG_TIMESYNC_RESP = 0x32, /* watch->ring: echo + t2/t3 watch UTC + uncertainty + locked */

    /* HID proxy (drive the peer's paired HID hosts over the authenticated link) */
    WH_MSG_HID_PROXY     = 0x42, /* watch->ring: a HID intent for the ring's active host */
    WH_MSG_HID_PROXY_ACK = 0x43, /* ring->watch: delivery status */

    /* --- telemetry / stream (0x60-0x7F) --- */
    WH_MSG_DEVICE_SEEN  = 0x60, /* live feed of a find (AP/BLE/154) */
    WH_MSG_THREAT_EVENT = 0x61,
    WH_MSG_STATUS       = 0x62,
    WH_MSG_LORA_RX      = 0x63, /* watch->ring: a delivered mesh packet */
    WH_MSG_LAN_NET      = 0x64, /* ring->watch: the ring's current LAN identity (gw MAC / subnet / AP) */
    WH_MSG_LAN_CLIENT   = 0x65, /* ring->watch: one host the ring found on its LAN */
    WH_MSG_NETSCAN_REQ    = 0x66, /* watch->ring: run a network probe (WH-NETSCAN) */
    WH_MSG_NETSCAN_RESULT = 0x67  /* ring->watch: streamed, batched probe results */
} wh_msg_type;

/* NAK reason codes */
typedef enum {
    WH_NAK_UNKNOWN_TYPE = 1,
    WH_NAK_NOT_SUPPORTED= 2,
    WH_NAK_BAD_ARG      = 3,
    WH_NAK_BUSY         = 4,
    WH_NAK_NO_CAP       = 5
} wh_nak_reason;

/* Time source for TIME_SYNC */
typedef enum {
    WH_TIME_NONE = 0,
    WH_TIME_GPS  = 1,
    WH_TIME_RTC  = 2,
    WH_TIME_SNTP = 3
} wh_time_src;

/* DEVICE_SEEN kinds */
typedef enum {
    WH_SEEN_WIFI_AP = 1,
    WH_SEEN_BLE     = 2,
    WH_SEEN_154     = 3,
    WH_SEEN_SUBGHZ  = 4
} wh_seen_kind;

/* ===================================================================== */
/* Wire header                                                           */
/* ===================================================================== */
typedef struct wh_hdr {
    uint8_t  version;     /* == WH_PROTO_VERSION */
    uint8_t  type;        /* wh_msg_type */
    uint8_t  flags;       /* WH_FLAG_* */
    uint16_t src_id;
    uint16_t dst_id;
    uint32_t seq;         /* per-direction frame counter (also AEAD nonce ctr) */
    uint16_t corr;        /* RPC correlation id (0 = none) */
    uint16_t msg_id;      /* logical message id (fragment grouping) */
    uint8_t  frag_index;  /* 0 .. frag_total-1 */
    uint8_t  frag_total;  /* >=1 (1 = unfragmented) */
    uint16_t payload_len; /* this fragment's payload length */
} wh_hdr;

/* Encode header into out (needs WH_HDR_LEN bytes). Returns WH_HDR_LEN or <0. */
int wh_hdr_encode(const wh_hdr *h, uint8_t *out, size_t cap);
/* Decode + validate header from in. Returns WH_OK or <0. */
int wh_hdr_decode(const uint8_t *in, size_t len, wh_hdr *out);

/* ===================================================================== */
/* Message payload structs (session layer + key data records)            */
/* ===================================================================== */
typedef struct wh_announce {
    uint8_t  proto_version;
    uint8_t  role;              /* wh_role */
    uint32_t caps;
    uint8_t  battery_pct;
    uint8_t  flags;             /* bit0: stealth on, bit1: paired */
    uint16_t short_id;
    char     nick[WH_NICK_MAX]; /* NUL-terminated */
} wh_announce;

typedef struct wh_caps {
    uint32_t caps;
    uint8_t  role;
    uint8_t  proto_version;
    uint8_t  battery_pct;
} wh_caps;

typedef struct wh_time_sync {
    int64_t  unix_seconds;
    uint16_t millis;
    uint8_t  source;   /* wh_time_src */
    uint8_t  valid;
} wh_time_sync;

typedef struct wh_gps_fix {
    int32_t  lat_1e7;   /* degrees * 1e7 */
    int32_t  lon_1e7;
    int32_t  alt_mm;
    uint32_t hacc_mm;   /* horizontal accuracy estimate, mm */
    int64_t  unix_seconds;
    uint16_t millis;
    uint16_t speed_mms; /* mm/s */
    uint16_t heading_cdeg; /* centidegrees, RELATIVE (no true north) */
    uint8_t  fix_type;  /* 0 none, 2 2D, 3 3D */
    uint8_t  sats;
} wh_gps_fix;

/* wh_device_seen.flags bits */
#define WH_SEEN_FLAG_THREAT   (1u << 0)
#define WH_SEEN_FLAG_RANDMAC  (1u << 1)

/* --- sub-GHz find payload (carried by DEVICE_SEEN when kind == WH_SEEN_SUBGHZ) ---
 * A kind-gated tail on wh_device_seen, appended AFTER the 21-byte timestamp tail and length-guarded,
 * so an un-updated decoder parses the base find and ignores it. rssi + a coarse freq_mhz already live
 * in the base; this carries the sub-GHz specifics incl. the exact Hz carrier. From the CC1101/EvilCrow
 * node reconciliation (see WH-LINK-RECONCILIATION.md / ANSWERS-for-subghz-agent.md). */
typedef enum {
    WH_SGMOD_2FSK = 0, WH_SGMOD_GFSK = 1, WH_SGMOD_OOK = 2, WH_SGMOD_4FSK = 3, WH_SGMOD_MSK = 4,
} wh_subghz_mod;
typedef enum {
    WH_SGCLASS_UNKNOWN = 0, WH_SGCLASS_PERIODIC_OOK = 1, WH_SGCLASS_VARIABLE_OOK = 2,
    WH_SGCLASS_FSK_PACKET = 3, WH_SGCLASS_CARRIER = 4, WH_SGCLASS_NOISE = 5,
} wh_subghz_class;
#define WH_SG_DEC_MAX    32   /* bounded decoded-payload cap (keeps a find in one 212 B fragment) */
#define WH_SG_TAIL_FIXED 20   /* WIRE fixed portion: 4+1+4+1+1+8+1 (node Â§3c). n_edges/duration_ms are local-only diagnostics, NOT transmitted. */
typedef struct wh_subghz_seen {
    uint32_t freq_hz;             /* exact carrier, e.g. 433920000 */
    uint8_t  mod;                 /* wh_subghz_mod */
    uint32_t drate_bps;           /* estimated symbol rate; 0 = unknown */
    uint8_t  lqi;                 /* 0..127 */
    uint8_t  klass;               /* wh_subghz_class */
    uint16_t n_edges;             /* transitions captured (diagnostic) */
    uint16_t duration_ms;         /* capture window (diagnostic) */
    uint8_t  fp[8];               /* fingerprint: 4x u16 LE dominant pulse widths (us), descending */
    uint8_t  dec_len;             /* 0..WH_SG_DEC_MAX */
    uint8_t  dec[WH_SG_DEC_MAX];  /* bounded decoded bytes (may be empty) */
} wh_subghz_seen;

typedef struct wh_device_seen {
    uint8_t  kind;                 /* wh_seen_kind */
    int8_t   rssi;
    uint8_t  channel;
    uint8_t  flags;                /* bit0 threat, bit1 randomized-mac, ... */
    uint8_t  sec;                  /* wh_sec-compatible: 0 unk 1 open 2 wep 3 wpa 4 wpa2 5 wpa3 6 wpa2/3 7 eap */
    uint16_t freq_mhz;             /* 0 if unknown (disambiguates 2.4/5/6 GHz channels) */
    uint8_t  addr[WH_ADDR_LEN];    /* BSSID / BLE addr / 154 short(2)+pad / sub-GHz fingerprint id */
    char     name[WH_NAME_MAX];    /* SSID / BLE name (NUL-terminated) */
    /* 3B timestamp tail (appended after name on the wire; length-guarded decode) */
    int64_t  utc_ns;               /* capture-time UTC ns; 0 when utc_valid==0 */
    int64_t  mono_ns;              /* ring monotonic ns at capture (always present) */
    uint8_t  utc_valid;            /* 1 = utc_ns anchored to a locked, same-boot sync */
    uint32_t sync_delay_ns;        /* round-trip of the anchoring sync (quality figure) */
    wh_subghz_seen sg;             /* valid iff kind == WH_SEEN_SUBGHZ (kind-gated tail, after the 21 B tail) */
} wh_device_seen;

/* ---- device status (WH_MSG_STATUS) ----
 * One message either device emits to mirror its operational state to the peer,
 * so each side can reflect the other. Timer/stopwatch are reported as ABSOLUTE
 * wall-clock times (unix-ms), never live countdowns: the peer computes
 * remaining/elapsed itself and ticks locally, so a countdown shows second-by-
 * second with zero per-second traffic (the shared TIME_SYNC keeps clocks aligned).
 * Emit on any state change plus a slow heartbeat. */
enum { WH_OPMODE_IDLE = 0, WH_OPMODE_SCANNING = 1, WH_OPMODE_SNIFFING = 2, WH_OPMODE_STEALTH = 3 };
#define WH_COLL_WIFI  0x01u   /* WiFi scan active        */
#define WH_COLL_BLE   0x02u   /* BLE scan active         */
#define WH_COLL_154   0x04u   /* 802.15.4 sniffer active */
enum { WH_TMR_IDLE = 0, WH_TMR_RUNNING = 1, WH_TMR_FIRED = 2 };
enum { WH_SW_IDLE  = 0, WH_SW_RUNNING = 1, WH_SW_PAUSED = 2 };

typedef struct wh_device_status {
    uint8_t  role;            /* WH_ROLE_* */
    uint8_t  mode;            /* WH_MODE_* */
    uint8_t  collectors;      /* WH_COLL_* bits currently live */
    uint8_t  batt_pct;        /* 0..100 */
    uint32_t seen_wifi;       /* running WiFi finds this session */
    uint32_t seen_ble;        /* running BLE finds this session  */
    uint8_t  timer_state;     /* WH_TMR_* */
    int64_t  timer_target_ms; /* absolute unix-ms fire time (valid iff RUNNING) */
    uint8_t  sw_state;        /* WH_SW_* */
    int64_t  sw_start_ms;     /* absolute unix-ms of the current run's start */
    uint32_t sw_accum_ms;     /* ms banked before the current run (pause/resume) */
} wh_device_status;

int wh_enc_status  (const wh_device_status *s, uint8_t *out, size_t cap);
int wh_dec_status  (const uint8_t *in, size_t len, wh_device_status *s);

/* ---- Meshtastic-compatible LoRa bridge --------------------------------
 * The WATCH owns the entire Meshtastic stack: channel keys, node DB, hop-limit
 * and dedup. The RING never holds channel keys or mesh state; it injects into
 * and subscribes from the mesh THROUGH the Watch over the already-authenticated
 * wh-link session, using SEMANTIC fields only (never a pre-built MeshPacket and
 * never raw protobuf). On LORA_TX the Watch composes the MeshPacket: it stamps
 * `from` = its own node num, allocates the packet id, applies the hop limit,
 * encrypts under the channel key, and transmits. This yields a two-hop trust
 * boundary — wh-link (ChaCha20-Poly1305 + SAS) secures Ring<->Watch; Meshtastic
 * channel crypto secures Watch<->mesh — with no key crossing it. "Compatible"
 * is pinned to a meshtastic-lite proto version out of band; wh-link carries
 * semantics so it stays decoupled from Meshtastic protobuf churn. The payload
 * cap below is a shared protocol constant (no runtime negotiation needed). */

#define WH_LORA_MAX_PAYLOAD 233   /* practical Meshtastic Data.payload cap */

/* A few common Meshtastic PortNums (values are Meshtastic's, not ours). */
typedef enum {
    WH_PORT_TEXT      = 1,    /* TEXT_MESSAGE_APP */
    WH_PORT_POSITION  = 3,    /* POSITION_APP   (Watch-originated: it has the GPS) */
    WH_PORT_NODEINFO  = 4,    /* NODEINFO_APP   (Watch-originated) */
    WH_PORT_TELEMETRY = 67,   /* TELEMETRY_APP  (Watch-originated) */
    WH_PORT_PRIVATE   = 256   /* PRIVATE_APP    (Whitehat private port) */
} wh_meshtastic_port;

/* Default injection policy enforced by the Watch: the Ring may only originate
 * TEXT and the Whitehat PRIVATE port; structured ports are Watch-originated so
 * the Ring can't spoof Position/NodeInfo/Telemetry into the mesh. Returns 1 if
 * `portnum` is allowed to the Ring by default. */
static inline int wh_lora_port_allowed_default(uint16_t portnum) {
    return portnum == WH_PORT_TEXT || portnum == WH_PORT_PRIVATE;
}

/* Ring -> Watch: inject a packet into the mesh (semantic fields only). */
typedef struct wh_lora_tx {
    uint32_t dest;         /* destination node num; 0xFFFFFFFF = broadcast */
    uint8_t  channel;      /* index into the Watch's channel table */
    uint8_t  want_ack;     /* request a mesh ACK */
    uint8_t  hop_limit;    /* 0 => Watch default */
    uint16_t portnum;      /* Meshtastic PortNum */
    uint16_t payload_len;  /* bytes in payload (<= WH_LORA_MAX_PAYLOAD) */
    uint8_t  payload[WH_LORA_MAX_PAYLOAD];
} wh_lora_tx;

/* Ring -> Watch: subscribe/unsubscribe to received mesh traffic. If n_ports==0
 * the Watch applies its default policy; otherwise only the listed PortNums on
 * the channels in channel_mask are forwarded to the Ring. */
typedef struct wh_lora_sub {
    uint8_t  enable;       /* 1 subscribe, 0 unsubscribe */
    uint8_t  channel_mask; /* bitmask of channel indices 0..7 to forward */
    uint8_t  n_ports;      /* number of PortNums below (0 => default policy) */
    uint16_t ports[8];
} wh_lora_sub;

/* Watch -> Ring: a received mesh packet, decrypted by the Watch, delivered as
 * semantic fields + opaque port payload. */
typedef struct wh_lora_rx {
    uint32_t from;         /* source node num */
    uint32_t rx_time;      /* unix seconds at the Watch (0 if unknown) */
    uint8_t  channel;      /* channel index it arrived on */
    int16_t  rssi;         /* dBm */
    int16_t  snr_q4;       /* SNR in quarter-dB (snr*4), Meshtastic convention */
    uint16_t portnum;      /* Meshtastic PortNum */
    uint16_t payload_len;
    uint8_t  payload[WH_LORA_MAX_PAYLOAD];
} wh_lora_rx;

/* Ring -> Watch: persist one record to the SD log. wh-link stays schema-neutral
 * and transports the record as opaque bytes; fmt=0 means the bytes are a single
 * wh-log NDJSON line (produced by wh_log.h on the Ring). The Watch MUST validate
 * (parse it) and re-serialize canonically before appending — it never writes
 * attacker-influenced bytes to the durable log verbatim. */
#define WH_LOGREC_MAX 240
enum { WH_LOGREC_FMT_NDJSON = 0 };
typedef struct wh_logrec_msg {
    uint8_t  fmt;          /* WH_LOGREC_FMT_* */
    uint16_t len;          /* bytes in data (<= WH_LOGREC_MAX) */
    uint8_t  data[WH_LOGREC_MAX];
} wh_logrec_msg;

/* Encoders return bytes written (or <0); decoders return WH_OK (or <0). */
int wh_enc_announce (const wh_announce *a, uint8_t *out, size_t cap);
int wh_dec_announce (const uint8_t *in, size_t len, wh_announce *a);
int wh_enc_caps     (const wh_caps *c, uint8_t *out, size_t cap);
int wh_dec_caps     (const uint8_t *in, size_t len, wh_caps *c);
int wh_enc_time_sync(const wh_time_sync *t, uint8_t *out, size_t cap);
int wh_dec_time_sync(const uint8_t *in, size_t len, wh_time_sync *t);

/* NTP-style clock sync (item 3). The ring is the client: it sends REQ with its own
 * monotonic t1, the watch replies RESP echoing t1 and adding its disciplined UTC at
 * receive (t2) and send (t3) plus an uncertainty and a locked bit. The ring runs the
 * min-delay offset math and gates absolute stamping on watch_locked. All little-endian.
 * REQ payload = 9 bytes; RESP payload = 30 bytes. */
typedef struct wh_timesync_req {
    uint8_t seq;
    int64_t t1_ring_ns;
} wh_timesync_req;

typedef struct wh_timesync_resp {
    uint8_t  seq;           /* echoed from REQ */
    int64_t  t1_ring_ns;    /* echoed unchanged */
    int64_t  t2_watch_ns;   /* watch UTC ns at REQ receive */
    int64_t  t3_watch_ns;   /* watch UTC ns at RESP send */
    uint32_t watch_unc_ns;  /* 1-sigma clock uncertainty */
    uint8_t  watch_locked;  /* 1 = GPS/SNTP-disciplined */
} wh_timesync_resp;

int wh_enc_timesync_req (const wh_timesync_req  *t, uint8_t *out, size_t cap);
int wh_dec_timesync_req (const uint8_t *in, size_t len, wh_timesync_req  *t);
int wh_enc_timesync_resp(const wh_timesync_resp *t, uint8_t *out, size_t cap);
int wh_dec_timesync_resp(const uint8_t *in, size_t len, wh_timesync_resp *t);

int wh_enc_gps_fix  (const wh_gps_fix *g, uint8_t *out, size_t cap);
int wh_dec_gps_fix  (const uint8_t *in, size_t len, wh_gps_fix *g);
int wh_enc_device_seen(const wh_device_seen *d, uint8_t *out, size_t cap);
int wh_dec_device_seen(const uint8_t *in, size_t len, wh_device_seen *d);

/* THREAT_EVENT (WH_MSG_THREAT_EVENT = 0x61), item 5. A ring detector alert, sent as a
 * fire-and-forget EVENT. Variable length: id is id_len bytes (MSB-first, like addr).
 * Payload: no-id 14 B, PAN id(2) 16 B, MAC id(6) 20 B. All little-endian scalars. */
typedef struct wh_threat_event {
    uint8_t  klass;         /* 0 FLOCK_OUI,1 CAMERA_OUI,2 WILDCARD_PROBE,3 ZIGBEE_PAN,4 PERMIT_JOIN,5 BLE_NAMED */
    uint8_t  confidence;    /* 0..100 */
    int16_t  rssi;          /* dBm; 0x7FFF = n/a */
    uint8_t  id_kind;       /* 0 none, 1 MAC(6), 2 PAN(2 or 8) */
    uint8_t  id_len;        /* 0 / 6 / 2 / 8 */
    uint8_t  id[8];         /* MSB-first */
    int64_t  utc_ns;        /* 0 when the ring wasn't UTC-synced (anchor with receive-time) */
} wh_threat_event;

int wh_enc_threat_event(const wh_threat_event *t, uint8_t *out, size_t cap);
int wh_dec_threat_event(const uint8_t *in, size_t len, wh_threat_event *t);

/* LAN_NET (WH_MSG_LAN_NET = 0x64). The ring's current network identity so the watch can tell whether
 * the ring's LAN clients share the watch's segment (gw_mac is the fingerprint — stable across
 * duplicate private ranges). Sent on association + ~30 s keepalive. All scalars little-endian; ip and
 * MAC bytes are network/wire order. Layout: gw_mac[6] subnet_base[4] prefix bssid[6] then the ssid as
 * a NUL-terminated string (NOT length-prefixed — the one wh-link string that isn't). */
typedef struct wh_lan_net {
    uint8_t  gw_mac[6];        /* gateway MAC (network id); all-zero = unknown network */
    uint8_t  subnet_base[4];   /* ip & netmask, network order */
    uint8_t  prefix;           /* CIDR /N */
    uint8_t  bssid[6];         /* the AP the ring is associated to */
    char     ssid[33];         /* NUL-terminated, <= 32 chars */
} wh_lan_net;

/* LAN_CLIENT (WH_MSG_LAN_CLIENT = 0x65). One host the ring found on its LAN. 19-byte head +
 * 17-byte timestamp tail (mono_ns/utc_ns/utc_valid — the DEVICE_SEEN tail minus sync_delay_ns);
 * the tail is length-guarded on decode, so a short/legacy sender still parses (missing tail ->
 * utc_valid = 0, and the watch logs receive-time). Fixed 36 B with the tail. */
typedef struct wh_lan_client_msg {
    uint8_t  gw_mac[6];        /* which network (matches the LAN_NET message) */
    uint8_t  ip[4];            /* client IPv4, network order */
    uint8_t  mac[6];           /* client MAC; all-zero if unknown */
    uint8_t  src;              /* WH_LAN_SRC_* bits (ARP / ping / RING) */
    int16_t  rtt_ms;           /* >= 0, or WH_LAN_RTT_NONE / WH_LAN_RTT_TIMEOUT */
    int64_t  mono_ns;          /* ring monotonic ns at capture (always present) */
    int64_t  utc_ns;           /* capture-time UTC ns; 0 when utc_valid == 0 */
    uint8_t  utc_valid;        /* 1 = utc_ns anchored to a locked, same-boot sync */
} wh_lan_client_msg;

int wh_enc_lan_net(const wh_lan_net *n, uint8_t *out, size_t cap);
int wh_dec_lan_net(const uint8_t *in, size_t len, wh_lan_net *n);
int wh_enc_lan_client(const wh_lan_client_msg *c, uint8_t *out, size_t cap);
int wh_dec_lan_client(const uint8_t *in, size_t len, wh_lan_client_msg *c);

/* ---- WH-NETSCAN: proxied network probing (watch drives, ring executes) --------------
 * Gated on WH_CAP_NETSCAN. REQ (0x66) = 8 bytes LE: probe,scope,flags,token,target(u32 net-order);
 * corr = token. RESULT (0x67) = token(u8) kind(u8) count(u8) then `count` records batched into one
 * fragment (<= WH_FRAG_CAP_ENC = 212). HOST records are wh_lan_client_msg verbatim (reuse the
 * LAN_CLIENT decoder); PORT records are port(u16 LE) service(u8) banner_len(u8) [banner]. */
enum { WH_NS_PING = 0, WH_NS_PORTSCAN = 1, WH_NS_DISCOVER = 2, WH_NS_CANCEL = 0xFF };        /* REQ.probe */
enum { WH_NS_SCOPE_QUICK = 0, WH_NS_SCOPE_WELLKNOWN = 1, WH_NS_SCOPE_FULL = 2 };             /* REQ.scope */
enum { WH_NSR_PORT = 0, WH_NSR_HOST = 1, WH_NSR_PROGRESS = 2, WH_NSR_DONE = 3, WH_NSR_ERROR = 4 }; /* RESULT.kind */
enum { WH_NSE_UNREACHABLE = 1, WH_NSE_BUSY = 2, WH_NSE_NO_CAP = 3, WH_NSE_BAD_TARGET = 4, WH_NSE_ABORTED = 5 }; /* ERROR code */
/* Shared service enum for PORT records: ring infers from the port, watch maps to a display name. */
enum {
    WH_SVC_UNKNOWN = 0, WH_SVC_SSH, WH_SVC_HTTP, WH_SVC_HTTPS, WH_SVC_TELNET, WH_SVC_FTP,
    WH_SVC_SMB, WH_SVC_RDP, WH_SVC_DNS, WH_SVC_MYSQL, WH_SVC_RTSP, WH_SVC_VNC, WH_SVC_MQTT,
    WH_SVC_UPNP, WH_SVC_SNMP, WH_SVC_NTP, WH_SVC_SMTP
};

/* HID_PROXY (WH_MSG_HID_PROXY = 0x42). A HID intent the watch sends the ring, which relays
 * it into the ring's own HID senders. Variable by kind; all scalars little-endian.
 *   off 0  u8 kind    (wh_hid_kind)
 *   off 1  u8 target  (0 = active host; 1..N = 1-based host ordinal)
 *   off 2  payload by kind:
 *     kbd(1):      u8 report[8]              mouse(2):   u8 buttons,i8 dx,i8 dy,i8 wheel
 *     consumer(3): u16 usage                 type(4):    u8 len, char text[len]
 *     action(5):   u8 action_id
 * kinds 4/5 (type-string, semantic action) let the watch drive common cases without carrying
 * the report layer; kinds 1-3 are full-fidelity raw reports. */
typedef enum {
    WH_HID_KBD = 1, WH_HID_MOUSE = 2, WH_HID_CONSUMER = 3, WH_HID_TYPE = 4, WH_HID_ACTION = 5
} wh_hid_kind;

#ifndef WH_HID_TEXT_MAX
#define WH_HID_TEXT_MAX 200          /* type-string cap (fits one frame incl. header + tag) */
#endif

typedef struct wh_hid_proxy {
    uint8_t kind;                     /* wh_hid_kind */
    uint8_t target;                   /* 0 active, else 1-based ordinal */
    union {
        uint8_t  kbd[8];
        struct { uint8_t buttons; int8_t dx, dy, wheel; } mouse;
        uint16_t consumer;
        struct { uint8_t len; char text[WH_HID_TEXT_MAX]; } type;
        uint8_t  action_id;
    } u;
} wh_hid_proxy;

/* HID_PROXY_ACK (WH_MSG_HID_PROXY_ACK = 0x43). off 0 u8 status, off 1 u8 target (resolved). */
typedef struct wh_hid_proxy_ack {
    uint8_t status;                   /* 0 ok,1 no host,2 locked,3 stealth,4 bad target,5 HID off */
    uint8_t target;                   /* echo of the host it was delivered to */
} wh_hid_proxy_ack;

int wh_enc_hid_proxy(const wh_hid_proxy *p, uint8_t *out, size_t cap);
int wh_dec_hid_proxy(const uint8_t *in, size_t len, wh_hid_proxy *p);
int wh_enc_hid_proxy_ack(const wh_hid_proxy_ack *a, uint8_t *out, size_t cap);
int wh_dec_hid_proxy_ack(const uint8_t *in, size_t len, wh_hid_proxy_ack *a);

int wh_enc_lora_tx (const wh_lora_tx *t, uint8_t *out, size_t cap);
int wh_dec_lora_tx (const uint8_t *in, size_t len, wh_lora_tx *t);
int wh_enc_lora_sub(const wh_lora_sub *s, uint8_t *out, size_t cap);
int wh_dec_lora_sub(const uint8_t *in, size_t len, wh_lora_sub *s);
int wh_enc_lora_rx (const wh_lora_rx *x, uint8_t *out, size_t cap);
int wh_dec_lora_rx (const uint8_t *in, size_t len, wh_lora_rx *x);
int wh_enc_logrec  (const wh_logrec_msg *m, uint8_t *out, size_t cap);
int wh_dec_logrec  (const uint8_t *in, size_t len, wh_logrec_msg *m);

/* ===================================================================== */
/* Crypto backend interface                                              */
/* ===================================================================== */
/* Contract: 32-byte keys, 12-byte nonces, 16-byte tags. Both
 * ChaCha20-Poly1305 (reference/default) and AES-256-GCM (mbedTLS on device)
 * satisfy this, selected at pairing via the algorithm id (reserved for now). */
typedef struct wh_crypto_if {
    int  (*x25519)(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
    int  (*x25519_base)(uint8_t out[32], const uint8_t scalar[32]);
    int  (*hkdf)(const uint8_t *salt, size_t salt_len,
                 const uint8_t *ikm, size_t ikm_len,
                 const uint8_t *info, size_t info_len,
                 uint8_t *out, size_t out_len);
    void (*sha256)(const void *data, size_t len, uint8_t out[32]);
    void (*hmac_sha256)(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len, uint8_t out[32]);
    void (*aead_seal)(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *pt, size_t pt_len,
                      uint8_t *ct, uint8_t tag[16]);
    int  (*aead_open)(const uint8_t key[32], const uint8_t nonce[12],
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *ct, size_t ct_len,
                      const uint8_t tag[16], uint8_t *pt);
} wh_crypto_if;

/* Reference backend (ChaCha20-Poly1305 + X25519 + HKDF-SHA256). */
const wh_crypto_if *wh_ref_crypto(void);

/* ===================================================================== */
/* Replay window (per direction)                                         */
/* ===================================================================== */
typedef struct wh_replay {
    uint32_t max;       /* highest accepted seq */
    uint64_t bits;      /* window: bit d set => (max-d) received */
    uint8_t  seen;      /* any accepted yet */
} wh_replay;

/* ===================================================================== */
/* Session (post-pairing symmetric state)                                */
/* ===================================================================== */
typedef struct wh_session {
    uint8_t  tx_key[WH_KEY_LEN];
    uint8_t  rx_key[WH_KEY_LEN];
    uint8_t  tx_prefix[WH_NONCE_PREFIX_LEN];
    uint8_t  rx_prefix[WH_NONCE_PREFIX_LEN];
    uint32_t tx_ctr;    /* next send counter (nonce) */
    wh_replay rx_replay;
    uint8_t  established;
} wh_session;

/* ---- session persistence (boot resume) ----
 * Serialize the live session to a WH_SESSION_BLOB_LEN-byte blob so it can be
 * stored in NVS and restored after a power cycle, resuming the encrypted link
 * with no re-pairing. On import the tx counter jumps forward by WH_PERSIST_MARGIN
 * so a nonce used before the restart can never be reused; the caller MUST re-save
 * immediately after a restore so the on-flash counter also advances (every reboot
 * then strictly increases it). Re-save at least every WH_PERSIST_STRIDE sends,
 * which must be < MARGIN. Both ends run byte-identical logic. */
#define WH_PERSIST_MARGIN   4096u
#define WH_PERSIST_STRIDE    512u
#define WH_SESSION_BLOB_LEN    95

/* ===================================================================== */
/* Pairing (X25519 + commitment numeric-comparison SAS)                  */
/* ===================================================================== */
typedef enum {
    WH_PS_IDLE = 0,
    WH_PS_I_SENT_REQ,   /* initiator sent PKi, awaiting RESP */
    WH_PS_R_SENT_RESP,  /* responder sent PKr+Cr, awaiting NA */
    WH_PS_I_SENT_NA,    /* initiator sent Ni, awaiting NB */
    WH_PS_AWAIT_SAS,    /* SAS computed, awaiting user numeric compare */
    WH_PS_ESTABLISHED,
    WH_PS_FAILED
} wh_pair_state;

typedef struct wh_pair {
    wh_pair_state state;
    uint8_t  is_initiator;
    uint8_t  sk[WH_X25519_LEN];   /* our ephemeral private */
    uint8_t  pk_i[WH_X25519_LEN]; /* initiator ephemeral public */
    uint8_t  pk_r[WH_X25519_LEN]; /* responder ephemeral public */
    uint8_t  n_i[WH_PAIR_NONCE_LEN];
    uint8_t  n_r[WH_PAIR_NONCE_LEN];
    uint8_t  commit[WH_COMMIT_LEN]; /* received Cr (initiator) */
    uint8_t  shared[WH_X25519_LEN];
    char     sas[WH_SAS_DIGITS + 1]; /* "NNNNNN" */
} wh_pair;

/* ===================================================================== */
/* Delivered message (to user callback)                                  */
/* ===================================================================== */
typedef struct wh_msg {
    uint8_t        type;
    uint8_t        flags;
    uint16_t       corr;
    uint16_t       src_id;
    const uint8_t *payload;
    size_t         len;
} wh_msg;

/* ===================================================================== */
/* Context + configuration                                               */
/* ===================================================================== */
struct wh_ctx;

typedef struct wh_config {
    /* identity / addressing */
    uint16_t local_id;
    uint16_t peer_id;          /* WH_ID_BROADCAST until known */
    uint8_t  role;             /* wh_role */
    uint32_t caps;
    char     nick[WH_NICK_MAX];

    /* backends / platform hooks */
    const wh_crypto_if *crypto;                    /* NULL -> wh_ref_crypto() */
    void  (*rng)(void *user, uint8_t *buf, size_t len);
    uint32_t (*now_ms)(void *user);
    int   (*transport_send)(void *user, const uint8_t *frame, size_t len);

    /* event callbacks */
    void  (*on_message)(void *user, const wh_msg *m);         /* non-handshake */
    void  (*on_sas)(void *user, const char *digits);          /* show + compare */
    void  (*on_paired)(void *user, int ok);                   /* handshake done */

    void  *user;               /* opaque, passed to all callbacks/hooks */
} wh_config;

typedef struct wh_ctx {
    wh_config cfg;
    const wh_crypto_if *crypto;
    wh_session sess;
    wh_pair    pair;
    uint16_t   next_msg_id;

    /* inbound reassembly (single in-flight message) */
    struct {
        uint8_t  active;
        uint16_t src_id;
        uint16_t msg_id;
        uint8_t  frag_total;
        uint16_t cap;         /* per-fragment cap in force (ENC vs plain) */
        uint16_t total_len;   /* known once final fragment arrives */
        uint8_t  have_final;
        uint32_t recv_mask[8];/* 256-bit bitmap */
        uint16_t recv_count;
        uint32_t start_ms;
        uint8_t  buf[WH_REASM_MAX];
    } reasm;
} wh_ctx;

/* Initialize a context. Copies cfg. Returns WH_OK or <0. */
int  wh_ctx_init(wh_ctx *ctx, const wh_config *cfg);

/* Set/clear the peer short id (e.g. after ANNOUNCE discovery). */
void wh_ctx_set_peer(wh_ctx *ctx, uint16_t peer_id);

/* True once a session is established (encrypted traffic allowed). */
int  wh_is_established(const wh_ctx *ctx);

/* Send a message. Handles encryption (if established + non-handshake),
 * fragmentation, and transport. `flags` may add REQ/RESP/EVENT/ACK_REQ; ENC is
 * managed automatically. Returns WH_OK or <0. */
int  wh_send(wh_ctx *ctx, uint8_t type, uint8_t flags, uint16_t corr,
             const uint8_t *payload, size_t len);

/* Feed one received raw frame (from ESP-NOW / BLE). Decrypts, replay-checks,
 * reassembles, and dispatches (handshake internally; else on_message).
 * Returns WH_OK if consumed, or <0 on a rejected/invalid frame (dropped). */
int  wh_on_frame(wh_ctx *ctx, const uint8_t *frame, size_t len);

/* Broadcast an ANNOUNCE beacon. */
int  wh_announce_send(wh_ctx *ctx, uint8_t battery_pct, uint8_t flags);

/* --- pairing --- */
/* Begin pairing as initiator (emits PAIR_REQ). peer_id must be set. */
int  wh_pair_start(wh_ctx *ctx);
/* After on_sas fires and the user compares digits on both devices, confirm. */
int  wh_pair_confirm(wh_ctx *ctx, int matched);
/* Current SAS digits (valid once state == AWAIT_SAS). */
const char *wh_pair_sas(const wh_ctx *ctx);
/* Reset pairing/session (forget). */
void wh_unpair(wh_ctx *ctx);

/* Session persistence: export the live (established) session into buf
 * (cap >= WH_SESSION_BLOB_LEN) for NVS storage -> bytes written, or <0 if not
 * established / buffer too short. import validates magic+length, restores the
 * session, and jumps tx_ctr by WH_PERSIST_MARGIN -> WH_OK, or <0. A blob whose
 * counter is within 2*MARGIN of the u32 max is refused (WH_ERR_STATE): re-pair. */
int  wh_session_export(const wh_ctx *ctx, uint8_t *buf, size_t cap);
int  wh_session_import(wh_ctx *ctx, const uint8_t *buf, size_t len);

/* Helpers */
const char *wh_strerr(int err);
int  wh_type_is_handshake(uint8_t type);

#ifdef __cplusplus
}
#endif

/* ===================================================================== */
/* ===================================================================== */
/* Implementation                                                        */
/* ===================================================================== */
/* ===================================================================== */
#ifdef WH_LINK_IMPLEMENTATION

#include <string.h>

#define WH_LINK_REF_CRYPTO_IMPLEMENTATION
#include "wh_link_ref_crypto.h"

/* ---- little-endian byte helpers (bounds-checked cursor) --------------- */
typedef struct wh__wr { uint8_t *p; size_t cap; size_t off; int err; } wh__wr;
typedef struct wh__rd { const uint8_t *p; size_t len; size_t off; int err; } wh__rd;

static void wh__wr_init(wh__wr *w, uint8_t *buf, size_t cap) { w->p = buf; w->cap = cap; w->off = 0; w->err = 0; }
static void wh__wr_u8(wh__wr *w, uint8_t v) {
    if (w->off + 1 > w->cap) { w->err = 1; return; }
    w->p[w->off++] = v;
}
static void wh__wr_u16(wh__wr *w, uint16_t v) { wh__wr_u8(w, (uint8_t)v); wh__wr_u8(w, (uint8_t)(v >> 8)); }
static void wh__wr_u32(wh__wr *w, uint32_t v) { for (int i = 0; i < 4; i++) wh__wr_u8(w, (uint8_t)(v >> (8 * i))); }
static void wh__wr_u64(wh__wr *w, uint64_t v) { for (int i = 0; i < 8; i++) wh__wr_u8(w, (uint8_t)(v >> (8 * i))); }
static void wh__wr_bytes(wh__wr *w, const uint8_t *b, size_t n) {
    if (w->off + n > w->cap) { w->err = 1; return; }
    memcpy(w->p + w->off, b, n); w->off += n;
}

static void wh__rd_init(wh__rd *r, const uint8_t *buf, size_t len) { r->p = buf; r->len = len; r->off = 0; r->err = 0; }
static uint8_t wh__rd_u8(wh__rd *r) {
    if (r->off + 1 > r->len) { r->err = 1; return 0; }
    return r->p[r->off++];
}
static uint16_t wh__rd_u16(wh__rd *r) { uint16_t a = wh__rd_u8(r); uint16_t b = wh__rd_u8(r); return (uint16_t)(a | (b << 8)); }
static uint32_t wh__rd_u32(wh__rd *r) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)wh__rd_u8(r) << (8 * i); return v; }
static uint64_t wh__rd_u64(wh__rd *r) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)wh__rd_u8(r) << (8 * i); return v; }
static void wh__rd_bytes(wh__rd *r, uint8_t *b, size_t n) {
    if (r->off + n > r->len) { r->err = 1; memset(b, 0, n); return; }
    memcpy(b, r->p + r->off, n); r->off += n;
}

/* ---- header encode/decode --------------------------------------------- */
int wh_hdr_encode(const wh_hdr *h, uint8_t *out, size_t cap)
{
    if (!h || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, WH_MAGIC0);
    wh__wr_u8(&w, WH_MAGIC1);
    wh__wr_u8(&w, h->version);
    wh__wr_u8(&w, h->type);
    wh__wr_u8(&w, h->flags);
    wh__wr_u8(&w, 0);              /* reserved */
    wh__wr_u16(&w, h->src_id);
    wh__wr_u16(&w, h->dst_id);
    wh__wr_u32(&w, h->seq);
    wh__wr_u16(&w, h->corr);
    wh__wr_u16(&w, h->msg_id);
    wh__wr_u8(&w, h->frag_index);
    wh__wr_u8(&w, h->frag_total);
    wh__wr_u16(&w, h->payload_len);
    if (w.err) return WH_ERR_SHORT;
    return (int)w.off; /* == WH_HDR_LEN */
}

int wh_hdr_decode(const uint8_t *in, size_t len, wh_hdr *out)
{
    if (!in || !out) return WH_ERR_ARG;
    if (len < WH_HDR_LEN) return WH_ERR_SHORT;
    wh__rd r; wh__rd_init(&r, in, len);
    uint8_t m0 = wh__rd_u8(&r), m1 = wh__rd_u8(&r);
    if (m0 != WH_MAGIC0 || m1 != WH_MAGIC1) return WH_ERR_MAGIC;
    out->version = wh__rd_u8(&r);
    if (out->version != WH_PROTO_VERSION) return WH_ERR_VERSION;
    out->type       = wh__rd_u8(&r);
    out->flags      = wh__rd_u8(&r);
    uint8_t rsv     = wh__rd_u8(&r);
    if (rsv != 0) return WH_ERR_RESERVED;
    out->src_id     = wh__rd_u16(&r);
    out->dst_id     = wh__rd_u16(&r);
    out->seq        = wh__rd_u32(&r);
    out->corr       = wh__rd_u16(&r);
    out->msg_id     = wh__rd_u16(&r);
    out->frag_index = wh__rd_u8(&r);
    out->frag_total = wh__rd_u8(&r);
    out->payload_len= wh__rd_u16(&r);
    if (r.err) return WH_ERR_SHORT;
    if (out->frag_total == 0) return WH_ERR_LEN;
    if (out->frag_index >= out->frag_total) return WH_ERR_LEN;
    return WH_OK;
}

/* ---- message encoders / decoders -------------------------------------- */
static void wh__put_cstr(wh__wr *w, const char *s, size_t maxlen)
{
    size_t n = 0;
    while (n < maxlen && s[n] != '\0') n++;
    wh__wr_u8(w, (uint8_t)n);
    wh__wr_bytes(w, (const uint8_t *)s, n);
}
static void wh__get_cstr(wh__rd *r, char *out, size_t outcap)
{
    uint8_t n = wh__rd_u8(r);
    size_t copy = n;
    if (copy > outcap - 1) copy = outcap - 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = wh__rd_u8(r);
        if (i < copy) out[i] = (char)c;
    }
    out[copy] = '\0';
}

int wh_enc_announce(const wh_announce *a, uint8_t *out, size_t cap)
{
    if (!a || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, a->proto_version);
    wh__wr_u8(&w, a->role);
    wh__wr_u32(&w, a->caps);
    wh__wr_u8(&w, a->battery_pct);
    wh__wr_u8(&w, a->flags);
    wh__wr_u16(&w, a->short_id);
    wh__put_cstr(&w, a->nick, WH_NICK_MAX - 1);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_announce(const uint8_t *in, size_t len, wh_announce *a)
{
    if (!in || !a) return WH_ERR_ARG;
    memset(a, 0, sizeof(*a));
    wh__rd r; wh__rd_init(&r, in, len);
    a->proto_version = wh__rd_u8(&r);
    a->role          = wh__rd_u8(&r);
    a->caps          = wh__rd_u32(&r);
    a->battery_pct   = wh__rd_u8(&r);
    a->flags         = wh__rd_u8(&r);
    a->short_id      = wh__rd_u16(&r);
    wh__get_cstr(&r, a->nick, WH_NICK_MAX);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_caps(const wh_caps *c, uint8_t *out, size_t cap)
{
    if (!c || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u32(&w, c->caps);
    wh__wr_u8(&w, c->role);
    wh__wr_u8(&w, c->proto_version);
    wh__wr_u8(&w, c->battery_pct);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_caps(const uint8_t *in, size_t len, wh_caps *c)
{
    if (!in || !c) return WH_ERR_ARG;
    memset(c, 0, sizeof(*c));
    wh__rd r; wh__rd_init(&r, in, len);
    c->caps          = wh__rd_u32(&r);
    c->role          = wh__rd_u8(&r);
    c->proto_version = wh__rd_u8(&r);
    c->battery_pct   = wh__rd_u8(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_time_sync(const wh_time_sync *t, uint8_t *out, size_t cap)
{
    if (!t || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u64(&w, (uint64_t)t->unix_seconds);
    wh__wr_u16(&w, t->millis);
    wh__wr_u8(&w, t->source);
    wh__wr_u8(&w, t->valid);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_time_sync(const uint8_t *in, size_t len, wh_time_sync *t)
{
    if (!in || !t) return WH_ERR_ARG;
    memset(t, 0, sizeof(*t));
    wh__rd r; wh__rd_init(&r, in, len);
    t->unix_seconds = (int64_t)wh__rd_u64(&r);
    t->millis       = wh__rd_u16(&r);
    t->source       = wh__rd_u8(&r);
    t->valid        = wh__rd_u8(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}
int wh_enc_timesync_req(const wh_timesync_req *t, uint8_t *out, size_t cap)
{
    if (!t || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8 (&w, t->seq);                         /* off 0  */
    wh__wr_u64(&w, (uint64_t)t->t1_ring_ns);        /* off 1  */
    return w.err ? WH_ERR_SHORT : (int)w.off;       /* 9 bytes */
}
int wh_dec_timesync_req(const uint8_t *in, size_t len, wh_timesync_req *t)
{
    if (!in || !t) return WH_ERR_ARG;
    memset(t, 0, sizeof(*t));
    wh__rd r; wh__rd_init(&r, in, len);
    t->seq        = wh__rd_u8(&r);
    t->t1_ring_ns = (int64_t)wh__rd_u64(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}
int wh_enc_timesync_resp(const wh_timesync_resp *t, uint8_t *out, size_t cap)
{
    if (!t || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8 (&w, t->seq);                         /* off 0  */
    wh__wr_u64(&w, (uint64_t)t->t1_ring_ns);        /* off 1  */
    wh__wr_u64(&w, (uint64_t)t->t2_watch_ns);       /* off 9  */
    wh__wr_u64(&w, (uint64_t)t->t3_watch_ns);       /* off 17 */
    wh__wr_u32(&w, t->watch_unc_ns);                /* off 25 */
    wh__wr_u8 (&w, t->watch_locked);                /* off 29 */
    return w.err ? WH_ERR_SHORT : (int)w.off;       /* 30 bytes */
}
int wh_dec_timesync_resp(const uint8_t *in, size_t len, wh_timesync_resp *t)
{
    if (!in || !t) return WH_ERR_ARG;
    memset(t, 0, sizeof(*t));
    wh__rd r; wh__rd_init(&r, in, len);
    t->seq          = wh__rd_u8(&r);
    t->t1_ring_ns   = (int64_t)wh__rd_u64(&r);
    t->t2_watch_ns  = (int64_t)wh__rd_u64(&r);
    t->t3_watch_ns  = (int64_t)wh__rd_u64(&r);
    t->watch_unc_ns = wh__rd_u32(&r);
    t->watch_locked = wh__rd_u8(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_gps_fix(const wh_gps_fix *g, uint8_t *out, size_t cap)
{
    if (!g || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u32(&w, (uint32_t)g->lat_1e7);
    wh__wr_u32(&w, (uint32_t)g->lon_1e7);
    wh__wr_u32(&w, (uint32_t)g->alt_mm);
    wh__wr_u32(&w, g->hacc_mm);
    wh__wr_u64(&w, (uint64_t)g->unix_seconds);
    wh__wr_u16(&w, g->millis);
    wh__wr_u16(&w, g->speed_mms);
    wh__wr_u16(&w, g->heading_cdeg);
    wh__wr_u8(&w, g->fix_type);
    wh__wr_u8(&w, g->sats);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_gps_fix(const uint8_t *in, size_t len, wh_gps_fix *g)
{
    if (!in || !g) return WH_ERR_ARG;
    memset(g, 0, sizeof(*g));
    wh__rd r; wh__rd_init(&r, in, len);
    g->lat_1e7      = (int32_t)wh__rd_u32(&r);
    g->lon_1e7      = (int32_t)wh__rd_u32(&r);
    g->alt_mm       = (int32_t)wh__rd_u32(&r);
    g->hacc_mm      = wh__rd_u32(&r);
    g->unix_seconds = (int64_t)wh__rd_u64(&r);
    g->millis       = wh__rd_u16(&r);
    g->speed_mms    = wh__rd_u16(&r);
    g->heading_cdeg = wh__rd_u16(&r);
    g->fix_type     = wh__rd_u8(&r);
    g->sats         = wh__rd_u8(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_device_seen(const wh_device_seen *d, uint8_t *out, size_t cap)
{
    if (!d || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, d->kind);
    wh__wr_u8(&w, (uint8_t)d->rssi);
    wh__wr_u8(&w, d->channel);
    wh__wr_u8(&w, d->flags);
    wh__wr_u8(&w, d->sec);
    wh__wr_u16(&w, d->freq_mhz);
    wh__wr_bytes(&w, d->addr, WH_ADDR_LEN);
    wh__put_cstr(&w, d->name, WH_NAME_MAX - 1);
    wh__wr_u64(&w, (uint64_t)d->utc_ns);        /* 3B tail (21 B) */
    wh__wr_u64(&w, (uint64_t)d->mono_ns);
    wh__wr_u8 (&w, d->utc_valid);
    wh__wr_u32(&w, d->sync_delay_ns);
    if (d->kind == WH_SEEN_SUBGHZ) {                 /* kind-gated sub-GHz tail, after the 21 B tail */
        wh__wr_u32(&w, d->sg.freq_hz);
        wh__wr_u8 (&w, d->sg.mod);
        wh__wr_u32(&w, d->sg.drate_bps);
        wh__wr_u8 (&w, d->sg.lqi);
        wh__wr_u8 (&w, d->sg.klass);
        wh__wr_bytes(&w, d->sg.fp, 8);   /* n_edges/duration_ms are local-only, not on the wire */
        wh__wr_u8 (&w, d->sg.dec_len);
        if (d->sg.dec_len) wh__wr_bytes(&w, d->sg.dec, d->sg.dec_len);
    }
    return w.err ? WH_ERR_SHORT : (int)w.off;
}

int wh_enc_status(const wh_device_status *s, uint8_t *out, size_t cap)
{
    if (!s || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, s->role);
    wh__wr_u8(&w, s->mode);
    wh__wr_u8(&w, s->collectors);
    wh__wr_u8(&w, s->batt_pct);
    wh__wr_u32(&w, s->seen_wifi);
    wh__wr_u32(&w, s->seen_ble);
    wh__wr_u8(&w, s->timer_state);
    wh__wr_u64(&w, (uint64_t)s->timer_target_ms);
    wh__wr_u8(&w, s->sw_state);
    wh__wr_u64(&w, (uint64_t)s->sw_start_ms);
    wh__wr_u32(&w, s->sw_accum_ms);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}

int wh_dec_status(const uint8_t *in, size_t len, wh_device_status *s)
{
    if (!in || !s) return WH_ERR_ARG;
    wh__rd r; wh__rd_init(&r, in, len);
    s->role            = wh__rd_u8(&r);
    s->mode            = wh__rd_u8(&r);
    s->collectors      = wh__rd_u8(&r);
    s->batt_pct        = wh__rd_u8(&r);
    s->seen_wifi       = wh__rd_u32(&r);
    s->seen_ble        = wh__rd_u32(&r);
    s->timer_state     = wh__rd_u8(&r);
    s->timer_target_ms = (int64_t)wh__rd_u64(&r);
    s->sw_state        = wh__rd_u8(&r);
    s->sw_start_ms     = (int64_t)wh__rd_u64(&r);
    s->sw_accum_ms     = wh__rd_u32(&r);
    return r.err ? WH_ERR_SHORT : (int)r.off;
}
int wh_dec_device_seen(const uint8_t *in, size_t len, wh_device_seen *d)
{
    if (!in || !d) return WH_ERR_ARG;
    memset(d, 0, sizeof(*d));
    wh__rd r; wh__rd_init(&r, in, len);
    d->kind    = wh__rd_u8(&r);
    d->rssi    = (int8_t)wh__rd_u8(&r);
    d->channel = wh__rd_u8(&r);
    d->flags   = wh__rd_u8(&r);
    d->sec     = wh__rd_u8(&r);
    d->freq_mhz= wh__rd_u16(&r);
    wh__rd_bytes(&r, d->addr, WH_ADDR_LEN);
    wh__get_cstr(&r, d->name, WH_NAME_MAX);
    /* optional 3B timestamp tail (21 B). Absent on a legacy/short frame -> utc_valid stays
     * 0 (from memset) and the base still parses. Guard so we never over-read. */
    if (!r.err && r.off + 21 <= r.len) {
        d->utc_ns        = (int64_t)wh__rd_u64(&r);
        d->mono_ns       = (int64_t)wh__rd_u64(&r);
        d->utc_valid     = wh__rd_u8(&r);
        d->sync_delay_ns = wh__rd_u32(&r);
    }
    if (d->kind == WH_SEEN_SUBGHZ && !r.err && r.off + WH_SG_TAIL_FIXED <= r.len) {
        d->sg.freq_hz     = wh__rd_u32(&r);
        d->sg.mod         = wh__rd_u8(&r);
        d->sg.drate_bps   = wh__rd_u32(&r);
        d->sg.lqi         = wh__rd_u8(&r);
        d->sg.klass       = wh__rd_u8(&r);
        wh__rd_bytes(&r, d->sg.fp, 8);   /* n_edges/duration_ms not on the wire; left 0 for remote finds */
        d->sg.dec_len     = wh__rd_u8(&r);
        if (d->sg.dec_len > WH_SG_DEC_MAX) d->sg.dec_len = WH_SG_DEC_MAX;
        if (!r.err && r.off + d->sg.dec_len <= r.len) wh__rd_bytes(&r, d->sg.dec, d->sg.dec_len);
    }
    return r.err ? WH_ERR_SHORT : WH_OK;
}
int wh_enc_threat_event(const wh_threat_event *t, uint8_t *out, size_t cap)
{
    if (!t || !out) return WH_ERR_ARG;
    if (t->id_len > 8) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8 (&w, t->klass);                    /* off 0 */
    wh__wr_u8 (&w, t->confidence);               /* off 1 */
    wh__wr_u16(&w, (uint16_t)t->rssi);           /* off 2 (i16 LE) */
    wh__wr_u8 (&w, t->id_kind);                  /* off 4 */
    wh__wr_u8 (&w, t->id_len);                   /* off 5 */
    wh__wr_bytes(&w, t->id, t->id_len);          /* off 6, id_len bytes */
    wh__wr_u64(&w, (uint64_t)t->utc_ns);         /* off 6+id_len */
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_threat_event(const uint8_t *in, size_t len, wh_threat_event *t)
{
    if (!in || !t) return WH_ERR_ARG;
    memset(t, 0, sizeof(*t));
    wh__rd r; wh__rd_init(&r, in, len);
    t->klass      = wh__rd_u8(&r);
    t->confidence = wh__rd_u8(&r);
    t->rssi       = (int16_t)wh__rd_u16(&r);
    t->id_kind    = wh__rd_u8(&r);
    t->id_len     = wh__rd_u8(&r);
    if (t->id_len > 8) return WH_ERR_ARG;        /* guard t->id[8] before the read */
    wh__rd_bytes(&r, t->id, t->id_len);
    t->utc_ns     = (int64_t)wh__rd_u64(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_lan_net(const wh_lan_net *n, uint8_t *out, size_t cap)
{
    if (!n || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_bytes(&w, n->gw_mac, 6);              /* off 0 */
    wh__wr_bytes(&w, n->subnet_base, 4);         /* off 6 */
    wh__wr_u8   (&w, n->prefix);                 /* off 10 */
    wh__wr_bytes(&w, n->bssid, 6);               /* off 11 */
    /* ssid: NUL-terminated (not the length-prefixed wh__put_cstr convention) */
    size_t sn = 0; while (sn < sizeof n->ssid - 1 && n->ssid[sn]) sn++;
    wh__wr_bytes(&w, (const uint8_t *)n->ssid, sn);
    wh__wr_u8   (&w, 0);                          /* NUL terminator */
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_lan_net(const uint8_t *in, size_t len, wh_lan_net *n)
{
    if (!in || !n) return WH_ERR_ARG;
    memset(n, 0, sizeof(*n));
    wh__rd r; wh__rd_init(&r, in, len);
    wh__rd_bytes(&r, n->gw_mac, 6);
    wh__rd_bytes(&r, n->subnet_base, 4);
    n->prefix = wh__rd_u8(&r);
    wh__rd_bytes(&r, n->bssid, 6);
    if (r.err) return WH_ERR_SHORT;              /* the fixed 17-byte head must be present */
    size_t si = 0;                               /* ssid: read to NUL or end — terminator optional */
    for (;;) {
        uint8_t c = wh__rd_u8(&r);
        if (r.err || c == 0) break;
        if (si < sizeof n->ssid - 1) n->ssid[si++] = (char)c;
    }
    n->ssid[si] = '\0';
    return WH_OK;                                /* head decoded; ssid may be empty or unterminated */
}

int wh_enc_lan_client(const wh_lan_client_msg *c, uint8_t *out, size_t cap)
{
    if (!c || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_bytes(&w, c->gw_mac, 6);              /* off 0 */
    wh__wr_bytes(&w, c->ip, 4);                  /* off 6 */
    wh__wr_bytes(&w, c->mac, 6);                 /* off 10 */
    wh__wr_u8   (&w, c->src);                    /* off 16 */
    wh__wr_u16  (&w, (uint16_t)c->rtt_ms);       /* off 17 (i16 LE) */
    wh__wr_u64  (&w, (uint64_t)c->mono_ns);      /* off 19: 17-byte tail, mono first */
    wh__wr_u64  (&w, (uint64_t)c->utc_ns);       /* off 27 */
    wh__wr_u8   (&w, c->utc_valid);              /* off 35 */
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_lan_client(const uint8_t *in, size_t len, wh_lan_client_msg *c)
{
    if (!in || !c) return WH_ERR_ARG;
    memset(c, 0, sizeof(*c));
    wh__rd r; wh__rd_init(&r, in, len);
    wh__rd_bytes(&r, c->gw_mac, 6);
    wh__rd_bytes(&r, c->ip, 4);
    wh__rd_bytes(&r, c->mac, 6);
    c->src    = wh__rd_u8(&r);
    c->rtt_ms = (int16_t)wh__rd_u16(&r);
    /* optional 17-byte timestamp tail; a short sender leaves utc_valid = 0 (receive-time logging) */
    if (!r.err && r.off + 17 <= r.len) {
        c->mono_ns   = (int64_t)wh__rd_u64(&r);
        c->utc_ns    = (int64_t)wh__rd_u64(&r);
        c->utc_valid = wh__rd_u8(&r);
    }
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_hid_proxy(const wh_hid_proxy *p, uint8_t *out, size_t cap)
{
    if (!p || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, p->kind);
    wh__wr_u8(&w, p->target);
    switch (p->kind) {
    case WH_HID_KBD:      wh__wr_bytes(&w, p->u.kbd, 8); break;
    case WH_HID_MOUSE:    wh__wr_u8(&w, p->u.mouse.buttons);
                          wh__wr_u8(&w, (uint8_t)p->u.mouse.dx);
                          wh__wr_u8(&w, (uint8_t)p->u.mouse.dy);
                          wh__wr_u8(&w, (uint8_t)p->u.mouse.wheel); break;
    case WH_HID_CONSUMER: wh__wr_u16(&w, p->u.consumer); break;
    case WH_HID_TYPE:     { uint8_t n = p->u.type.len > WH_HID_TEXT_MAX ? WH_HID_TEXT_MAX : p->u.type.len;
                            wh__wr_u8(&w, n);
                            wh__wr_bytes(&w, (const uint8_t *)p->u.type.text, n); } break;
    case WH_HID_ACTION:   wh__wr_u8(&w, p->u.action_id); break;
    default:              return WH_ERR_ARG;
    }
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_hid_proxy(const uint8_t *in, size_t len, wh_hid_proxy *p)
{
    if (!in || !p) return WH_ERR_ARG;
    memset(p, 0, sizeof(*p));
    wh__rd r; wh__rd_init(&r, in, len);
    p->kind   = wh__rd_u8(&r);
    p->target = wh__rd_u8(&r);
    switch (p->kind) {
    case WH_HID_KBD:      wh__rd_bytes(&r, p->u.kbd, 8); break;
    case WH_HID_MOUSE:    p->u.mouse.buttons = wh__rd_u8(&r);
                          p->u.mouse.dx = (int8_t)wh__rd_u8(&r);
                          p->u.mouse.dy = (int8_t)wh__rd_u8(&r);
                          p->u.mouse.wheel = (int8_t)wh__rd_u8(&r); break;
    case WH_HID_CONSUMER: p->u.consumer = wh__rd_u16(&r); break;
    case WH_HID_TYPE:     { uint8_t n = wh__rd_u8(&r);
                            if (n > WH_HID_TEXT_MAX) return WH_ERR_ARG;   /* guard text[] */
                            wh__rd_bytes(&r, (uint8_t *)p->u.type.text, n);
                            p->u.type.len = n; } break;
    case WH_HID_ACTION:   p->u.action_id = wh__rd_u8(&r); break;
    default:              return WH_ERR_ARG;
    }
    return r.err ? WH_ERR_SHORT : WH_OK;
}
int wh_enc_hid_proxy_ack(const wh_hid_proxy_ack *a, uint8_t *out, size_t cap)
{
    if (!a || !out) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, a->status);
    wh__wr_u8(&w, a->target);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_hid_proxy_ack(const uint8_t *in, size_t len, wh_hid_proxy_ack *a)
{
    if (!in || !a) return WH_ERR_ARG;
    memset(a, 0, sizeof(*a));
    wh__rd r; wh__rd_init(&r, in, len);
    a->status = wh__rd_u8(&r);
    a->target = wh__rd_u8(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

/* ---- Meshtastic LoRa bridge + log-record transport codecs -------------- */
int wh_enc_lora_tx(const wh_lora_tx *t, uint8_t *out, size_t cap)
{
    if (!t || !out) return WH_ERR_ARG;
    if (t->payload_len > WH_LORA_MAX_PAYLOAD) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u32(&w, t->dest);
    wh__wr_u8(&w, t->channel);
    wh__wr_u8(&w, t->want_ack);
    wh__wr_u8(&w, t->hop_limit);
    wh__wr_u16(&w, t->portnum);
    wh__wr_u16(&w, t->payload_len);
    wh__wr_bytes(&w, t->payload, t->payload_len);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_lora_tx(const uint8_t *in, size_t len, wh_lora_tx *t)
{
    if (!in || !t) return WH_ERR_ARG;
    memset(t, 0, sizeof(*t));
    wh__rd r; wh__rd_init(&r, in, len);
    t->dest        = wh__rd_u32(&r);
    t->channel     = wh__rd_u8(&r);
    t->want_ack    = wh__rd_u8(&r);
    t->hop_limit   = wh__rd_u8(&r);
    t->portnum     = wh__rd_u16(&r);
    t->payload_len = wh__rd_u16(&r);
    if (r.err || t->payload_len > WH_LORA_MAX_PAYLOAD) return WH_ERR_SHORT;
    wh__rd_bytes(&r, t->payload, t->payload_len);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_lora_sub(const wh_lora_sub *s, uint8_t *out, size_t cap)
{
    if (!s || !out) return WH_ERR_ARG;
    if (s->n_ports > 8) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, s->enable);
    wh__wr_u8(&w, s->channel_mask);
    wh__wr_u8(&w, s->n_ports);
    for (uint8_t i = 0; i < s->n_ports; i++) wh__wr_u16(&w, s->ports[i]);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_lora_sub(const uint8_t *in, size_t len, wh_lora_sub *s)
{
    if (!in || !s) return WH_ERR_ARG;
    memset(s, 0, sizeof(*s));
    wh__rd r; wh__rd_init(&r, in, len);
    s->enable       = wh__rd_u8(&r);
    s->channel_mask = wh__rd_u8(&r);
    s->n_ports      = wh__rd_u8(&r);
    if (r.err || s->n_ports > 8) return WH_ERR_SHORT;
    for (uint8_t i = 0; i < s->n_ports; i++) s->ports[i] = wh__rd_u16(&r);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_lora_rx(const wh_lora_rx *x, uint8_t *out, size_t cap)
{
    if (!x || !out) return WH_ERR_ARG;
    if (x->payload_len > WH_LORA_MAX_PAYLOAD) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u32(&w, x->from);
    wh__wr_u32(&w, x->rx_time);
    wh__wr_u8(&w, x->channel);
    wh__wr_u16(&w, (uint16_t)x->rssi);
    wh__wr_u16(&w, (uint16_t)x->snr_q4);
    wh__wr_u16(&w, x->portnum);
    wh__wr_u16(&w, x->payload_len);
    wh__wr_bytes(&w, x->payload, x->payload_len);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_lora_rx(const uint8_t *in, size_t len, wh_lora_rx *x)
{
    if (!in || !x) return WH_ERR_ARG;
    memset(x, 0, sizeof(*x));
    wh__rd r; wh__rd_init(&r, in, len);
    x->from        = wh__rd_u32(&r);
    x->rx_time     = wh__rd_u32(&r);
    x->channel     = wh__rd_u8(&r);
    x->rssi        = (int16_t)wh__rd_u16(&r);
    x->snr_q4      = (int16_t)wh__rd_u16(&r);
    x->portnum     = wh__rd_u16(&r);
    x->payload_len = wh__rd_u16(&r);
    if (r.err || x->payload_len > WH_LORA_MAX_PAYLOAD) return WH_ERR_SHORT;
    wh__rd_bytes(&r, x->payload, x->payload_len);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

int wh_enc_logrec(const wh_logrec_msg *m, uint8_t *out, size_t cap)
{
    if (!m || !out) return WH_ERR_ARG;
    if (m->len > WH_LOGREC_MAX) return WH_ERR_ARG;
    wh__wr w; wh__wr_init(&w, out, cap);
    wh__wr_u8(&w, m->fmt);
    wh__wr_u16(&w, m->len);
    wh__wr_bytes(&w, m->data, m->len);
    return w.err ? WH_ERR_SHORT : (int)w.off;
}
int wh_dec_logrec(const uint8_t *in, size_t len, wh_logrec_msg *m)
{
    if (!in || !m) return WH_ERR_ARG;
    memset(m, 0, sizeof(*m));
    wh__rd r; wh__rd_init(&r, in, len);
    m->fmt = wh__rd_u8(&r);
    m->len = wh__rd_u16(&r);
    if (r.err || m->len > WH_LOGREC_MAX) return WH_ERR_SHORT;
    wh__rd_bytes(&r, m->data, m->len);
    return r.err ? WH_ERR_SHORT : WH_OK;
}

/* ---- reference crypto adapter ----------------------------------------- */
static const wh_crypto_if wh__ref = {
    whc_x25519,
    whc_x25519_base,
    whc_hkdf,
    whc_sha256,
    whc_hmac_sha256,
    whc_chacha20poly1305_seal,
    whc_chacha20poly1305_open
};
const wh_crypto_if *wh_ref_crypto(void) { return &wh__ref; }

/* ---- replay window ---------------------------------------------------- */
static int wh__replay_check(const wh_replay *w, uint32_t seq)
{
    if (!w->seen) return WH_OK;
    if (seq > w->max) return WH_OK;
    uint32_t diff = w->max - seq;
    if (diff >= 64) return WH_ERR_REPLAY;      /* too old */
    if (w->bits & (1ull << diff)) return WH_ERR_REPLAY; /* duplicate */
    return WH_OK;
}
static void wh__replay_mark(wh_replay *w, uint32_t seq)
{
    if (!w->seen) { w->max = seq; w->bits = 1; w->seen = 1; return; }
    if (seq > w->max) {
        uint32_t shift = seq - w->max;
        w->bits = (shift >= 64) ? 0 : (w->bits << shift);
        w->bits |= 1;
        w->max = seq;
    } else {
        uint32_t diff = w->max - seq;
        if (diff < 64) w->bits |= (1ull << diff);
    }
}

/* ---- nonce construction ----------------------------------------------- */
static void wh__build_nonce(const uint8_t prefix[WH_NONCE_PREFIX_LEN],
                            uint32_t ctr, uint8_t out[WH_NONCE_LEN])
{
    memcpy(out, prefix, WH_NONCE_PREFIX_LEN);
    out[8]  = (uint8_t)(ctr);
    out[9]  = (uint8_t)(ctr >> 8);
    out[10] = (uint8_t)(ctr >> 16);
    out[11] = (uint8_t)(ctr >> 24);
}

/* ---- misc ------------------------------------------------------------- */
int wh_type_is_handshake(uint8_t type)
{
    switch (type) {
        case WH_MSG_ANNOUNCE:
        case WH_MSG_PAIR_REQ:
        case WH_MSG_PAIR_RESP:
        case WH_MSG_PAIR_NA:
        case WH_MSG_PAIR_NB:
            return 1;
        default:
            return 0;
    }
}

const char *wh_strerr(int e)
{
    switch (e) {
        case WH_OK:            return "ok";
        case WH_ERR_ARG:       return "bad argument";
        case WH_ERR_SHORT:     return "buffer too short";
        case WH_ERR_MAGIC:     return "bad magic";
        case WH_ERR_VERSION:   return "bad version";
        case WH_ERR_RESERVED:  return "reserved field nonzero";
        case WH_ERR_LEN:       return "length inconsistent";
        case WH_ERR_NOT_READY: return "session not established";
        case WH_ERR_TOO_BIG:   return "message too big";
        case WH_ERR_AUTH:      return "authentication failed";
        case WH_ERR_REPLAY:    return "replay rejected";
        case WH_ERR_STATE:     return "bad pairing state";
        case WH_ERR_COMMIT:    return "commitment mismatch";
        case WH_ERR_REASM:     return "reassembly error";
        case WH_ERR_NOMEM:     return "out of memory";
        case WH_ERR_TRANSPORT: return "transport error";
        default:               return "unknown error";
    }
}

/* ---- context ---------------------------------------------------------- */
int wh_ctx_init(wh_ctx *ctx, const wh_config *cfg)
{
    if (!ctx || !cfg) return WH_ERR_ARG;
    if (!cfg->rng || !cfg->transport_send) return WH_ERR_ARG;
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;
    ctx->crypto = cfg->crypto ? cfg->crypto : wh_ref_crypto();
    ctx->next_msg_id = 1;
    ctx->pair.state = WH_PS_IDLE;
    ctx->sess.established = 0;
    return WH_OK;
}

void wh_ctx_set_peer(wh_ctx *ctx, uint16_t peer_id) { if (ctx) ctx->cfg.peer_id = peer_id; }
int  wh_is_established(const wh_ctx *ctx) { return ctx && ctx->sess.established; }

int wh_session_export(const wh_ctx *ctx, uint8_t *buf, size_t cap)
{
    if (!ctx || !buf) return WH_ERR_ARG;
    if (!ctx->sess.established) return WH_ERR_STATE;      /* nothing to persist */
    if (cap < WH_SESSION_BLOB_LEN) return WH_ERR_SHORT;
    wh__wr w; wh__wr_init(&w, buf, cap);
    wh__wr_u8(&w, 'W'); wh__wr_u8(&w, 'H'); wh__wr_u8(&w, 'S'); wh__wr_u8(&w, 1); /* magic + ver */
    wh__wr_u16(&w, ctx->cfg.peer_id);
    wh__wr_bytes(&w, ctx->sess.tx_key, WH_KEY_LEN);
    wh__wr_bytes(&w, ctx->sess.rx_key, WH_KEY_LEN);
    wh__wr_bytes(&w, ctx->sess.tx_prefix, WH_NONCE_PREFIX_LEN);
    wh__wr_bytes(&w, ctx->sess.rx_prefix, WH_NONCE_PREFIX_LEN);
    wh__wr_u32(&w, ctx->sess.tx_ctr);                    /* pre-jump value */
    wh__wr_u32(&w, ctx->sess.rx_replay.max);
    wh__wr_u8(&w,  ctx->sess.rx_replay.seen);
    return w.err ? WH_ERR_SHORT : (int)w.off;            /* == WH_SESSION_BLOB_LEN */
}

int wh_session_import(wh_ctx *ctx, const uint8_t *buf, size_t len)
{
    if (!ctx || !buf) return WH_ERR_ARG;
    if (len < WH_SESSION_BLOB_LEN) return WH_ERR_SHORT;
    wh__rd r; wh__rd_init(&r, buf, len);
    uint8_t m0 = wh__rd_u8(&r), m1 = wh__rd_u8(&r), m2 = wh__rd_u8(&r), ver = wh__rd_u8(&r);
    if (m0 != 'W' || m1 != 'H' || m2 != 'S' || ver != 1) return WH_ERR_ARG;
    uint16_t peer = wh__rd_u16(&r);
    uint8_t tx_key[WH_KEY_LEN], rx_key[WH_KEY_LEN];
    uint8_t tx_pfx[WH_NONCE_PREFIX_LEN], rx_pfx[WH_NONCE_PREFIX_LEN];
    wh__rd_bytes(&r, tx_key, WH_KEY_LEN);
    wh__rd_bytes(&r, rx_key, WH_KEY_LEN);
    wh__rd_bytes(&r, tx_pfx, WH_NONCE_PREFIX_LEN);
    wh__rd_bytes(&r, rx_pfx, WH_NONCE_PREFIX_LEN);
    uint32_t stored_ctr  = wh__rd_u32(&r);
    uint32_t replay_max  = wh__rd_u32(&r);
    uint8_t  replay_seen = wh__rd_u8(&r);
    if (r.err) return WH_ERR_SHORT;
    /* Counter-wrap guard: if the jumped counter could approach the u32 max, refuse
     * and force a re-pair (fresh keys = fresh nonce space). ~1M reboots away. */
    if (stored_ctr > (uint32_t)(0xFFFFFFFFu - 2u * WH_PERSIST_MARGIN)) return WH_ERR_STATE;
    memcpy(ctx->sess.tx_key,    tx_key, WH_KEY_LEN);
    memcpy(ctx->sess.rx_key,    rx_key, WH_KEY_LEN);
    memcpy(ctx->sess.tx_prefix, tx_pfx, WH_NONCE_PREFIX_LEN);
    memcpy(ctx->sess.rx_prefix, rx_pfx, WH_NONCE_PREFIX_LEN);
    ctx->sess.tx_ctr         = stored_ctr + WH_PERSIST_MARGIN;   /* forward jump: no reuse */
    ctx->sess.rx_replay.max  = replay_max;
    ctx->sess.rx_replay.bits = 0;                               /* not stored; reset */
    ctx->sess.rx_replay.seen = replay_seen;
    ctx->sess.established     = 1;
    ctx->cfg.peer_id         = peer;
    ctx->pair.state          = WH_PS_ESTABLISHED;               /* consistent post-pair state */
    return WH_OK;
}

static uint32_t wh__now(wh_ctx *ctx) { return ctx->cfg.now_ms ? ctx->cfg.now_ms(ctx->cfg.user) : 0; }

/* Build one frame (header + optional-sealed payload) and hand to transport. */
static int wh__emit(wh_ctx *ctx, uint8_t type, uint8_t flags, uint16_t corr,
                    uint16_t msg_id, uint8_t frag_index, uint8_t frag_total,
                    const uint8_t *payload, uint16_t plen, int enc)
{
    uint8_t frame[WH_MAX_FRAME];
    wh_hdr h;
    memset(&h, 0, sizeof(h));
    h.version = WH_PROTO_VERSION;
    h.type = type;
    h.flags = (uint8_t)(flags | (enc ? WH_FLAG_ENC : 0));
    h.src_id = ctx->cfg.local_id;
    h.dst_id = ctx->cfg.peer_id;
    h.corr = corr;
    h.msg_id = msg_id;
    h.frag_index = frag_index;
    h.frag_total = frag_total;
    h.payload_len = plen;
    h.seq = enc ? ctx->sess.tx_ctr : 0; /* plaintext handshake: seq=0 */

    int hl = wh_hdr_encode(&h, frame, sizeof(frame));
    if (hl < 0) return hl;

    if (enc) {
        if (!ctx->sess.established) return WH_ERR_NOT_READY;
        uint8_t nonce[WH_NONCE_LEN];
        wh__build_nonce(ctx->sess.tx_prefix, ctx->sess.tx_ctr, nonce);
        if ((size_t)hl + plen + WH_TAG_LEN > sizeof(frame)) return WH_ERR_TOO_BIG;
        uint8_t *ct = frame + hl;
        uint8_t *tag = frame + hl + plen;
        ctx->crypto->aead_seal(ctx->sess.tx_key, nonce, frame, (size_t)hl,
                               payload, plen, ct, tag);
        ctx->sess.tx_ctr++;
        size_t flen = (size_t)hl + plen + WH_TAG_LEN;
        return ctx->cfg.transport_send(ctx->cfg.user, frame, flen) == 0 ? WH_OK : WH_ERR_TRANSPORT;
    } else {
        if ((size_t)hl + plen > sizeof(frame)) return WH_ERR_TOO_BIG;
        if (plen) memcpy(frame + hl, payload, plen);
        size_t flen = (size_t)hl + plen;
        return ctx->cfg.transport_send(ctx->cfg.user, frame, flen) == 0 ? WH_OK : WH_ERR_TRANSPORT;
    }
}

int wh_send(wh_ctx *ctx, uint8_t type, uint8_t flags, uint16_t corr,
            const uint8_t *payload, size_t len)
{
    if (!ctx) return WH_ERR_ARG;
    if (len && !payload) return WH_ERR_ARG;

    int handshake = wh_type_is_handshake(type);
    int enc = (!handshake) && ctx->sess.established;
    if (!handshake && !ctx->sess.established) return WH_ERR_NOT_READY;

    uint16_t cap = (uint16_t)(enc ? WH_FRAG_CAP_ENC : WH_FRAG_CAP_PLAIN);
    uint16_t msg_id = ctx->next_msg_id++;

    if (len <= cap) {
        return wh__emit(ctx, type, flags, corr, msg_id, 0, 1, payload, (uint16_t)len, enc);
    }

    /* fragment */
    size_t nfull = len / cap;
    size_t rem = len % cap;
    size_t nfrag = nfull + (rem ? 1 : 0);
    if (nfrag > WH_MAX_FRAGS) return WH_ERR_TOO_BIG;

    size_t off = 0;
    for (size_t i = 0; i < nfrag; i++) {
        uint16_t chunk = (uint16_t)((i + 1 < nfrag) ? cap : (len - off));
        int r = wh__emit(ctx, type, flags, corr, msg_id, (uint8_t)i, (uint8_t)nfrag,
                         payload + off, chunk, enc);
        if (r != WH_OK) return r;
        off += chunk;
    }
    return WH_OK;
}

int wh_announce_send(wh_ctx *ctx, uint8_t battery_pct, uint8_t flags)
{
    if (!ctx) return WH_ERR_ARG;
    wh_announce a;
    memset(&a, 0, sizeof(a));
    a.proto_version = WH_PROTO_VERSION;
    a.role = (uint8_t)ctx->cfg.role;
    a.caps = ctx->cfg.caps;
    a.battery_pct = battery_pct;
    a.flags = flags;
    a.short_id = ctx->cfg.local_id;
    memcpy(a.nick, ctx->cfg.nick, WH_NICK_MAX);
    a.nick[WH_NICK_MAX - 1] = '\0';
    uint8_t buf[64];
    int n = wh_enc_announce(&a, buf, sizeof(buf));
    if (n < 0) return n;

    /* ANNOUNCE is broadcast regardless of configured peer */
    uint16_t saved = ctx->cfg.peer_id;
    ctx->cfg.peer_id = WH_ID_BROADCAST;
    int r = wh__emit(ctx, WH_MSG_ANNOUNCE, WH_FLAG_EVENT, 0, ctx->next_msg_id++,
                     0, 1, buf, (uint16_t)n, 0);
    ctx->cfg.peer_id = saved;
    return r;
}

/* ---- pairing ---------------------------------------------------------- */
static void wh__pair_commit(const wh_crypto_if *cr,
                            const uint8_t n_r[WH_PAIR_NONCE_LEN],
                            const uint8_t pk_r[WH_X25519_LEN],
                            const uint8_t pk_i[WH_X25519_LEN],
                            uint8_t out[WH_COMMIT_LEN])
{
    uint8_t msg[6 + WH_X25519_LEN * 2];
    memcpy(msg, "wh-cmt", 6);
    memcpy(msg + 6, pk_r, WH_X25519_LEN);
    memcpy(msg + 6 + WH_X25519_LEN, pk_i, WH_X25519_LEN);
    uint8_t mac[32];
    cr->hmac_sha256(n_r, WH_PAIR_NONCE_LEN, msg, sizeof(msg), mac);
    memcpy(out, mac, WH_COMMIT_LEN);
}

static void wh__pair_sas(const wh_crypto_if *cr,
                         const uint8_t pk_i[WH_X25519_LEN],
                         const uint8_t pk_r[WH_X25519_LEN],
                         const uint8_t n_i[WH_PAIR_NONCE_LEN],
                         const uint8_t n_r[WH_PAIR_NONCE_LEN],
                         char out[WH_SAS_DIGITS + 1])
{
    uint8_t msg[6 + WH_X25519_LEN * 2 + WH_PAIR_NONCE_LEN * 2];
    size_t o = 0;
    memcpy(msg + o, "wh-sas", 6); o += 6;
    memcpy(msg + o, pk_i, WH_X25519_LEN); o += WH_X25519_LEN;
    memcpy(msg + o, pk_r, WH_X25519_LEN); o += WH_X25519_LEN;
    memcpy(msg + o, n_i, WH_PAIR_NONCE_LEN); o += WH_PAIR_NONCE_LEN;
    memcpy(msg + o, n_r, WH_PAIR_NONCE_LEN); o += WH_PAIR_NONCE_LEN;
    uint8_t h[32];
    cr->sha256(msg, o, h);
    uint32_t v = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                 ((uint32_t)h[2] << 8) | (uint32_t)h[3];
    v %= 1000000u;
    for (int i = WH_SAS_DIGITS - 1; i >= 0; i--) { out[i] = (char)('0' + (v % 10)); v /= 10; }
    out[WH_SAS_DIGITS] = '\0';
}

/* Derive the session keys from the completed handshake. */
static void wh__session_derive(wh_ctx *ctx)
{
    const wh_crypto_if *cr = ctx->crypto;
    wh_pair *p = &ctx->pair;

    /* shared = X25519(sk_self, peer ephemeral) */
    const uint8_t *peer_pk = p->is_initiator ? p->pk_r : p->pk_i;
    cr->x25519(p->shared, p->sk, peer_pk);

    /* salt = Ni || Nr ; info = "wh-link/v1 session" || PKi || PKr */
    uint8_t salt[WH_PAIR_NONCE_LEN * 2];
    memcpy(salt, p->n_i, WH_PAIR_NONCE_LEN);
    memcpy(salt + WH_PAIR_NONCE_LEN, p->n_r, WH_PAIR_NONCE_LEN);

    uint8_t info[18 + WH_X25519_LEN * 2];
    memcpy(info, "wh-link/v1 session", 18);
    memcpy(info + 18, p->pk_i, WH_X25519_LEN);
    memcpy(info + 18 + WH_X25519_LEN, p->pk_r, WH_X25519_LEN);

    uint8_t okm[WH_KEY_LEN * 2 + WH_NONCE_PREFIX_LEN * 2]; /* 80 */
    cr->hkdf(salt, sizeof(salt), p->shared, WH_X25519_LEN, info, sizeof(info),
             okm, sizeof(okm));

    const uint8_t *key_i2r = okm;
    const uint8_t *key_r2i = okm + WH_KEY_LEN;
    const uint8_t *np_i2r  = okm + WH_KEY_LEN * 2;
    const uint8_t *np_r2i  = okm + WH_KEY_LEN * 2 + WH_NONCE_PREFIX_LEN;

    if (p->is_initiator) {
        memcpy(ctx->sess.tx_key, key_i2r, WH_KEY_LEN);
        memcpy(ctx->sess.rx_key, key_r2i, WH_KEY_LEN);
        memcpy(ctx->sess.tx_prefix, np_i2r, WH_NONCE_PREFIX_LEN);
        memcpy(ctx->sess.rx_prefix, np_r2i, WH_NONCE_PREFIX_LEN);
    } else {
        memcpy(ctx->sess.tx_key, key_r2i, WH_KEY_LEN);
        memcpy(ctx->sess.rx_key, key_i2r, WH_KEY_LEN);
        memcpy(ctx->sess.tx_prefix, np_r2i, WH_NONCE_PREFIX_LEN);
        memcpy(ctx->sess.rx_prefix, np_i2r, WH_NONCE_PREFIX_LEN);
    }
    ctx->sess.tx_ctr = 0;
    memset(&ctx->sess.rx_replay, 0, sizeof(ctx->sess.rx_replay));
    ctx->sess.established = 1;

    whc_wipe(okm, sizeof(okm));
    whc_wipe(salt, sizeof(salt));
}

int wh_pair_start(wh_ctx *ctx)
{
    if (!ctx) return WH_ERR_ARG;
    /* The initiator legitimately may NOT know the peer id yet: SAS pairing exists
     * precisely to bootstrap trust with an as-yet-unauthenticated device. So a
     * broadcast peer is allowed here — PAIR_REQ goes out with dst = BROADCAST, the
     * responder learns our id from the frame's src, and we learn the responder's
     * id from the PAIR_RESP header (the app's frame handler sets the peer before
     * the session is established). On point-to-point BLE the frame reaches the one
     * connected peer; on a shared bus, the responder is whoever answers.
     *
     * (Previously this rejected peer_id == BROADCAST, which deadlocked real
     * pairing: the initiator sends first, so it can never have learned the peer
     * yet. The protocol tests missed it because they pre-set peer_id.) */
    wh_pair *p = &ctx->pair;
    memset(p, 0, sizeof(*p));
    p->is_initiator = 1;
    ctx->cfg.rng(ctx->cfg.user, p->sk, WH_X25519_LEN);
    ctx->crypto->x25519_base(p->pk_i, p->sk);
    ctx->cfg.rng(ctx->cfg.user, p->n_i, WH_PAIR_NONCE_LEN);
    p->state = WH_PS_I_SENT_REQ;
    /* PAIR_REQ payload = PKi */
    return wh__emit(ctx, WH_MSG_PAIR_REQ, 0, 0, ctx->next_msg_id++, 0, 1,
                    p->pk_i, WH_X25519_LEN, 0);
}

static int wh__pair_on_msg(wh_ctx *ctx, uint8_t type, const uint8_t *pl, size_t len)
{
    wh_pair *p = &ctx->pair;
    const wh_crypto_if *cr = ctx->crypto;

    switch (type) {
    case WH_MSG_PAIR_REQ: {
        /* responder path: receive PKi */
        if (len < WH_X25519_LEN) return WH_ERR_SHORT;
        memset(p, 0, sizeof(*p));
        p->is_initiator = 0;
        memcpy(p->pk_i, pl, WH_X25519_LEN);
        cr->x25519_base(p->pk_r, (ctx->cfg.rng(ctx->cfg.user, p->sk, WH_X25519_LEN), p->sk));
        ctx->cfg.rng(ctx->cfg.user, p->n_r, WH_PAIR_NONCE_LEN);
        uint8_t commit[WH_COMMIT_LEN];
        wh__pair_commit(cr, p->n_r, p->pk_r, p->pk_i, commit);
        p->state = WH_PS_R_SENT_RESP;
        /* PAIR_RESP payload = PKr || Cr */
        uint8_t buf[WH_X25519_LEN + WH_COMMIT_LEN];
        memcpy(buf, p->pk_r, WH_X25519_LEN);
        memcpy(buf + WH_X25519_LEN, commit, WH_COMMIT_LEN);
        return wh__emit(ctx, WH_MSG_PAIR_RESP, 0, 0, ctx->next_msg_id++, 0, 1,
                        buf, sizeof(buf), 0);
    }
    case WH_MSG_PAIR_RESP: {
        /* initiator path: receive PKr || Cr */
        if (p->state != WH_PS_I_SENT_REQ) return WH_ERR_STATE;
        if (len < WH_X25519_LEN + WH_COMMIT_LEN) return WH_ERR_SHORT;
        memcpy(p->pk_r, pl, WH_X25519_LEN);
        memcpy(p->commit, pl + WH_X25519_LEN, WH_COMMIT_LEN);
        p->state = WH_PS_I_SENT_NA;
        /* PAIR_NA payload = Ni */
        return wh__emit(ctx, WH_MSG_PAIR_NA, 0, 0, ctx->next_msg_id++, 0, 1,
                        p->n_i, WH_PAIR_NONCE_LEN, 0);
    }
    case WH_MSG_PAIR_NA: {
        /* responder path: receive Ni, reveal Nr, compute SAS */
        if (p->state != WH_PS_R_SENT_RESP) return WH_ERR_STATE;
        if (len < WH_PAIR_NONCE_LEN) return WH_ERR_SHORT;
        memcpy(p->n_i, pl, WH_PAIR_NONCE_LEN);
        int r = wh__emit(ctx, WH_MSG_PAIR_NB, 0, 0, ctx->next_msg_id++, 0, 1,
                         p->n_r, WH_PAIR_NONCE_LEN, 0);
        if (r != WH_OK) return r;
        wh__pair_sas(cr, p->pk_i, p->pk_r, p->n_i, p->n_r, p->sas);
        p->state = WH_PS_AWAIT_SAS;
        if (ctx->cfg.on_sas) ctx->cfg.on_sas(ctx->cfg.user, p->sas);
        return WH_OK;
    }
    case WH_MSG_PAIR_NB: {
        /* initiator path: receive Nr, verify commitment, compute SAS */
        if (p->state != WH_PS_I_SENT_NA) return WH_ERR_STATE;
        if (len < WH_PAIR_NONCE_LEN) return WH_ERR_SHORT;
        memcpy(p->n_r, pl, WH_PAIR_NONCE_LEN);
        uint8_t expect[WH_COMMIT_LEN];
        wh__pair_commit(cr, p->n_r, p->pk_r, p->pk_i, expect);
        if (whc_memeq_ct(expect, p->commit, WH_COMMIT_LEN) != 0) {
            p->state = WH_PS_FAILED;
            if (ctx->cfg.on_paired) ctx->cfg.on_paired(ctx->cfg.user, 0);
            return WH_ERR_COMMIT;
        }
        wh__pair_sas(cr, p->pk_i, p->pk_r, p->n_i, p->n_r, p->sas);
        p->state = WH_PS_AWAIT_SAS;
        if (ctx->cfg.on_sas) ctx->cfg.on_sas(ctx->cfg.user, p->sas);
        return WH_OK;
    }
    default:
        return WH_ERR_STATE;
    }
}

int wh_pair_confirm(wh_ctx *ctx, int matched)
{
    if (!ctx) return WH_ERR_ARG;
    if (ctx->pair.state != WH_PS_AWAIT_SAS) return WH_ERR_STATE;
    if (!matched) {
        ctx->pair.state = WH_PS_FAILED;
        if (ctx->cfg.on_paired) ctx->cfg.on_paired(ctx->cfg.user, 0);
        return WH_OK;
    }
    wh__session_derive(ctx);
    ctx->pair.state = WH_PS_ESTABLISHED;
    if (ctx->cfg.on_paired) ctx->cfg.on_paired(ctx->cfg.user, 1);
    return WH_OK;
}

const char *wh_pair_sas(const wh_ctx *ctx)
{
    return (ctx && ctx->pair.state == WH_PS_AWAIT_SAS) ? ctx->pair.sas : "";
}

void wh_unpair(wh_ctx *ctx)
{
    if (!ctx) return;
    whc_wipe(&ctx->sess, sizeof(ctx->sess));
    whc_wipe(&ctx->pair, sizeof(ctx->pair));
    ctx->sess.established = 0;
    ctx->pair.state = WH_PS_IDLE;
    memset(&ctx->reasm, 0, sizeof(ctx->reasm));
}

/* ---- reassembly + inbound dispatch ------------------------------------ */
static int wh__mask_test(const uint32_t mask[8], unsigned i) { return (mask[i >> 5] >> (i & 31)) & 1u; }
static void wh__mask_set(uint32_t mask[8], unsigned i) { mask[i >> 5] |= (1u << (i & 31)); }

static void wh__deliver(wh_ctx *ctx, const wh_hdr *h, const uint8_t *pl, size_t len)
{
    if (wh_type_is_handshake(h->type)) {
        (void)wh__pair_on_msg(ctx, h->type, pl, len);
        return;
    }
    if (ctx->cfg.on_message) {
        wh_msg m;
        m.type = h->type;
        m.flags = h->flags;
        m.corr = h->corr;
        m.src_id = h->src_id;
        m.payload = pl;
        m.len = len;
        ctx->cfg.on_message(ctx->cfg.user, &m);
    }
}

int wh_on_frame(wh_ctx *ctx, const uint8_t *frame, size_t len)
{
    if (!ctx || !frame) return WH_ERR_ARG;
    wh_hdr h;
    int r = wh_hdr_decode(frame, len, &h);
    if (r != WH_OK) return r;

    int enc = (h.flags & WH_FLAG_ENC) != 0;
    size_t need = WH_HDR_LEN + h.payload_len + (enc ? WH_TAG_LEN : 0);
    if (len < need) return WH_ERR_LEN;

    /* plaintext payload buffer (post-decrypt if enc) */
    uint8_t pt[WH_FRAG_CAP_PLAIN];
    const uint8_t *payload;
    uint16_t plen = h.payload_len;

    if (enc) {
        if (!ctx->sess.established) return WH_ERR_NOT_READY;
        if (plen > WH_FRAG_CAP_ENC) return WH_ERR_LEN;
        /* pre-check replay (cheap reject) before doing AEAD */
        if (wh__replay_check(&ctx->sess.rx_replay, h.seq) != WH_OK) return WH_ERR_REPLAY;
        uint8_t nonce[WH_NONCE_LEN];
        wh__build_nonce(ctx->sess.rx_prefix, h.seq, nonce);
        const uint8_t *ct = frame + WH_HDR_LEN;
        const uint8_t *tag = frame + WH_HDR_LEN + plen;
        int o = ctx->crypto->aead_open(ctx->sess.rx_key, nonce, frame, WH_HDR_LEN,
                                       ct, plen, tag, pt);
        if (o != 0) return WH_ERR_AUTH;
        wh__replay_mark(&ctx->sess.rx_replay, h.seq); /* only after auth */
        payload = pt;
    } else {
        payload = frame + WH_HDR_LEN;
    }

    /* unfragmented fast path */
    if (h.frag_total == 1) {
        wh__deliver(ctx, &h, payload, plen);
        return WH_OK;
    }

    /* fragmented: offset-addressed reassembly */
    uint16_t cap = (uint16_t)(enc ? WH_FRAG_CAP_ENC : WH_FRAG_CAP_PLAIN);
    int is_final = (h.frag_index + 1 == h.frag_total);
    /* non-final fragments must be exactly cap bytes */
    if (!is_final && plen != cap) return WH_ERR_REASM;
    if (is_final && plen == 0) return WH_ERR_REASM;

    /* new message? (different src/msg_id, or none active) */
    if (!ctx->reasm.active ||
        ctx->reasm.src_id != h.src_id ||
        ctx->reasm.msg_id != h.msg_id) {
        memset(&ctx->reasm, 0, sizeof(ctx->reasm));
        ctx->reasm.active = 1;
        ctx->reasm.src_id = h.src_id;
        ctx->reasm.msg_id = h.msg_id;
        ctx->reasm.frag_total = h.frag_total;
        ctx->reasm.cap = cap;
        ctx->reasm.start_ms = wh__now(ctx);
    }
    if (ctx->reasm.frag_total != h.frag_total || ctx->reasm.cap != cap) return WH_ERR_REASM;

    size_t offset = (size_t)h.frag_index * cap;
    if (offset + plen > WH_REASM_MAX) { ctx->reasm.active = 0; return WH_ERR_TOO_BIG; }

    if (!wh__mask_test(ctx->reasm.recv_mask, h.frag_index)) {
        memcpy(ctx->reasm.buf + offset, payload, plen);
        wh__mask_set(ctx->reasm.recv_mask, h.frag_index);
        ctx->reasm.recv_count++;
        if (is_final) {
            ctx->reasm.have_final = 1;
            ctx->reasm.total_len = (uint16_t)(offset + plen);
        }
    }

    if (ctx->reasm.have_final && ctx->reasm.recv_count == ctx->reasm.frag_total) {
        wh_hdr hh = h;
        hh.frag_index = 0;
        hh.frag_total = 1;
        uint16_t tot = ctx->reasm.total_len;
        ctx->reasm.active = 0;
        wh__deliver(ctx, &hh, ctx->reasm.buf, tot);
    }
    return WH_OK;
}

#endif /* WH_LINK_IMPLEMENTATION */
#endif /* WH_LINK_H */
